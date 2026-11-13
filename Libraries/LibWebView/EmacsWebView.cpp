/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibCore/BrowserVersion.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibWebCommon/Clipboard/SystemClipboard.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/EmacsProtocol.h>
#include <LibWebView/EmacsServer.h>
#include <LibWebView/EmacsTransport.h>
#include <LibWebView/EmacsWebView.h>
#include <LibWebView/PageBackingStores.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentPage.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace WebView {

using Web::UIEvents::KeyCode;
using Web::UIEvents::KeyModifier;
using Web::UIEvents::MouseButton;

// The most pixel bytes one frame record carries. A larger rectangle goes out as bands of rows.
static constexpr size_t maximum_pixel_bytes_per_record = EmacsProtocol::maximum_record_length - 1 - EmacsProtocol::frame_header_size;
// A smaller rectangle than this is sent as it is, since compressing it saves little.
static constexpr size_t minimum_pixel_bytes_to_compress = 16 * KiB;

using EmacsProtocol::maximum_device_pixel_ratio;
using EmacsProtocol::maximum_text_bytes;
using EmacsProtocol::maximum_viewport_area;
using EmacsProtocol::maximum_viewport_dimension;
using EmacsProtocol::minimum_device_pixel_ratio;
using EmacsProtocol::Viewport;
using EmacsProtocol::viewport_from;

static constexpr double maximum_wheel_delta = 1'000'000;
static constexpr int maximum_history_delta = 10'000;

static constexpr size_t maximum_favicon_bytes = 1 * MiB;

static constexpr auto popup_url_timeout = AK::Duration::from_seconds(2);

// These make text fit for the wire: valid UTF-8, with a replacement character for what is not, of a bounded length.
static String wire_text(StringView text)
{
    return EmacsProtocol::wire_text(text);
}

static String wire_text(String const& text)
{
    return wire_text(text.bytes_as_string_view());
}

static String wire_text(ByteString const& text)
{
    return wire_text(text.view());
}

static String wire_text(Utf16View const& text)
{
    // NB: A code unit takes at least one byte of UTF-8.
    auto text_to_convert = text;
    if (text_to_convert.length_in_code_units() > maximum_text_bytes)
        text_to_convert = text_to_convert.substring_view(0, maximum_text_bytes);
    auto converted = String::from_utf16_with_replacement_character(text_to_convert);
    if (converted.is_error())
        return {};
    return wire_text(converted.value().bytes_as_string_view());
}

static String wire_text(Utf16String const& text)
{
    return wire_text(text.utf16_view());
}

static String wire_text(URL::URL const& url)
{
    return wire_text(url.serialize().bytes_as_string_view());
}

template<typename... Parameters>
static String wire_formatted(CheckedFormatString<Parameters...>&& format, Parameters const&... parameters)
{
    StringBuilder builder;
    builder.appendff(move(format), parameters...);
    return wire_text(builder.string_view());
}

static Optional<double> number(JsonObject const& object, StringView key)
{
    auto value = object.get(key);
    if (!value.has_value() || !value->is_number())
        return {};
    return value->get_double_with_precision_loss();
}

static int integer(JsonObject const& object, StringView key, int fallback = 0)
{
    auto value = number(object, key);
    if (!value.has_value() || !isfinite(*value))
        return fallback;
    return static_cast<int>(clamp(round(*value), -2147483647.0, 2147483647.0));
}

static JsonValue string_or_null(Optional<String> const& string)
{
    if (string.has_value())
        return wire_text(*string);
    return JsonValue {};
}

static JsonValue echoed(JsonObject const& object, StringView key)
{
    if (auto value = object.get(key); value.has_value())
        return *value;
    return JsonValue {};
}

// Writes a file that only this user reads. Without a path, the file gets a name in /tmp, where anyone can put a file or
// a link at a name, so that one is a file this process creates.
// Without a path the file is <name><suffix> in EmacsTransport::private_directory(), or where there is none or it cannot
// be made there, /tmp/<name>-XXXXXX<suffix> with XXXXXX chosen at random: anyone can put a file or a link at a name
// in /tmp.
static ErrorOr<ByteString> write_private_file(Optional<ByteString> const& path, StringView default_name, StringView suffix, ReadonlyBytes contents)
{
    auto fd = -1;
    auto written_path = path.value_or({});
    if (path.has_value()) {
        fd = ::open(path->characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0)
            return Error::from_syscall("open"sv, errno);
    } else {
        if (auto directory = EmacsTransport::private_directory(); directory.has_value()) {
            written_path = ByteString::formatted("{}/{}{}", *directory, default_name, suffix);
            (void)::unlink(written_path.characters());
            fd = ::open(written_path.characters(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        }
        if (fd < 0) {
            auto pattern = ByteString::formatted("/tmp/{}-XXXXXX{}", default_name, suffix);
            Vector<char> name;
            name.append(pattern.characters(), pattern.length() + 1);
            fd = ::mkstemps(name.data(), static_cast<int>(suffix.length()));
            if (fd < 0)
                return Error::from_syscall("mkstemps"sv, errno);
            written_path = name.data();
            (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }

    auto file = TRY(Core::File::adopt_fd(fd, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(contents));
    return written_path;
}

NonnullOwnPtr<EmacsWebView> EmacsWebView::create(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, double device_pixel_ratio, EmacsServer& server, u32 id, Optional<CanonicalTraversable&> traversable)
{
    auto view = adopt_own(*new EmacsWebView(move(theme), viewport_size, device_pixel_ratio, server, id));
    // NB: A view renders only while a client is there to see it.
    view->initialize_tab(Web::HTML::VisibilityState::Hidden, traversable);
    view->set_presents_from_cpu_memory(true);
    return view;
}

EmacsWebView::EmacsWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, double device_pixel_ratio, EmacsServer& server, u32 id)
    : HeadlessWebView(move(theme), viewport_size)
    , m_server(server)
    , m_id(id)
{
    m_device_pixel_ratio = device_pixel_ratio;

    on_title_change = [this](auto const&) { schedule_state(); };
    on_url_change = [this](auto const&) { schedule_state(); };
    on_loading_state_change = [this](bool) { schedule_state(); };

    on_cursor_change = [this](Gfx::Cursor const& cursor) {
        auto cursor_message = message("cursor"sv);
        cursor_message.set("cursor"sv, EmacsProtocol::css_cursor_name(cursor));
        send_latest(move(cursor_message));
    };

    on_link_hover = [this](URL::URL const& url) {
        auto hover = message("link_hover"sv);
        hover.set("url"sv, wire_text(url));
        send_latest(move(hover));
    };
    on_link_unhover = [this] {
        auto hover = message("link_hover"sv);
        hover.set("url"sv, JsonValue {});
        send_latest(move(hover));
    };

    on_enter_tooltip_area = [this](ByteString const& text) {
        auto tooltip = message("tooltip"sv);
        tooltip.set("text"sv, wire_text(text));
        send_latest(move(tooltip));
    };
    on_leave_tooltip_area = [this] {
        auto tooltip = message("tooltip"sv);
        tooltip.set("text"sv, JsonValue {});
        send_latest(move(tooltip));
    };

    on_favicon_change = [this](Optional<Gfx::Bitmap const&> bitmap) {
        auto favicon = message("favicon"sv);
        favicon.set("png"sv, JsonValue {});
        if (bitmap.has_value()) {
            auto png = Gfx::PNGWriter::encode(*bitmap);
            if (png.is_error() || png.value().size() > maximum_favicon_bytes) {
                dbgln("Emacs: view {}: not sending a favicon that cannot be encoded or is too large", m_id);
            } else if (auto base64 = encode_base64(png.value()); !base64.is_error()) {
                favicon.set("png"sv, base64.release_value());
            }
        }
        send_latest(move(favicon));
    };

    on_page_background_color_change = [this](Gfx::Color color) {
        if (m_last_sent_background_color == color || !m_client_knows_view)
            return;
        m_last_sent_background_color = color;
        auto background = message("background_color"sv);
        background.set("rgb"sv, wire_formatted("#{:02x}{:02x}{:02x}", color.red(), color.green(), color.blue()));
        send_latest(move(background));
    };

    on_find_in_page = [this](size_t current_match_index, Optional<size_t> const& total_match_count) {
        auto result = message("find_result"sv);
        result.set("query"sv, wire_text(m_find_query));
        auto has_match = !total_match_count.has_value() || *total_match_count > 0;
        result.set("index"sv, static_cast<u64>(has_match ? current_match_index + 1 : 0));
        if (total_match_count.has_value())
            result.set("total"sv, static_cast<u64>(*total_match_count));
        else
            result.set("total"sv, JsonValue {});
        send(result);
    };

    on_input_method_state_change = [this] {
        auto const& state = input_method_state();
        auto ime = message("ime"sv);
        ime.set("enabled"sv, state.is_enabled);
        ime.set("activation"sv, state.has_transient_activation);
        if (state.caret_rect.has_value()) {
            JsonObject caret;
            caret.set("x"sv, state.caret_rect->x().value());
            caret.set("y"sv, state.caret_rect->y().value());
            caret.set("width"sv, state.caret_rect->width().value());
            caret.set("height"sv, state.caret_rect->height().value());
            ime.set("caret"sv, move(caret));
        } else {
            ime.set("caret"sv, JsonValue {});
        }

        auto serialized = ime.serialized();
        if (serialized == m_last_sent_input_method_state)
            return;
        m_last_sent_input_method_state = move(serialized);
        send(ime);
    };

    on_console_message = [this](ConsoleOutput output) {
        if (!m_console_enabled)
            return;

        auto console = message("console"sv);
        output.output.visit(
            [&](ConsoleLog const& log) {
                auto level = "log"sv;
                switch (log.level) {
                case JS::ConsoleLogLevel::Assert:
                case JS::ConsoleLogLevel::Error:
                    level = "error"sv;
                    break;
                case JS::ConsoleLogLevel::Warn:
                    level = "warn"sv;
                    break;
                case JS::ConsoleLogLevel::Info:
                    level = "info"sv;
                    break;
                case JS::ConsoleLogLevel::Debug:
                    level = "debug"sv;
                    break;
                default:
                    break;
                }

                StringBuilder text;
                for (auto const& argument : log.arguments) {
                    if (text.length() > maximum_text_bytes)
                        break;
                    if (!text.is_empty())
                        text.append(' ');
                    if (argument.is_string())
                        text.append(argument.as_string());
                    else
                        text.append(argument.serialized());
                }
                console.set("level"sv, level);
                console.set("text"sv, wire_text(text.string_view()));
            },
            [&](ConsoleError const& error) {
                console.set("level"sv, "error"sv);
                console.set("text"sv, wire_formatted("{}: {}", error.name, error.message));
            },
            [&](ConsoleTrace const& trace) {
                console.set("level"sv, "trace"sv);
                console.set("text"sv, wire_text(trace.label));
            },
            [](ConsoleGroupEnd const&) { });
        if (console.has("text"sv))
            send(console);
    };

    on_web_content_crashed = [this](WebContentCrashReason reason) {
        // Nothing is left of the page that asked.
        withdraw_page_requests();
        end_hint_session(HintSessionEnd::Cancelled);
        m_hint_activations.clear();
        m_hint_clicks.clear();
        if (m_pending_screenshot.has_value()) {
            m_pending_screenshot.clear();
            send_error("cannot take a screenshot: the page crashed"sv);
        }

        auto crashed = message("crashed"sv);
        crashed.set("reason"sv, reason == WebContentCrashReason::RejectedIPC ? "rejected_ipc"sv : "process_crash"sv);
        send(crashed);
        stop_sending_frame();
    };

    on_top_level_navigation_commit = [this] {
        // What a document asked is over once another document takes its place.
        auto document_id = traversable().active_document().id();
        if (m_active_document_id == document_id)
            return;
        m_active_document_id = document_id;
        withdraw_page_requests();
        end_hint_session(HintSessionEnd::Cancelled);
        m_hint_activations.clear();
        m_hint_clicks.clear();
    };

    on_close = [this] {
        close_for_page();
    };

    on_hints_collected = [this](WebContentPage& page, u64 request_id, JsonValue const& result) {
        did_collect_hints(page, request_id, result);
    };
    on_hint_activation_prepared = [this](WebContentPage& page, u64 request_id, JsonValue const& result) {
        did_prepare_hint_activation(page, request_id, result);
    };
    on_hint_activation_finished = [this](WebContentPage& page, u64 request_id, bool clicked) {
        did_finish_hint_activation(page, request_id, clicked);
    };

    on_new_web_view = [this](auto, auto, CanonicalTraversable& traversable) {
        if (auto* view = m_server.open_view({}, *this, traversable))
            return view->handle();
        // Otherwise the page gets the window it asked for, which nobody is shown and which closes once its URL is known.
        auto& popup = adopt_child_web_view(HeadlessWebView::create_child(*this, traversable));
        watch_blocked_popup(popup);
        return popup.handle();
    };

    on_request_alert = [this](Utf16String const& text) {
        JsonObject dialog;
        dialog.set("kind"sv, "alert"sv);
        dialog.set("message"sv, wire_text(text));
        dialog.set("default"sv, JsonValue {});
        dialog.set("origin"sv, wire_text(url().origin().serialize()));
        open_dialog(Web::PendingDialog::Alert, move(dialog));
    };

    on_request_confirm = [this](Utf16String const& text) {
        JsonObject dialog;
        dialog.set("kind"sv, "confirm"sv);
        dialog.set("message"sv, wire_text(text));
        dialog.set("default"sv, JsonValue {});
        dialog.set("origin"sv, wire_text(url().origin().serialize()));
        open_dialog(Web::PendingDialog::Confirm, move(dialog));
    };

    on_request_prompt = [this](Utf16String const& text, Utf16String const& default_text) {
        JsonObject dialog;
        dialog.set("kind"sv, "prompt"sv);
        dialog.set("message"sv, wire_text(text));
        dialog.set("default"sv, wire_text(default_text));
        dialog.set("origin"sv, wire_text(url().origin().serialize()));
        if (open_dialog(Web::PendingDialog::Prompt, move(dialog)))
            m_pending_prompt_text = default_text;
    };

    on_request_set_prompt_text = [this](Utf16String const& text) {
        m_pending_prompt_text = text;
        for (auto& request : m_open_requests) {
            if (request.kind != RequestKind::Dialog)
                continue;
            request.message.set("default"sv, wire_text(text));
            auto update = message("dialog_update"sv);
            update.set("id"sv, request.id);
            update.set("default"sv, wire_text(text));
            send(update);
        }
    };

    on_request_before_unload = [this](String const& source, Function<void(bool)> on_complete) {
        JsonObject dialog;
        dialog.set("kind"sv, "beforeunload"sv);
        dialog.set("message"sv, ""sv);
        dialog.set("default"sv, JsonValue {});
        dialog.set("origin"sv, wire_text(source));
        withdraw_requests(RequestKind::Dialog);
        auto id = open_request(RequestKind::Dialog, move(dialog));
        if (!id.has_value()) {
            on_complete(true);
            return;
        }
        m_before_unload = BeforeUnload { *id, move(on_complete) };
    };

    on_request_select_dropdown = [this](Gfx::IntPoint position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) {
        auto option_to_json = [](Web::HTML::SelectItemOption const& option, Optional<String> const& group) {
            JsonObject item;
            item.set("id"sv, option.id);
            item.set("label"sv, wire_text(option.label));
            item.set("selected"sv, option.selected);
            item.set("disabled"sv, option.disabled);
            item.set("group"sv, string_or_null(group));
            return item;
        };

        JsonArray json_items;
        for (auto const& item : items) {
            item.visit(
                [&](Web::HTML::SelectItemOption const& option) {
                    json_items.must_append(option_to_json(option, {}));
                },
                [&](Web::HTML::SelectItemOptionGroup const& group) {
                    auto label = wire_text(group.label);
                    for (auto const& option : group.items)
                        json_items.must_append(option_to_json(option, label));
                },
                [&](Web::HTML::SelectItemSeparator const&) {
                    JsonObject separator;
                    separator.set("separator"sv, true);
                    json_items.must_append(move(separator));
                });
        }

        JsonObject select;
        select.set("x"sv, position.x());
        select.set("y"sv, position.y());
        // NB: The width arrives in CSS pixels.
        select.set("min_width"sv, static_cast<i64>(round(minimum_width * m_device_pixel_ratio)));
        select.set("items"sv, move(json_items));
        withdraw_requests(RequestKind::Select);
        if (!open_request(RequestKind::Select, move(select)).has_value())
            select_dropdown_closed({});
    };

    on_request_file_picker = [this](Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files) {
        JsonArray accept;
        for (auto const& filter : accepted_file_types.filters) {
            filter.visit(
                [&](Web::HTML::FileFilter::FileType type) {
                    switch (type) {
                    case Web::HTML::FileFilter::FileType::Audio:
                        accept.must_append("audio/*"sv);
                        break;
                    case Web::HTML::FileFilter::FileType::Image:
                        accept.must_append("image/*"sv);
                        break;
                    case Web::HTML::FileFilter::FileType::Video:
                        accept.must_append("video/*"sv);
                        break;
                    }
                },
                [&](Web::HTML::FileFilter::MimeType const& mime_type) {
                    accept.must_append(wire_text(mime_type.value));
                },
                [&](Web::HTML::FileFilter::Extension const& extension) {
                    accept.must_append(wire_formatted(".{}", extension.value));
                });
        }

        JsonObject picker;
        picker.set("multiple"sv, allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes);
        picker.set("accept"sv, move(accept));
        picker.set("directory"sv, JsonValue {});
        withdraw_requests(RequestKind::FilePicker);
        if (!open_request(RequestKind::FilePicker, move(picker)).has_value())
            file_picker_closed({});
    };

    on_request_color_picker = [this](Color current_color) {
        JsonObject picker;
        picker.set("current"sv, wire_formatted("#{:02x}{:02x}{:02x}", current_color.red(), current_color.green(), current_color.blue()));
        withdraw_requests(RequestKind::ColorPicker);
        if (!open_request(RequestKind::ColorPicker, move(picker)).has_value())
            color_picker_update({}, Web::HTML::ColorPickerUpdateState::Closed);
    };

    on_request_date_picker = [this](Utf16String const& input_type, Utf16String const& value, Utf16String const& minimum, Utf16String const& maximum) {
        JsonObject picker;
        picker.set("input_type"sv, wire_text(input_type));
        picker.set("value"sv, wire_text(value));
        picker.set("min"sv, wire_text(minimum));
        picker.set("max"sv, wire_text(maximum));
        withdraw_requests(RequestKind::DatePicker);
        if (!open_request(RequestKind::DatePicker, move(picker)).has_value())
            date_picker_closed({});
    };

    on_request_permission = [this](PermissionPromptRequest const& request, PermissionPromptCallback on_outcome) {
        JsonObject permission;
        permission.set("name"sv, Web::Permissions::to_string(request.name));
        permission.set("origin"sv, wire_text(request.top_level_origin.serialize()));
        permission.set("is_file"sv, request.is_file);
        // A notice tells the user that the system keeps the feature from the browser. Its answer decides nothing.
        if (request.kind == PermissionPromptKind::SystemDeniedNotice)
            permission.set("kind"sv, "system_denied_notice"sv);

        auto id = open_request(RequestKind::Permission, move(permission));
        if (!id.has_value()) {
            on_outcome(Web::Permissions::PromptOutcome::Dismissed);
            return;
        }
        m_permission_callbacks.set(*id, move(on_outcome));
        m_permission_requests.set(*id, request);
        if (m_client_knows_view)
            did_show_permission_prompt(request);
    };

    on_cancel_permission_prompt = [this](PermissionPromptRequest const& request) {
        Vector<u64> cancelled_ids;
        for (auto const& [id, pending] : m_permission_requests) {
            if (pending.is_same_request_as(request))
                cancelled_ids.append(id);
        }
        for (auto id : cancelled_ids) {
            m_permission_requests.remove(id);
            m_permission_callbacks.remove(id);
            m_open_requests.remove_all_matching([&](auto const& open) { return open.id == id; });
            auto cancelled = message("permission_cancelled"sv);
            cancelled.set("id"sv, id);
            send(cancelled);
        }
    };

    on_request_external_url_confirmation = [this](URL::URL const& url, URL::Origin const&, ExternalURLHandler const& handler, Function<void(bool)> on_complete) {
        JsonObject external;
        external.set("url"sv, wire_text(url));
        external.set("application"sv, wire_text(handler.application_name()));
        auto id = open_request(RequestKind::ExternalURL, move(external));
        if (!id.has_value()) {
            on_complete(false);
            return;
        }
        m_external_url_callbacks.set(*id, move(on_complete));
    };
}

EmacsWebView::~EmacsWebView() = default;

JsonObject EmacsWebView::message(StringView type) const
{
    JsonObject object;
    object.set("type"sv, type);
    object.set("view"sv, m_id);
    return object;
}

void EmacsWebView::send(JsonObject const& object)
{
    if (m_client_knows_view)
        m_server.send(object);
}

void EmacsWebView::send_latest(JsonObject message)
{
    if (!m_client_knows_view)
        return;
    auto type = message.get_string("type"sv).value();
    m_held_messages.remove_first_matching([&](auto const& held) { return held.get_string("type"sv) == type; });
    m_held_messages.append(move(message));
    send_held_messages();
}

void EmacsWebView::send_held_messages()
{
    if (m_held_messages.is_empty() || m_server.connection_is_behind() || !m_server.client_has_read(m_end_of_sent_latest_messages))
        return;
    auto held_messages = move(m_held_messages);
    for (auto const& message : held_messages)
        send(message);
    m_end_of_sent_latest_messages = m_server.end_of_stream();
}

void EmacsWebView::did_block_download(StringView name)
{
    send_error(wire_formatted("download blocked: {}", name));
    // A window whose first navigation is a download has nothing to show.
    if (m_opened_by_page && !m_active_document_id.has_value())
        close_for_page();
}

void EmacsWebView::close_for_page()
{
    withdraw_page_requests();
    end_hint_session(HintSessionEnd::Cancelled);
    auto closed = message("view_closed"sv);
    closed.set("reason"sv, "closed"sv);
    send(closed);
    m_server.view_did_close({}, m_id);
}

void EmacsWebView::send_error(StringView text)
{
    m_server.send_error(m_id, text);
}

JsonObject EmacsWebView::description() const
{
    JsonObject object;
    object.set("view"sv, m_id);
    object.set("url"sv, wire_text(displayed_url()));
    object.set("title"sv, wire_text(title()));
    object.set("loading"sv, is_loading());
    object.set("can_go_back"sv, can_navigate_back());
    object.set("can_go_forward"sv, can_navigate_forward());
    object.set("zoom"sv, zoom_level());
    object.set("width"sv, m_viewport_size.width().value());
    object.set("height"sv, m_viewport_size.height().value());
    object.set("dpr"sv, m_device_pixel_ratio);
    return object;
}

void EmacsWebView::did_change_state()
{
    schedule_state();
}

void EmacsWebView::update_zoom()
{
    HeadlessWebView::update_zoom();
    schedule_state();
}

// One navigation changes several parts of the state, and the client gets the whole of it once.
void EmacsWebView::schedule_state()
{
    if (m_state_scheduled)
        return;
    m_state_scheduled = true;
    Core::deferred_invoke([weak_this = make_weak_ptr<EmacsWebView>()] {
        if (!weak_this)
            return;
        weak_this->m_state_scheduled = false;
        weak_this->send_state();
    });
}

void EmacsWebView::send_state()
{
    auto state = message("state"sv);
    state.set("url"sv, wire_text(displayed_url()));
    state.set("title"sv, wire_text(title()));
    state.set("loading"sv, is_loading());
    state.set("can_go_back"sv, can_navigate_back());
    state.set("can_go_forward"sv, can_navigate_forward());
    state.set("zoom"sv, zoom_level());

    auto serialized = state.serialized();
    if (serialized == m_last_sent_state)
        return;
    m_last_sent_state = move(serialized);
    send_latest(move(state));
}

// A new client, or a view that gets the focus again, is sent the state even when it repeats the last one sent.
void EmacsWebView::resend_input_method_state()
{
    m_last_sent_input_method_state = {};
    if (on_input_method_state_change)
        on_input_method_state_change();
}

void EmacsWebView::client_went_away()
{
    // The labels were for the client that asked for them.
    end_hint_session(HintSessionEnd::Silently);
    m_client_knows_view = false;
    m_held_messages.clear();
    m_end_of_sent_latest_messages = 0;
    m_frame_in_flight = false;
    m_frame_needs_full = true;
    m_remaining_bands.clear();
    m_shared_frame_buffer = nullptr;
    m_retired_shared_frame_buffer = nullptr;
    m_held_mouse_buttons = MouseButton::None;
    update_visibility();
}

// A view is visible while a client knows it and has not hidden it. Without a client, nobody sees a page, so none
// renders; the visibility the last client chose comes back for the next one.
void EmacsWebView::update_visibility()
{
    auto visible = m_client_knows_view && m_visible_to_client;
    if (visible == m_shown)
        return;
    m_shown = visible;
    set_system_visibility_state(visible ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
    if (visible) {
        m_frame_needs_full = true;
        schedule_frame_flush();
    } else {
        stop_sending_frame();
        release_hidden_frame_buffers();
    }
}

void EmacsWebView::stop_sending_frame()
{
    m_remaining_bands.clear();
    m_frame_needs_full = true;
    m_server.stop_sending_frames(*this);
}

// A hidden view needs no segment and no pixels to compress until it is shown, when it sends a whole frame anyway. The
// segment goes once the client is done with the frame in it.
void EmacsWebView::release_hidden_frame_buffers()
{
    if (m_shown || m_frame_in_flight)
        return;
    m_shared_frame_buffer = nullptr;
    m_retired_shared_frame_buffer = nullptr;
    m_frame_encoder.release_scratch();
}

void EmacsWebView::client_stopped_using_shared_memory()
{
    m_shared_frame_buffer = nullptr;
    m_retired_shared_frame_buffer = nullptr;
    m_frame_needs_full = true;
    schedule_frame_flush();
}

void EmacsWebView::client_changed()
{
    client_went_away();
    m_client_knows_view = true;
    update_visibility();
    m_last_sent_state = {};
    m_last_sent_background_color.clear();
    send_state();
    resend_input_method_state();
    if (auto color = page_background_color(); color.alpha() != 0)
        on_page_background_color_change(color);
    for (auto const& request : m_open_requests) {
        send(request.message);
        if (auto permission = m_permission_requests.get(request.id); permission.has_value())
            did_show_permission_prompt(*permission);
    }
    schedule_frame_flush();
}

void EmacsWebView::client_cannot_use_shared_memory()
{
    m_shared_frame_buffer = nullptr;
    m_retired_shared_frame_buffer = nullptr;
    m_frame_in_flight = false;
    m_frame_needs_full = true;
    schedule_frame_flush();
}

void EmacsWebView::did_accept_presented_backing_store(i32, Gfx::IntRect damage_rect)
{
    m_pending_damage = m_pending_damage.has_value() ? m_pending_damage->united(damage_rect) : damage_rect;
    schedule_frame_flush();
}

void EmacsWebView::schedule_frame_flush()
{
    if (m_frame_flush_scheduled)
        return;
    m_frame_flush_scheduled = true;
    Core::deferred_invoke([weak_this = make_weak_ptr<EmacsWebView>()] {
        if (!weak_this)
            return;
        weak_this->m_frame_flush_scheduled = false;
        weak_this->flush_frame();
    });
}

void EmacsWebView::flush_frame()
{
    // The client has one frame record at a time. What changes meanwhile is sent once it has acknowledged that. A view the
    // client has hidden sends nothing.
    auto has_frame_to_send = m_frame_needs_full || m_remaining_bands.has_value() || m_pending_damage.has_value();
    if (!m_client_knows_view || !m_visible_to_client || m_frame_in_flight || !has_frame_to_send) {
        m_server.stop_waiting_for_frame_turn(*this);
        return;
    }

    auto frame = backing_stores().displayed_frame();
    auto bitmap = frame.has_value() && frame->shared_image_buffer ? frame->shared_image_buffer->bitmap_if_present() : nullptr;
    if (!bitmap) {
        // The frame is sent once it is presented.
        m_server.stop_waiting_for_frame_turn(*this);
        return;
    }

    // NB: The views share the connection; this one may have to wait for its turn, and is told when it has it.
    if (!m_server.begin_frame_turn(*this))
        return;
    ScopeGuard end_turn = [&] {
        m_server.end_frame_turn(*this, m_remaining_bands.has_value());
    };

    // NB: A backing store can be larger than the frame presented in it.
    Gfx::IntSize frame_size { min(frame->size.width(), bitmap->width()), min(frame->size.height(), bitmap->height()) };
    if (auto result = send_frame(*bitmap, frame_size, frame->device_pixel_ratio); result.is_error()) {
        // What the frame would have shown is sent with the next one, whole.
        dbgln("Emacs: view {}: cannot send a frame: {}", m_id, result.error());
        m_frame_needs_full = true;
        m_remaining_bands.clear();
    }
}

ErrorOr<bool> EmacsWebView::write_frame_to_shared_memory(Gfx::Bitmap const& bitmap, Gfx::IntSize frame_size, Gfx::IntRect rect, bool whole_frame)
{
    auto row_stride = static_cast<size_t>(frame_size.width()) * 4;
    auto frame_bytes = row_stride * frame_size.height();

    auto announced_new_segment = false;
    if (!m_shared_frame_buffer || m_shared_frame_buffer->size() < frame_bytes) {
        auto buffer = TRY(EmacsSharedFrameBuffer::create(frame_bytes));
        m_retired_shared_frame_buffer = move(m_shared_frame_buffer);
        m_shared_frame_buffer = move(buffer);

        auto shm = message("shm"sv);
        shm.set("name"sv, wire_text(m_shared_frame_buffer->name()));
        shm.set("size"sv, static_cast<u64>(m_shared_frame_buffer->size()));
        send(shm);
        announced_new_segment = true;
    }

    if (announced_new_segment || whole_frame || m_shared_frame_size != frame_size)
        rect = { {}, frame_size };
    m_shared_frame_size = frame_size;

    auto offset = rect.y() * row_stride + static_cast<size_t>(rect.x()) * 4;
    EmacsFrameEncoder::copy_pixels(bitmap, rect, m_shared_frame_buffer->bytes().slice(offset), row_stride);
    return announced_new_segment;
}

ErrorOr<void> EmacsWebView::send_frame(Gfx::Bitmap const& bitmap, Gfx::IntSize frame_size, double device_pixel_ratio)
{
    Gfx::IntRect frame_rect { {}, frame_size };
    if (frame_rect.is_empty())
        return {};

    if (frame_size != m_last_sent_frame_size)
        m_frame_needs_full = true;
    if (m_frame_needs_full)
        m_remaining_bands.clear();

    Gfx::IntRect rect;
    u8 flags = 0;
    if (m_remaining_bands.has_value()) {
        rect = m_remaining_bands->intersected(frame_rect);
        if (rect.is_empty()) {
            m_remaining_bands.clear();
            return {};
        }
    } else if (m_frame_needs_full) {
        rect = frame_rect;
        flags |= EmacsProtocol::frame_flag_full;
        m_pending_damage.clear();
    } else {
        if (!m_pending_damage.has_value())
            return {};
        rect = m_pending_damage->intersected(frame_rect);
        m_pending_damage.clear();
        if (rect.is_empty())
            return {};
    }

    auto encoding_started = MonotonicTime::now();
    auto encoding = EmacsProtocol::FrameEncoding::Raw;
    if (m_server.client_uses_shared_memory()) {
        auto announced_new_segment = write_frame_to_shared_memory(bitmap, frame_size, rect, (flags & EmacsProtocol::frame_flag_full) != 0);
        if (announced_new_segment.is_error()) {
            dbgln("Emacs: view {}: cannot share a frame in memory: {}", m_id, announced_new_segment.error());
        } else {
            encoding = EmacsProtocol::FrameEncoding::Shm;
            m_remaining_bands.clear();
            if (announced_new_segment.value()) {
                rect = frame_rect;
                flags |= EmacsProtocol::frame_flag_full;
            }
        }
    }

    if (encoding != EmacsProtocol::FrameEncoding::Shm) {
        auto row_bytes = static_cast<size_t>(rect.width()) * 4;
        auto rows_per_record = static_cast<int>(max<size_t>(1, maximum_pixel_bytes_per_record / row_bytes));
        if (rect.height() > rows_per_record) {
            m_remaining_bands = Gfx::IntRect { rect.x(), rect.y() + rows_per_record, rect.width(), rect.height() - rows_per_record };
            rect.set_height(rows_per_record);
            flags |= EmacsProtocol::frame_flag_more;
        } else {
            m_remaining_bands.clear();
        }

        if (m_server.client_accepts_zstd() && row_bytes * rect.height() >= minimum_pixel_bytes_to_compress)
            encoding = EmacsProtocol::FrameEncoding::Zstd;
    }

    EmacsProtocol::FrameHeader header {
        .seq = m_frame_seq + 1,
        .view = m_id,
        .width = static_cast<u32>(frame_size.width()),
        .height = static_cast<u32>(frame_size.height()),
        .dpr = device_pixel_ratio,
        .damage_x = static_cast<u32>(rect.x()),
        .damage_y = static_cast<u32>(rect.y()),
        .damage_width = static_cast<u32>(rect.width()),
        .damage_height = static_cast<u32>(rect.height()),
        .encoding = encoding,
        .flags = flags,
    };

    ByteBuffer record;
    EmacsProtocol::begin_record(record, EmacsProtocol::RecordKind::Frame);
    EmacsProtocol::append_frame_header(record, header);
    switch (encoding) {
    case EmacsProtocol::FrameEncoding::Raw:
        TRY(m_frame_encoder.append_raw(record, bitmap, rect));
        break;
    case EmacsProtocol::FrameEncoding::Zstd:
        // Pixels that do not compress go as they are.
        if (!TRY(m_frame_encoder.append_zstd(record, bitmap, rect))) {
            header.encoding = encoding = EmacsProtocol::FrameEncoding::Raw;
            record.resize(EmacsProtocol::record_header_size);
            EmacsProtocol::append_frame_header(record, header);
            TRY(m_frame_encoder.append_raw(record, bitmap, rect));
        }
        break;
    case EmacsProtocol::FrameEncoding::Shm: {
        // The frame starts at the beginning of the segment.
        u64 offset = 0;
        record.append(&offset, sizeof(offset));
        break;
    }
    }
    TRY(EmacsProtocol::finish_record(record));

    // Whole frames are rare, and their cost is what a slow connection or a large view shows first.
    if (flags & EmacsProtocol::frame_flag_full) {
        static constexpr Array encoding_names = { "raw"sv, "zstd"sv, "shm"sv };
        dbgln("Emacs: view {}: whole frame {}x{}@{} as {}: {} bytes in {} ms", m_id, frame_size.width(), frame_size.height(), device_pixel_ratio, encoding_names[to_underlying(encoding)], record.size(), (MonotonicTime::now() - encoding_started).to_milliseconds());
    }

    ++m_frame_seq;
    m_frame_in_flight = true;
    m_frame_needs_full = false;
    m_last_sent_frame_size = frame_size;
    m_server.send_record(move(record));
    return {};
}

void EmacsWebView::handle_frame_ack(JsonObject const& ack)
{
    if (!m_frame_in_flight || ack.get_u32("seq"sv) != m_frame_seq)
        return;

    m_frame_in_flight = false;
    m_retired_shared_frame_buffer = nullptr;
    release_hidden_frame_buffers();
    if (m_frame_needs_full || m_remaining_bands.has_value() || m_pending_damage.has_value())
        schedule_frame_flush();
}

void EmacsWebView::set_viewport(JsonObject const& object)
{
    auto viewport = viewport_from(object, { m_viewport_size, m_device_pixel_ratio }, m_server.viewport_area_left_for(this));
    if (viewport.is_error()) {
        send_error(viewport.error().string_literal());
        return;
    }
    m_device_pixel_ratio = viewport.value().device_pixel_ratio;
    reset_viewport_size(viewport.value().size);
    m_frame_needs_full = true;
}

void EmacsWebView::handle_message(StringView type, JsonObject const& object)
{
    if (type == "frame_ack"sv) {
        handle_frame_ack(object);
    } else if (type == "mouse"sv) {
        handle_mouse(object);
    } else if (type == "key"sv) {
        handle_key(object);
    } else if (type == "text"sv) {
        if (auto text = object.get_string("text"sv); text.has_value() && !text->is_empty())
            commit_text_from_input_method(Utf16String::from_utf8(*text));
    } else if (type == "set_viewport"sv) {
        set_viewport(object);
    } else if (type == "set_visibility"sv) {
        m_visible_to_client = object.get_bool("visible"sv).value_or(true);
        update_visibility();
    } else if (type == "set_focus"sv) {
        auto focused = object.get_bool("focused"sv).value_or(true);
        set_has_system_focus(focused);
        if (focused)
            resend_input_method_state();
    } else if (type == "load"sv) {
        handle_load(object);
    } else if (type == "load_html"sv) {
        if (auto html = object.get_string("html"sv); html.has_value())
            load_html(*html);
        else
            send_error("load_html needs html"sv);
    } else if (type == "reload"sv) {
        reload(object.get_bool("bypass_cache"sv).value_or(false) ? BypassCache::Yes : BypassCache::No);
    } else if (type == "stop"sv) {
        stop_loading();
    } else if (type == "history"sv) {
        auto delta = integer(object, "delta"sv);
        if (delta < -maximum_history_delta || delta > maximum_history_delta)
            send_error("history delta is out of range"sv);
        else if (delta != 0)
            traverse_the_history_by_delta(delta);
    } else if (type == "zoom"sv) {
        auto operation = object.get_string("op"sv).value_or({});
        if (operation == "in"sv)
            zoom_in();
        else if (operation == "out"sv)
            zoom_out();
        else if (operation == "reset"sv)
            reset_zoom();
        else
            send_error("zoom needs op in, out or reset"sv);
    } else if (type == "find"sv) {
        handle_find(object);
    } else if (type == "find_step"sv) {
        if (object.get_string("direction"sv).value_or({}) == "previous"sv)
            find_in_page_previous_match();
        else
            find_in_page_next_match();
    } else if (type == "edit"sv) {
        handle_edit(object);
    } else if (type == "paste"sv) {
        handle_paste(object);
    } else if (type == "request_selection"sv) {
        handle_request_selection(object);
    } else if (type == "screenshot"sv) {
        handle_screenshot(object);
    } else if (type == "set_console"sv) {
        m_console_enabled = object.get_bool("enabled"sv).value_or(false);
        start_sending_console_output(m_console_enabled, m_console_enabled);
    } else if (type == "hints"sv) {
        handle_hints(object);
    } else if (type == "hints_narrow"sv) {
        handle_hints_narrow(object);
    } else if (type == "hints_end"sv) {
        handle_hints_end(object);
    } else if (type == "request_source"sv) {
        send_error("request_source is not supported"sv);
    } else if (type == "dialog_closed"sv) {
        handle_dialog_closed(object);
    } else if (type == "select_closed"sv) {
        handle_select_closed(object);
    } else if (type == "file_picker_closed"sv) {
        handle_file_picker_closed(object);
    } else if (type == "color_picker_closed"sv) {
        handle_color_picker_closed(object);
    } else if (type == "date_picker_closed"sv) {
        handle_date_picker_closed(object);
    } else if (type == "permission_closed"sv) {
        handle_permission_closed(object);
    } else if (type == "external_url_closed"sv) {
        handle_external_url_closed(object);
    }
}

static Web::MouseEvent mouse_event(Web::MouseEvent::Type type, Gfx::IntPoint position, MouseButton button, MouseButton buttons, KeyModifier modifiers)
{
    Web::MouseEvent event {};
    event.type = type;
    event.position = position.to_type<Web::DevicePixels>();
    event.screen_position = event.position;
    event.button = button;
    event.buttons = buttons;
    event.modifiers = modifiers;
    return event;
}

void EmacsWebView::handle_mouse(JsonObject const& object)
{
    auto kind = object.get_string("kind"sv).value_or({});
    auto modifiers = EmacsProtocol::modifiers_from_protocol(object.get_u32("modifiers"sv).value_or(0));

    auto viewport = viewport_size().to_type<int>();
    Gfx::IntPoint position {
        clamp(integer(object, "x"sv), 0, max(0, viewport.width() - 1)),
        clamp(integer(object, "y"sv), 0, max(0, viewport.height() - 1)),
    };

    auto button = MouseButton::None;
    if (auto name = object.get_string("button"sv); name.has_value())
        button = EmacsProtocol::mouse_button_from_name(*name).value_or(MouseButton::None);

    // The client says which buttons are held after the event. For one that does not, the presses and releases count.
    auto held_buttons = object.get_array("buttons"sv);
    if (held_buttons.has_value()) {
        m_held_mouse_buttons = MouseButton::None;
        held_buttons->for_each([&](JsonValue const& name) {
            if (name.is_string())
                m_held_mouse_buttons |= EmacsProtocol::mouse_button_from_name(name.as_string()).value_or(MouseButton::None);
        });
    }

    if (kind == "move"sv) {
        enqueue_input_event(mouse_event(Web::MouseEvent::Type::MouseMove, position, MouseButton::None, m_held_mouse_buttons, modifiers));
    } else if (kind == "down"sv) {
        if (button == MouseButton::None) {
            send_error("mouse down needs a button"sv);
            return;
        }
        if (!held_buttons.has_value())
            m_held_mouse_buttons |= button;
        m_click_count = clamp(integer(object, "clicks"sv, 1), 1, 3);
        auto event = mouse_event(Web::MouseEvent::Type::MouseDown, position, button, m_held_mouse_buttons, modifiers);
        event.click_count = m_click_count;
        enqueue_input_event(move(event));
    } else if (kind == "up"sv) {
        if (button == MouseButton::None) {
            send_error("mouse up needs a button"sv);
            return;
        }
        if (!held_buttons.has_value())
            m_held_mouse_buttons &= ~button;
        auto event = mouse_event(Web::MouseEvent::Type::MouseUp, position, button, m_held_mouse_buttons, modifiers);
        event.click_count = clamp(integer(object, "clicks"sv, m_click_count), 1, 3);
        enqueue_input_event(move(event));
    } else if (kind == "wheel"sv) {
        auto precision = object.get_bool("precise"sv).value_or(false) ? Web::WheelDeltaPrecision::Precise : Web::WheelDeltaPrecision::Discrete;
        auto delta_x = number(object, "dx"sv).value_or(0);
        auto delta_y = number(object, "dy"sv).value_or(0);
        if (!isfinite(delta_x) || !isfinite(delta_y) || fabs(delta_x) > maximum_wheel_delta || fabs(delta_y) > maximum_wheel_delta) {
            send_error("wheel deltas are out of range"sv);
            return;
        }
        auto event = mouse_event(Web::MouseEvent::Type::MouseWheel, position, MouseButton::None, m_held_mouse_buttons, modifiers);
        event.wheel_delta_x = delta_x;
        event.wheel_delta_y = delta_y;
        event.wheel_tick_120ths_x = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_x, precision);
        event.wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_y, precision);
        event.wheel_delta_precision = precision;
        enqueue_input_event(move(event));
    } else if (kind == "leave"sv) {
        enqueue_input_event(mouse_event(Web::MouseEvent::Type::MouseLeave, {}, MouseButton::None, MouseButton::None, modifiers));
    } else {
        send_error("mouse needs kind down, up, move, wheel or leave"sv);
    }
}

void EmacsWebView::handle_key(JsonObject const& object)
{
    auto kind = object.get_string("kind"sv).value_or({});
    if (kind != "down"sv && kind != "up"sv) {
        send_error("key needs kind down or up"sv);
        return;
    }
    auto is_down = kind == "down"sv;

    auto key = KeyCode::Key_Invalid;
    if (auto name = object.get_string("key"sv); name.has_value()) {
        if (auto key_code = EmacsProtocol::key_code_from_name(*name); key_code.has_value())
            key = *key_code;
        else
            dbgln("Emacs: view {}: unknown key name '{}'", m_id, *name);
    }

    u32 code_point = 0;
    if (object.has("code_point"sv)) {
        auto requested = number(object, "code_point"sv);
        if (!requested.has_value() || !(*requested >= 0 && *requested <= 0x10ffff) || *requested != trunc(*requested) || (*requested >= 0xd800 && *requested <= 0xdfff)) {
            send_error("code_point is not a Unicode scalar value"sv);
            return;
        }
        code_point = static_cast<u32>(*requested);
    }

    Web::KeyEvent event {};
    event.type = is_down ? Web::KeyEvent::Type::KeyDown : Web::KeyEvent::Type::KeyUp;
    event.key = key;
    event.modifiers = EmacsProtocol::modifiers_from_protocol(object.get_u32("modifiers"sv).value_or(0));
    event.code_point = code_point;
    event.repeat = is_down && object.get_bool("repeat"sv).value_or(false);

    auto is_shortcut = (event.modifiers & (KeyModifier::Mod_Ctrl | KeyModifier::Mod_Super)) != 0;
    event.should_insert_text = is_down && event.code_point != 0 && !is_shortcut;
    enqueue_input_event(move(event));
}

void EmacsWebView::handle_load(JsonObject const& object)
{
    auto text = object.get_string("url"sv);
    if (!text.has_value()) {
        send_error("load needs url"sv);
        return;
    }

    auto url = sanitize_url(*text, Application::settings().search_engine_settings().engine);
    if (!url.has_value()) {
        send_error(wire_formatted("not a URL: {}", *text));
        return;
    }
    load_from_browser_ui(*url);
}

void EmacsWebView::handle_find(JsonObject const& object)
{
    m_find_query = object.get_string("query"sv).value_or({});
    if (m_find_query.is_empty()) {
        find_in_page_end();
        return;
    }

    auto case_sensitivity = object.get_bool("case_sensitive"sv).value_or(false) ? CaseSensitivity::CaseSensitive : CaseSensitivity::CaseInsensitive;
    find_in_page(Utf16String::from_utf8(m_find_query), case_sensitivity, object.get_bool("highlight_all"sv).value_or(false));
}

void EmacsWebView::handle_edit(JsonObject const& object)
{
    auto operation = object.get_string("op"sv).value_or({});
    if (operation == "select_all"sv) {
        select_all();
    } else if (operation == "undo"sv) {
        undo();
    } else if (operation == "redo"sv) {
        redo();
    } else if (operation == "cut"sv) {
        // The client owns the clipboard, so the text that was cut goes to it.
        cut_selected_text()->when_resolved([weak_this = make_weak_ptr<EmacsWebView>()](ByteString const& text) {
            if (!weak_this)
                return;
            auto clipboard = weak_this->message("clipboard"sv);
            clipboard.set("text"sv, wire_text(text));
            weak_this->send(clipboard);
        });
    } else {
        send_error("edit needs op select_all, cut, undo or redo"sv);
    }
}

void EmacsWebView::handle_paste(JsonObject const& object)
{
    auto text = object.get_string("text"sv);
    if (!text.has_value()) {
        send_error("paste needs text"sv);
        return;
    }

    Web::Clipboard::SystemClipboardItem item;
    item.system_clipboard_representations.append({ "text/plain"_string, text->to_byte_string() });
    Application::the().insert_clipboard_item(move(item));
    paste_from_clipboard(Web::Clipboard::PasteMode::PlainTextOnly);
}

void EmacsWebView::handle_request_selection(JsonObject const& object)
{
    selected_text()->when_resolved([weak_this = make_weak_ptr<EmacsWebView>(), request = echoed(object, "request"sv)](ByteString const& text) {
        if (!weak_this)
            return;
        auto selection = weak_this->message("selection"sv);
        selection.set("request"sv, request);
        selection.set("text"sv, wire_text(text));
        weak_this->send(selection);
    });
}

void EmacsWebView::handle_screenshot(JsonObject const& object)
{
    auto kind = object.get_string("kind"sv).value_or("visible"_string);
    PendingScreenshot screenshot {
        .request = echoed(object, "request"sv),
        .path = object.get_string("path"sv).map([](auto const& path) { return path.to_byte_string(); }),
    };

    if (kind == "visible"sv) {
        RefPtr<Gfx::Bitmap> bitmap;
        Gfx::IntSize frame_size;
        if (auto frame = backing_stores().displayed_frame(); frame.has_value() && frame->shared_image_buffer) {
            bitmap = frame->shared_image_buffer->bitmap_if_present();
            frame_size = frame->size;
        }
        if (!bitmap) {
            send_error("there is no frame to take a screenshot of"sv);
            return;
        }

        // NB: A backing store can be larger than the frame presented in it, and holds older frames outside of it.
        auto frame_rect = Gfx::IntRect { {}, frame_size }.intersected(bitmap->rect());
        if (frame_rect != bitmap->rect()) {
            auto cropped = bitmap->cropped(frame_rect);
            if (cropped.is_error()) {
                send_error(wire_formatted("cannot take a screenshot: {}", cropped.error()));
                return;
            }
            bitmap = cropped.release_value();
        }
        save_screenshot(screenshot, *bitmap);
        return;
    }

    if (kind != "full"sv) {
        send_error("screenshot needs kind visible or full"sv);
        return;
    }
    if (m_pending_screenshot.has_value()) {
        send_error("a screenshot is already being taken"sv);
        return;
    }

    m_pending_screenshot = move(screenshot);
    page().async_take_document_screenshot();
}

void EmacsWebView::did_receive_screenshot(Badge<WebContentPage>, Gfx::ShareableBitmap const& screenshot)
{
    // A renderer answers a screenshot request; any other screenshot it sends is misbehavior.
    if (!m_pending_screenshot.has_value()) {
        page().client().did_misbehave("did_take_screenshot"sv, "no screenshot was requested"sv);
        return;
    }
    auto pending = m_pending_screenshot.release_value();

    if (!screenshot.is_valid()) {
        send_error("cannot take a screenshot of the whole page"sv);
        return;
    }
    save_screenshot(pending, *screenshot.bitmap());
}

void EmacsWebView::save_screenshot(PendingScreenshot const& screenshot, Gfx::Bitmap const& bitmap)
{
    auto save = [&]() -> ErrorOr<ByteString> {
        auto png = TRY(Gfx::PNGWriter::encode(bitmap));
        ByteString default_name;
        if (!screenshot.path.has_value())
            default_name = ByteString::formatted("ladybird-emacs-{}-screenshot-{}", getpid(), m_server.next_screenshot_number());
        return write_private_file(screenshot.path, default_name, ".png"sv, png);
    };

    auto saved_path = save();
    if (saved_path.is_error()) {
        send_error(wire_formatted("cannot save a screenshot: {}", saved_path.error()));
        return;
    }

    auto saved = message("screenshot_saved"sv);
    saved.set("request"sv, screenshot.request);
    saved.set("path"sv, wire_text(saved_path.value()));
    send(saved);
}

static constexpr auto hint_answer_timeout = AK::Duration::from_seconds(5);

EmacsWebView::HintSession* EmacsWebView::hint_session_named_by(JsonObject const& object)
{
    auto id = EmacsProtocol::hint_session_id(object.get("id"sv).value_or({}));
    if (!m_hint_session.has_value() || m_hint_session->id != id)
        return nullptr;
    return &*m_hint_session;
}

// The error names the session the refused message did, so that the client can end it.
void EmacsWebView::send_hint_error(StringView text, JsonValue id)
{
    auto error = message("error"sv);
    error.set("message"sv, wire_text(text));
    if (!id.is_null())
        error.set("id"sv, move(id));
    send(error);
}

void EmacsWebView::handle_hints(JsonObject const& object)
{
    auto id = EmacsProtocol::hint_session_id(object.get("id"sv).value_or({}));
    if (!id.has_value()) {
        send_hint_error("hints needs an id that is a non-negative integer below 2^53"sv, echoed(object, "id"sv));
        return;
    }
    auto alphabet = object.get_string("alphabet"sv);
    if (!alphabet.has_value() || !EmacsProtocol::is_valid_hint_alphabet(*alphabet)) {
        send_hint_error("hints needs an alphabet of 2 to 36 distinct characters of a-z and 0-9"sv, echoed(object, "id"sv));
        return;
    }

    end_hint_session(HintSessionEnd::Silently);

    auto serial = m_next_hint_session_serial++;
    auto timeout = Core::Timer::create_single_shot(hint_answer_timeout, [weak_this = make_weak_ptr<EmacsWebView>(), serial] {
        if (!weak_this || !weak_this->m_hint_session.has_value() || weak_this->m_hint_session->serial != serial || weak_this->m_hint_session->listed)
            return;
        dbgln("Emacs: view {}: a page did not answer a hints request in time", weak_this->m_id);
        weak_this->end_hint_session(HintSessionEnd::Cancelled);
    });
    m_hint_session = HintSession {
        .serial = serial,
        .id = *id,
        .alphabet = *alphabet,
        .collection_request = m_server.next_request_id(),
        .pages = {},
        .pages_to_answer = {},
        .answers = {},
        .targets = {},
        .labels = {},
        .listed = false,
        .prefix = {},
        .timeout = timeout,
        .grace = nullptr,
    };

    // Each process hosting a document of the tab finds what can be clicked in its documents.
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        if (!page.is_open())
            return;
        m_hint_session->pages.append(page);
        m_hint_session->pages_to_answer.append(&page);
    });
    timeout->start();
    for (auto& page : m_hint_session->pages)
        page->async_hints_collect(m_hint_session->collection_request);
    if (m_hint_session->pages.is_empty())
        list_hints();
}

// The process hosting the top-level document is waited for; once it answered, the others get a short while more, and
// the frames of a process that has not answered by then add nothing.
static constexpr auto hint_answer_grace = AK::Duration::from_milliseconds(500);

void EmacsWebView::did_collect_hints(WebContentPage& page, u64 request_id, JsonValue const& result)
{
    if (!m_hint_session.has_value() || m_hint_session->collection_request != request_id || m_hint_session->listed)
        return;
    auto& session = *m_hint_session;
    auto index = session.pages_to_answer.find_first_index(&page);
    if (!index.has_value())
        return;
    session.pages_to_answer.remove(*index);

    auto host = session.pages.find_first_index_if([&](auto const& asked) { return asked.ptr() == &page; });
    if (auto answer = EmacsProtocol::parse_hint_answer(result); answer.is_error())
        dbgln("Emacs: view {}: a page answered a hints request with something unusable: {}", m_id, answer.error());
    else
        session.answers.append({ *host, answer.release_value() });

    if (session.pages_to_answer.is_empty()) {
        list_hints();
        return;
    }
    if (&page != traversable().page_hosting(traversable()).ptr() || session.grace)
        return;
    session.grace = Core::Timer::create_single_shot(hint_answer_grace, [weak_this = make_weak_ptr<EmacsWebView>(), serial = session.serial] {
        if (!weak_this || !weak_this->m_hint_session.has_value() || weak_this->m_hint_session->serial != serial || weak_this->m_hint_session->listed)
            return;
        dbgln("Emacs: view {}: listing hints without {} page(s) that did not answer", weak_this->m_id, weak_this->m_hint_session->pages_to_answer.size());
        weak_this->list_hints();
    });
    session.grace->start();
}

// What the processes found, as their documents and frames are in the tree the UI process knows.
void EmacsWebView::list_hints()
{
    auto& session = *m_hint_session;
    // NB: The timers may be the ones whose callback this is.
    for (auto* timer : { &session.timeout, &session.grace }) {
        if (*timer) {
            (*timer)->stop();
            Core::deferred_invoke([timer = move(*timer)] { });
        }
    }

    Vector<EmacsProtocol::HintNavigable> navigables;
    Function<void(CanonicalNavigable const&, Optional<size_t>)> describe = [&](CanonicalNavigable const& navigable, Optional<size_t> parent) {
        auto host_page = traversable().page_hosting(navigable);
        auto host = session.pages.find_first_index_if([&](auto const& page) { return page.ptr() == host_page.ptr(); });
        EmacsProtocol::HintNavigable described;
        described.id = navigable.id();
        described.parent = parent;
        described.host = host.value_or(NumericLimits<size_t>::max());
        described.remote = navigable.has_remote_host();
        described.local_root_offset = traversable().local_root_offset(navigable).to_type<int>();
        described.visible = navigable.viewport_intersection().to_type<int>();
        navigables.append(described);
        auto index = navigables.size() - 1;
        for (auto const& child : navigable.children())
            describe(*child, index);
    };
    describe(traversable(), {});

    auto list = EmacsProtocol::merge_hint_answers(session.answers, navigables, viewport_size().to_type<int>(), m_device_pixel_ratio * zoom_level());
    session.labels = EmacsProtocol::hint_labels(session.alphabet, list.items.size());

    JsonArray items;
    HashMap<WebContentPage*, JsonArray> labels_by_page;
    for (size_t index = 0; index < list.items.size(); ++index) {
        auto const& listed = list.items[index];
        auto& page = session.pages[listed.host];
        JsonObject object;
        object.set("index"sv, static_cast<u64>(index));
        object.set("label"sv, session.labels[index]);
        object.set("kind"sv, EmacsProtocol::hint_kind_name(listed.item.kind));
        object.set("text"sv, listed.item.text);
        object.set("url"sv, string_or_null(listed.item.url));
        object.set("x"sv, listed.item.x);
        object.set("y"sv, listed.item.y);
        object.set("width"sv, listed.item.width);
        object.set("height"sv, listed.item.height);
        items.must_append(move(object));
        session.targets.append({ page, Web::UniqueNodeID { listed.item.node } });

        JsonArray pair;
        pair.must_append(listed.item.node);
        pair.must_append(session.labels[index]);
        labels_by_page.ensure(page.ptr()).must_append(move(pair));
    }

    session.listed = true;
    send_hints_list(move(items), list.truncated);

    if (session.targets.is_empty()) {
        end_hint_session(HintSessionEnd::Silently);
        return;
    }
    for (auto& [page, labels] : labels_by_page) {
        if (!page->is_open())
            continue;
        page->async_hints_draw(labels);
        if (!session.prefix.is_empty())
            page->async_hints_narrow(session.prefix);
    }
}

void EmacsWebView::send_hints_list(JsonArray items, bool truncated)
{
    auto list = message("hints_list"sv);
    list.set("id"sv, m_hint_session->id);
    list.set("items"sv, move(items));
    list.set("truncated"sv, truncated);
    send(list);
}

void EmacsWebView::handle_hints_narrow(JsonObject const& object)
{
    auto* session = hint_session_named_by(object);
    if (!session) {
        send_hint_error("hints_narrow names no hint session of the view"sv, echoed(object, "id"sv));
        return;
    }
    // NB: Until the processes have answered, the labels are at most as long as those of the most items there can be.
    auto label_length = session->labels.is_empty()
        ? EmacsProtocol::hint_label_length(session->alphabet.bytes().size(), EmacsProtocol::maximum_hint_count)
        : session->labels.first().bytes().size();
    auto prefix = object.get_string("prefix"sv);
    if (!prefix.has_value() || !EmacsProtocol::is_valid_hint_prefix(*prefix, session->alphabet, label_length)) {
        send_hint_error("hints_narrow needs a prefix of characters of the alphabet, no longer than the labels"sv, echoed(object, "id"sv));
        return;
    }

    session->prefix = *prefix;
    // NB: Labels that are yet to be drawn are drawn narrowed.
    if (!session->listed)
        return;
    HashTable<WebContentPage*> narrowed;
    for (auto& target : session->targets) {
        if (narrowed.set(target.page.ptr()) == HashSetResult::InsertedNewEntry && target.page->is_open())
            target.page->async_hints_narrow(session->prefix);
    }
}

void EmacsWebView::handle_hints_end(JsonObject const& object)
{
    auto* session = hint_session_named_by(object);
    if (!session) {
        send_hint_error("hints_end names no hint session of the view"sv, echoed(object, "id"sv));
        return;
    }

    Optional<size_t> index;
    if (session->listed)
        index = EmacsProtocol::hint_activation_index(object.get("activate"sv).value_or({}), session->targets.size());
    end_hint_session(HintSessionEnd::Silently, index);
}

void EmacsWebView::end_hint_session(HintSessionEnd how, Optional<size_t> activate)
{
    if (!m_hint_session.has_value())
        return;
    auto session = m_hint_session.release_value();
    for (auto* timer : { &session.timeout, &session.grace }) {
        if (*timer) {
            (*timer)->stop();
            // NB: The timer may be the one whose callback this is.
            Core::deferred_invoke([timer = move(*timer)] { });
        }
    }

    if (how == HintSessionEnd::Cancelled) {
        auto cancelled = message("hints_cancelled"sv);
        cancelled.set("id"sv, session.id);
        send(cancelled);
    }

    // NB: A process handles what it is sent in order, so it prepares the activation before it forgets the labels.
    if (activate.has_value()) {
        auto& target = session.targets[*activate];
        if (target.page->is_open()) {
            auto request = m_server.next_request_id();
            auto now = MonotonicTime::now();
            m_hint_activations.remove_all_matching([&](auto const&, auto const& activation) { return activation.deadline < now; });
            m_hint_activations.set(request, { session.id, target.page, target.node, now + hint_answer_timeout });
            target.page->async_hints_activate(request, target.node);
        }
    }
    for (auto& page : session.pages) {
        if (page->is_open())
            page->async_hints_clear();
    }
}

void EmacsWebView::did_prepare_hint_activation(WebContentPage& page, u64 request_id, JsonValue const& result)
{
    // NB: Only the process that was asked answers, and only once.
    auto activation_entry = m_hint_activations.get(request_id);
    if (!activation_entry.has_value() || activation_entry->page.ptr() != &page)
        return;
    auto activation = m_hint_activations.take(request_id).release_value();
    auto session_id = JsonValue { activation.session_id };

    auto const* answer = result.is_object() ? &result.as_object() : nullptr;
    auto action = answer ? answer->get_string("action"sv).value_or({}) : String {};
    if (action == "focus"sv)
        return;
    if (action == "tampered"sv) {
        send_hint_error("the labels were tampered with, so nothing was activated"sv, session_id);
        return;
    }
    if (action != "click"sv) {
        send_hint_error("the element to activate is gone or covered"sv, session_id);
        return;
    }

    auto refuse = [&] { send_hint_error("the element to activate cannot be clicked"sv, session_id); };
    auto x = number(*answer, "x"sv);
    auto y = number(*answer, "y"sv);
    if (!x.has_value() || !y.has_value() || !(*x >= 0 && *x < maximum_viewport_dimension) || !(*y >= 0 && *y < maximum_viewport_dimension))
        return refuse();

    // The local root the point is in must be one the process hosts.
    CanonicalNavigable const* root = &traversable();
    if (auto name = answer->get_string("navigable"sv); name.has_value()) {
        auto id = EmacsProtocol::hint_navigable_from_name(*name);
        auto navigable = id.has_value() ? traversable().find(*id) : OptionalNone {};
        if (!navigable.has_value() || !navigable->has_remote_host())
            return refuse();
        root = &*navigable;
    }
    if (traversable().page_hosting(*root).ptr() != &page)
        return refuse();

    // The click goes into the view at the point in its viewport, as a click of the user's mouse does, so each process
    // the events pass through hit-tests them on their way: those hosting a document around the element must find the frame
    // the element is in, the one hosting the element must find the element.
    auto offset = traversable().local_root_offset(*root);
    Gfx::IntPoint position { static_cast<int>(*x) + offset.x().value(), static_cast<int>(*y) + offset.y().value() };
    if (!Gfx::IntRect { {}, viewport_size().to_type<int>() }.contains(position))
        return refuse();

    // The local root that holds a navigable in the process hosting it: none for the top-level traversable.
    auto local_root_id_of = [](CanonicalNavigable const& navigable) -> Optional<Web::HTML::CrossProcessId> {
        for (auto const* ancestor = &navigable; ancestor->parent(); ancestor = ancestor->parent()) {
            if (ancestor->has_remote_host())
                return ancestor->id();
        }
        return {};
    };
    struct Guard {
        NonnullRefPtr<WebContentPage> page;
        Optional<Web::UniqueNodeID> element;
        Optional<Web::HTML::CrossProcessId> frame;
        Optional<Web::HTML::CrossProcessId> root;
    };
    Vector<Guard> guards;
    guards.append({ page, activation.node, {}, local_root_id_of(*root) });
    for (auto const* navigable = root; navigable; navigable = navigable->parent()) {
        if (!navigable->has_remote_host())
            continue;
        auto parent_page = traversable().page_hosting(*navigable->parent());
        if (!parent_page || !parent_page->is_open())
            return refuse();
        guards.append({ *parent_page, {}, navigable->id(), local_root_id_of(*navigable->parent()) });
    }

    auto first_id = next_input_event_id();
    Vector<u64> event_ids { first_id, first_id + 1, first_id + 2 };
    HintClick click { session_id.as_integer<u64>(), {}, page, MonotonicTime::now() + hint_answer_timeout };
    auto now = MonotonicTime::now();
    m_hint_clicks.remove_all_matching([&](auto const&, auto const& click) { return click.deadline < now; });
    for (auto& guard : guards) {
        guard.page->async_hints_guard_click(request_id, guard.element, guard.frame, guard.root, event_ids);
        click.pages.append(guard.page);
    }
    m_hint_clicks.set(request_id, move(click));

    // NB: The events take the connection the guards took, after them.
    enqueue_input_event_bypassing_compositor(mouse_event(Web::MouseEvent::Type::MouseMove, position, MouseButton::None, MouseButton::None, KeyModifier::Mod_None));
    auto down = mouse_event(Web::MouseEvent::Type::MouseDown, position, MouseButton::Primary, MouseButton::Primary, KeyModifier::Mod_None);
    down.click_count = 1;
    enqueue_input_event_bypassing_compositor(move(down));
    auto up = mouse_event(Web::MouseEvent::Type::MouseUp, position, MouseButton::Primary, MouseButton::None, KeyModifier::Mod_None);
    up.click_count = 1;
    enqueue_input_event_bypassing_compositor(move(up));
    VERIFY(next_input_event_id() == first_id + 3);
}

void EmacsWebView::did_finish_hint_activation(WebContentPage& page, u64 request_id, bool clicked)
{
    auto click = m_hint_clicks.get(request_id);
    if (!click.has_value() || !any_of(click->pages, [&](auto const& guarding) { return guarding.ptr() == &page; }))
        return;
    if (clicked && click->element_page.ptr() != &page)
        return;
    auto finished = m_hint_clicks.take(request_id).release_value();
    if (clicked)
        return;

    // NB: A process that let the press through cancels it.
    for (auto& guarding : finished.pages) {
        if (guarding->is_open())
            guarding->async_hints_cancel_click(request_id);
    }
    send_hint_error("the element moved or was covered before the click, so nothing was activated"sv, JsonValue { finished.session_id });
}

Optional<u64> EmacsWebView::open_request(RequestKind kind, JsonObject request)
{
    static constexpr Array type_names = { "dialog"sv, "select"sv, "file_picker"sv, "color_picker"sv, "date_picker"sv, "permission"sv, "external_url"sv };
    auto type = type_names[to_underlying(kind)];

    auto id = m_server.next_request_id();
    request.set("type"sv, type);
    request.set("view"sv, m_id);
    request.set("id"sv, id);

    // What the client cannot be asked, the page is not left waiting for.
    if (EmacsProtocol::encode_control_record(request).is_error()) {
        dbgln("Emacs: view {}: a {} request does not fit a record", m_id, type);
        send_error(wire_formatted("a {} request of the page does not fit a record", type));
        return {};
    }

    send(request);
    m_open_requests.append({ id, kind, move(request) });
    return id;
}

bool EmacsWebView::open_dialog(Web::PendingDialog dialog, JsonObject request)
{
    withdraw_requests(RequestKind::Dialog);

    auto id = open_request(RequestKind::Dialog, move(request));
    if (!id.has_value()) {
        m_pending_dialog = dialog;
        close_pending_dialog(false, {});
        return false;
    }
    m_pending_dialog = dialog;
    m_dialog_request_id = id;
    return true;
}

void EmacsWebView::close_pending_dialog(bool accepted, Optional<Utf16String> prompt_response)
{
    m_dialog_request_id.clear();
    m_pending_prompt_text.clear();
    switch (exchange(m_pending_dialog, Web::PendingDialog::None)) {
    case Web::PendingDialog::None:
    case Web::PendingDialog::BeforeUnload:
        break;
    case Web::PendingDialog::Alert:
        alert_closed();
        break;
    case Web::PendingDialog::Confirm:
        confirm_closed(accepted);
        break;
    case Web::PendingDialog::Prompt:
        prompt_closed(accepted ? move(prompt_response) : Optional<Utf16String> {});
        break;
    }
}

Optional<EmacsWebView::OpenRequest> EmacsWebView::take_request(RequestKind kind, JsonObject const& answer)
{
    auto id = answer.get_u64("id"sv);
    auto index = m_open_requests.find_first_index_if([&](auto const& request) { return request.kind == kind && request.id == id; });
    if (!index.has_value())
        return {};
    return m_open_requests.take(*index);
}

// The request is over without an answer: the client is told to stop asking, and whoever waited for the answer is
// forgotten.
void EmacsWebView::withdraw_request(OpenRequest const& request)
{
    if (m_dialog_request_id == request.id) {
        m_dialog_request_id.clear();
        m_pending_dialog = Web::PendingDialog::None;
        m_pending_prompt_text.clear();
    }
    if (m_before_unload.has_value() && m_before_unload->request_id == request.id)
        m_before_unload.clear();
    m_permission_callbacks.remove(request.id);
    m_permission_requests.remove(request.id);
    m_external_url_callbacks.remove(request.id);

    auto cancelled = message("request_cancelled"sv);
    cancelled.set("id"sv, request.id);
    send(cancelled);
}

// The page asks one thing of a kind at a time, so a request the client never answered is over once another comes.
void EmacsWebView::withdraw_requests(RequestKind kind)
{
    for (size_t index = 0; index < m_open_requests.size();) {
        if (m_open_requests[index].kind == kind)
            withdraw_request(m_open_requests.take(index));
        else
            ++index;
    }
}

// Withdraws what a document asked. Permissions and external URLs are asked for the view, which ends those itself.
void EmacsWebView::withdraw_page_requests()
{
    for (auto kind : { RequestKind::Dialog, RequestKind::Select, RequestKind::FilePicker, RequestKind::ColorPicker, RequestKind::DatePicker })
        withdraw_requests(kind);
}

void EmacsWebView::handle_dialog_closed(JsonObject const& object)
{
    auto request = take_request(RequestKind::Dialog, object);
    if (!request.has_value())
        return;

    auto accepted = object.get_bool("accepted"sv).value_or(false);

    if (m_before_unload.has_value() && m_before_unload->request_id == request->id) {
        auto before_unload = m_before_unload.release_value();
        before_unload.on_complete(accepted);
        return;
    }
    if (m_dialog_request_id != request->id)
        return;

    Optional<Utf16String> prompt_response;
    if (auto text = object.get_string("text"sv); text.has_value())
        prompt_response = Utf16String::from_utf8(*text);
    else
        prompt_response = m_pending_prompt_text.value_or({});
    close_pending_dialog(accepted, move(prompt_response));
}

void EmacsWebView::handle_select_closed(JsonObject const& object)
{
    if (!take_request(RequestKind::Select, object).has_value())
        return;
    select_dropdown_closed(object.get_u32("item"sv));
}

void EmacsWebView::handle_file_picker_closed(JsonObject const& object)
{
    if (!take_request(RequestKind::FilePicker, object).has_value())
        return;

    Vector<Web::HTML::SelectedFile> files;
    if (auto paths = object.get_array("paths"sv); paths.has_value()) {
        paths->for_each([&](JsonValue const& path) {
            if (!path.is_string())
                return;
            auto file = create_selected_file(path.as_string().to_byte_string());
            if (file.is_error()) {
                send_error(wire_formatted("cannot read {}: {}", path.as_string(), file.error()));
                return;
            }
            files.append(file.release_value());
        });
    }
    file_picker_closed(move(files));
}

void EmacsWebView::handle_color_picker_closed(JsonObject const& object)
{
    if (!take_request(RequestKind::ColorPicker, object).has_value())
        return;

    Optional<Color> color;
    if (auto text = object.get_string("color"sv); text.has_value())
        color = Color::from_string(*text);
    color_picker_update(color, Web::HTML::ColorPickerUpdateState::Closed);
}

void EmacsWebView::handle_date_picker_closed(JsonObject const& object)
{
    if (!take_request(RequestKind::DatePicker, object).has_value())
        return;

    Optional<Utf16String> value;
    if (auto text = object.get_string("value"sv); text.has_value())
        value = Utf16String::from_utf8(*text);
    date_picker_closed(move(value));
}

void EmacsWebView::handle_permission_closed(JsonObject const& object)
{
    auto request = take_request(RequestKind::Permission, object);
    if (!request.has_value())
        return;

    m_permission_requests.remove(request->id);
    auto callback = m_permission_callbacks.take(request->id);
    if (!callback.has_value())
        return;

    auto outcome = Web::Permissions::PromptOutcome::Dismissed;
    auto name = object.get_string("outcome"sv).value_or({});
    if (name == "allow_once"sv)
        outcome = Web::Permissions::PromptOutcome::AllowOnce;
    else if (name == "allow_always"sv)
        outcome = Web::Permissions::PromptOutcome::AllowAlways;
    else if (name == "deny"sv)
        outcome = Web::Permissions::PromptOutcome::Block;
    (*callback)(outcome);
}

void EmacsWebView::handle_external_url_closed(JsonObject const& object)
{
    auto request = take_request(RequestKind::ExternalURL, object);
    if (!request.has_value())
        return;
    if (auto callback = m_external_url_callbacks.take(request->id); callback.has_value())
        (*callback)(object.get_bool("open"sv).value_or(false));
}

void EmacsWebView::did_request_context_menu(ContextMenuRequest request)
{
    auto url_or_null = [](Optional<Web::HTML::PreparedNavigationDescriptor> const& navigation) -> JsonValue {
        if (navigation.has_value())
            return wire_text(navigation->url);
        return JsonValue {};
    };

    auto menu = message("context_menu"sv);
    menu.set("x"sv, request.widget_position.x());
    menu.set("y"sv, request.widget_position.y());
    menu.set("link_url"sv, url_or_null(request.link_navigation));
    menu.set("image_url"sv, JsonValue {});
    menu.set("image_mime_type"sv, JsonValue {});
    menu.set("media_url"sv, JsonValue {});
    menu.set("media"sv, JsonValue {});
    menu.set("selection"sv, string_or_null(request.selected_text));
    menu.set("editable"sv, request.is_editable);

    switch (request.kind) {
    case ContextMenuKind::Page:
        menu.set("kind"sv, "page"sv);
        break;
    case ContextMenuKind::Link:
        menu.set("kind"sv, "link"sv);
        break;
    case ContextMenuKind::Image:
        menu.set("kind"sv, "image"sv);
        menu.set("image_url"sv, url_or_null(request.navigation));
        menu.set("image_mime_type"sv, string_or_null(request.image_mime_type));
        break;
    case ContextMenuKind::Media:
        menu.set("kind"sv, "media"sv);
        menu.set("media_url"sv, url_or_null(request.navigation));
        if (request.media.has_value()) {
            JsonObject media;
            media.set("playing"sv, request.media->is_playing);
            media.set("muted"sv, request.media->is_muted);
            media.set("loop"sv, request.media->is_looping);
            media.set("controls"sv, request.media->has_user_agent_controls);
            menu.set("media"sv, move(media));
        }
        break;
    }
    send(menu);
}

void EmacsWebView::insert_clipboard_item(Web::Clipboard::SystemClipboardItem item)
{
    for (auto const& representation : item.system_clipboard_representations) {
        auto const* text = representation.data.get_pointer<ByteString>();
        if (representation.name != "text/plain"sv || !text)
            continue;
        auto clipboard = message("clipboard"sv);
        clipboard.set("text"sv, wire_text(*text));
        send(clipboard);
        break;
    }
    HeadlessWebView::insert_clipboard_item(move(item));
}

// What the page or the browser would open in a tab or window of its own gets a view when the client takes those.
// Otherwise it is not shown: the client learns the URL, and may open it in a view.
ViewImplementation* EmacsWebView::create_view_for_new_tab_or_window(IsPrivate is_private)
{
    if (auto* opened = m_server.open_view({}, *this, {}))
        return opened;
    auto* view = HeadlessWebView::create_view_for_new_tab_or_window(is_private);
    if (view)
        watch_blocked_popup(static_cast<HeadlessWebView&>(*view));
    return view;
}

void EmacsWebView::watch_blocked_popup(HeadlessWebView& popup)
{
    auto popup_view_id = popup.view_id();

    popup.on_url_change = [this, popup_view_id](URL::URL const& url) {
        if (url != URL::about_blank())
            report_blocked_popup(popup_view_id, url);
    };

    // A window opened without a URL never gets one.
    auto timer = Core::Timer::create_single_shot(popup_url_timeout, [weak_this = make_weak_ptr<EmacsWebView>(), popup_view_id] {
        if (weak_this)
            weak_this->report_blocked_popup(popup_view_id, {});
    });
    timer->start();
    m_popup_timers.set(popup_view_id, move(timer));
}

void EmacsWebView::report_blocked_popup(u64 popup_view_id, Optional<URL::URL> const& url)
{
    auto timer = m_popup_timers.take(popup_view_id);
    if (!timer.has_value())
        return;
    // The timer may be the one whose callback this is.
    Core::deferred_invoke([timer = timer.release_value()] { });

    auto blocked = message("popup_blocked"sv);
    if (url.has_value())
        blocked.set("url"sv, wire_text(*url));
    else
        blocked.set("url"sv, JsonValue {});
    send(blocked);

    // Deferred, as the popup may be the view whose callback this is.
    Core::deferred_invoke([weak_this = make_weak_ptr<EmacsWebView>()] {
        if (weak_this)
            weak_this->close_child_web_views();
    });
}

}
