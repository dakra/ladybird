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
#include <AK/Random.h>
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
#include <LibWebCommon/WebView/Debugger.h>
#include <LibWebView/Application.h>
#include <LibWebView/EmacsProtocol.h>
#include <LibWebView/EmacsWebView.h>
#include <LibWebView/PageBackingStores.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
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

// The Compositor pads a backing store by 256 pixels each way while a view is resized, and a Metal texture is at most
// 16384 pixels wide or high.
static constexpr int maximum_viewport_dimension = 16000;
// A frame of this many pixels takes 256 MiB, of which the Compositor keeps three.
static constexpr i64 maximum_viewport_area = 64'000'000;
static constexpr double minimum_device_pixel_ratio = 0.25;
static constexpr double maximum_device_pixel_ratio = 8.0;

static constexpr double maximum_wheel_delta = 1'000'000;
static constexpr int maximum_history_delta = 10'000;

// Text that comes from a page is cut to this many bytes of UTF-8 before it goes into a control message, which keeps
// every message well within a record.
static constexpr size_t maximum_text_bytes = 1 * MiB;
static constexpr size_t maximum_favicon_bytes = 1 * MiB;

static constexpr auto popup_url_timeout = AK::Duration::from_seconds(2);

static EmacsServer* s_the_server;

// These make text fit for the wire: valid UTF-8, with a replacement character for what is not, of a bounded length.
static String wire_text(StringView text)
{
    if (text.length() > maximum_text_bytes) {
        auto length = maximum_text_bytes;
        while (length > 0 && (static_cast<u8>(text[length]) & 0xc0) == 0x80)
            --length;
        text = text.substring_view(0, length);
    }
    return String::from_utf8_with_replacement_character(text, String::WithBOMHandling::No);
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

struct Viewport {
    Web::DevicePixelSize size;
    double device_pixel_ratio { 1.0 };
};

// The size is brought within what the Compositor can allocate. A device pixel ratio out of range is refused.
static ErrorOr<Viewport> viewport_from(JsonObject const& object, Viewport fallback)
{
    auto device_pixel_ratio = fallback.device_pixel_ratio;
    if (object.has("dpr"sv)) {
        auto requested = number(object, "dpr"sv);
        if (!requested.has_value() || !isfinite(*requested) || *requested < minimum_device_pixel_ratio || *requested > maximum_device_pixel_ratio)
            return Error::from_string_literal("dpr must be a number from 0.25 to 8");
        device_pixel_ratio = *requested;
    }

    auto width = clamp(integer(object, "width"sv, fallback.size.width().value()), 1, maximum_viewport_dimension);
    auto height = clamp(integer(object, "height"sv, fallback.size.height().value()), 1, maximum_viewport_dimension);
    height = min(height, static_cast<int>(maximum_viewport_area / width));
    return Viewport { { width, height }, device_pixel_ratio };
}

// Writes a file that only this user reads. Without a path, the file gets a name in /tmp, where anyone can put a file or
// a link at a name, so that one is a file this process creates.
static ErrorOr<ByteString> write_private_file(Optional<ByteString> const& path, StringView default_name, ReadonlyBytes contents)
{
    auto fd = -1;
    auto written_path = path.value_or({});
    if (path.has_value()) {
        fd = ::open(path->characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    } else {
        written_path = ByteString::formatted("/tmp/{}", default_name);
        (void)::unlink(written_path.characters());
        fd = ::open(written_path.characters(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    }
    if (fd < 0)
        return Error::from_syscall("open"sv, errno);

    auto file = TRY(Core::File::adopt_fd(fd, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(contents));
    return written_path;
}

NonnullOwnPtr<EmacsWebView> EmacsWebView::create(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, double device_pixel_ratio, EmacsServer& server, u32 id)
{
    auto view = adopt_own(*new EmacsWebView(move(theme), viewport_size, device_pixel_ratio, server, id));
    view->initialize_tab(Web::HTML::VisibilityState::Visible);
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
        send(cursor_message);
    };

    on_link_hover = [this](URL::URL const& url) {
        auto hover = message("link_hover"sv);
        hover.set("url"sv, wire_text(url));
        send(hover);
    };
    on_link_unhover = [this] {
        auto hover = message("link_hover"sv);
        hover.set("url"sv, JsonValue {});
        send(hover);
    };

    on_enter_tooltip_area = [this](ByteString const& text) {
        auto tooltip = message("tooltip"sv);
        tooltip.set("text"sv, wire_text(text));
        send(tooltip);
    };
    on_leave_tooltip_area = [this] {
        auto tooltip = message("tooltip"sv);
        tooltip.set("text"sv, JsonValue {});
        send(tooltip);
    };

    on_favicon_change = [this](Optional<Gfx::Bitmap const&> bitmap) {
        auto favicon = message("favicon"sv);
        favicon.set("png"sv, JsonValue {});
        if (bitmap.has_value()) {
            auto png = Gfx::PNGWriter::encode(*bitmap);
            if (png.is_error() || png.value().size() > maximum_favicon_bytes) {
                dbgln("Emacs: not sending a favicon that cannot be encoded or is too large");
            } else if (auto base64 = encode_base64(png.value()); !base64.is_error()) {
                favicon.set("png"sv, base64.release_value());
            }
        }
        send(favicon);
    };

    on_page_background_color_change = [this](Gfx::Color color) {
        if (m_last_sent_background_color == color || !m_client_knows_view)
            return;
        m_last_sent_background_color = color;
        auto background = message("background_color"sv);
        background.set("rgb"sv, wire_formatted("#{:02x}{:02x}{:02x}", color.red(), color.green(), color.blue()));
        send(background);
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
        end_hint_session(HintSessionEnd::PageGone);
        // NB: The process that was to answer them is gone.
        m_pending_hint_scripts.clear();
        settle_console_result_callback({});
        if (m_pending_screenshot.has_value()) {
            m_pending_screenshot.clear();
            send_error("cannot take a screenshot: the page crashed"sv);
        }

        auto crashed = message("crashed"sv);
        crashed.set("reason"sv, reason == WebContentCrashReason::RejectedIPC ? "rejected_ipc"sv : "process_crash"sv);
        send(crashed);
        m_frame_needs_full = true;
    };

    on_top_level_navigation_commit = [this] {
        // What a document asked is over once another document takes its place.
        auto document_id = traversable().active_document().id();
        if (m_active_document_id == document_id)
            return;
        m_active_document_id = document_id;
        withdraw_page_requests();
        ignore_hint_scripts();
        end_hint_session(HintSessionEnd::Navigated);
    };

    on_close = [this] {
        withdraw_page_requests();
        end_hint_session(HintSessionEnd::PageGone);
        auto closed = message("view_closed"sv);
        closed.set("reason"sv, "closed"sv);
        send(closed);
        m_server.view_did_close({});
    };

    on_new_web_view = [this](auto, auto, CanonicalTraversable& traversable) {
        // The page gets the window it asked for, which nobody is shown and which closes once its URL is known.
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

void EmacsWebView::did_block_download(StringView name)
{
    send_error(wire_formatted("download blocked: {}", name));
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
    send(state);
}

void EmacsWebView::client_went_away()
{
    // The labels were for the client that asked for them.
    end_hint_session(HintSessionEnd::Silently);
    m_client_knows_view = false;
    m_frame_in_flight = false;
    m_frame_needs_full = true;
    m_remaining_bands.clear();
    m_shared_frame_buffer = nullptr;
    m_retired_shared_frame_buffer = nullptr;
    m_held_mouse_buttons = MouseButton::None;
}

void EmacsWebView::client_changed()
{
    client_went_away();
    m_client_knows_view = true;
    m_last_sent_state = {};
    m_last_sent_background_color.clear();
    m_last_sent_input_method_state = {};
    send_state();
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
    // The client has one frame record at a time. What changes meanwhile is sent once it has acknowledged that.
    if (!m_client_knows_view || m_frame_in_flight)
        return;

    auto frame = backing_stores().displayed_frame();
    if (!frame.has_value() || !frame->shared_image_buffer)
        return;
    auto bitmap = frame->shared_image_buffer->bitmap_if_present();
    if (!bitmap)
        return;

    // NB: A backing store can be larger than the frame presented in it.
    Gfx::IntSize frame_size { min(frame->size.width(), bitmap->width()), min(frame->size.height(), bitmap->height()) };
    if (auto result = send_frame(*bitmap, frame_size, frame->device_pixel_ratio); result.is_error()) {
        // What the frame would have shown is sent with the next one, whole.
        dbgln("Emacs: cannot send a frame: {}", result.error());
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
            dbgln("Emacs: cannot share a frame in memory: {}", announced_new_segment.error());
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
        dbgln("Emacs: whole frame {}x{}@{} as {}: {} bytes in {} ms", frame_size.width(), frame_size.height(), device_pixel_ratio, encoding_names[to_underlying(encoding)], record.size(), (MonotonicTime::now() - encoding_started).to_milliseconds());
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
    if (m_frame_needs_full || m_remaining_bands.has_value() || m_pending_damage.has_value())
        schedule_frame_flush();
}

void EmacsWebView::set_viewport(JsonObject const& object)
{
    auto viewport = viewport_from(object, { m_viewport_size, m_device_pixel_ratio });
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
        auto visible = object.get_bool("visible"sv).value_or(true);
        set_system_visibility_state(visible ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
        if (visible)
            schedule_frame_flush();
    } else if (type == "set_focus"sv) {
        set_has_system_focus(object.get_bool("focused"sv).value_or(true));
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
            dbgln("Emacs: unknown key name '{}'", *name);
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
            default_name = ByteString::formatted("ladybird-emacs-{}-screenshot-{}.png", getpid(), ++m_screenshots_taken);
        return write_private_file(screenshot.path, default_name, png);
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

// The script that finds what a hint session labels. It runs in the page, so it treats every element alike and returns
// what it found as JSON; the server checks all of it. It leaves the elements, and the functions that draw, narrow and
// remove their labels, under a global property whose name the server made up.
static constexpr auto hint_collect_script = R"~~~((function () {
    "use strict";
    var w = window, d = document, name = "@NAME@";
    var elements = [], items = [], host = null, labels = [];
    var roles = " button link tab menuitem checkbox radio switch option ";
    var textTypes = " text search email url tel password number ";
    var hostStyle = ["all", "initial", "position", "absolute", "left", "0", "top", "0", "width", "0", "height", "0",
        "margin", "0", "padding", "0", "border", "0", "display", "block", "overflow", "visible", "z-index", "2147483647",
        "pointer-events", "none", "visibility", "visible", "opacity", "1", "transform", "none", "filter", "none"];
    var labelStyle = "position:absolute;display:block;box-sizing:border-box;margin:0;padding:0 2px;" +
        "border:1px solid #a08a00;border-radius:2px;background:#fff3a3;color:#1a1a1a;" +
        "font:bold 11px/13px monospace;letter-spacing:0;white-space:nowrap;text-transform:none;" +
        "text-decoration:none;pointer-events:none;";

    function roleOf(el) {
        var role = el.getAttribute("role");
        return role ? role.trim().split(/\s+/)[0].toLowerCase() : "";
    }
    function isTextField(el) {
        var tag = el.localName;
        if (tag === "textarea")
            return true;
        if (tag === "input")
            return textTypes.indexOf(" " + el.type + " ") >= 0;
        return el.hasAttribute("contenteditable") && el.isContentEditable === true;
    }
    function canActivate(el) {
        var tag = el.localName;
        if ((tag === "a" || tag === "area") && el.hasAttribute("href"))
            return true;
        if (tag === "input")
            return el.type !== "hidden";
        if (tag === "button" || tag === "select" || tag === "textarea" || tag === "summary" || tag === "label")
            return true;
        var editable = el.getAttribute("contenteditable");
        if (editable !== null && editable.toLowerCase() !== "false")
            return true;
        if (el.hasAttribute("onclick"))
            return true;
        var tabindex = el.getAttribute("tabindex");
        if (tabindex !== null && parseInt(tabindex, 10) >= 0)
            return true;
        var role = roleOf(el);
        return role !== "" && roles.indexOf(" " + role + " ") >= 0;
    }
    function isDisabled(el) {
        try {
            return el.matches(":disabled");
        } catch (e) {
            return false;
        }
    }
    function kindOf(el) {
        var tag = el.localName, role = roleOf(el);
        if (((tag === "a" || tag === "area") && el.hasAttribute("href")) || role === "link")
            return "link";
        if (isTextField(el))
            return "input";
        if (tag === "button" || tag === "summary" || role === "button")
            return "button";
        if (tag === "input" && " button submit reset image ".indexOf(" " + el.type + " ") >= 0)
            return "button";
        return "other";
    }
    function clean(text) {
        if (typeof text !== "string")
            return "";
        text = text.replace(/\s+/g, " ").trim();
        if (text.length > 200) {
            text = text.slice(0, 200);
            var last = text.charCodeAt(199);
            if (last >= 0xd800 && last <= 0xdbff)
                text = text.slice(0, 199);
        }
        return typeof text.toWellFormed === "function" ? text.toWellFormed() : text;
    }
    function labelText(el) {
        var found = el.labels;
        return found && found.length ? found[0].innerText : "";
    }
    function textOf(el) {
        var tag = el.localName, text = el.getAttribute("aria-label");
        if (clean(text))
            return clean(text);
        if (tag === "input") {
            var type = el.type;
            if (type === "submit" || type === "reset" || type === "button")
                text = el.value;
            else if (type === "image")
                text = el.alt;
            else if (type === "checkbox" || type === "radio" || type === "password" || type === "file")
                text = "";
            else
                text = el.value;
            if (!clean(text))
                text = labelText(el) || el.placeholder || el.title;
        } else if (tag === "textarea") {
            text = el.value || labelText(el) || el.placeholder || el.title;
        } else if (tag === "select") {
            var selected = el.selectedOptions;
            text = (selected && selected.length ? selected[0].text : "") || labelText(el) || el.title;
        } else {
            text = el.innerText;
            if (!clean(text)) {
                var image = el.querySelector("img[alt]");
                text = (image && image.alt) || el.title || "";
            }
        }
        return clean(text);
    }
    function urlOf(el) {
        var tag = el.localName;
        if ((tag !== "a" && tag !== "area") || !el.hasAttribute("href"))
            return null;
        var url = el.href;
        if (typeof url !== "string") {
            try {
                url = new URL(el.getAttribute("href"), d.baseURI).href;
            } catch (e) {
                return null;
            }
        }
        if (typeof url !== "string" || url.length > 16384)
            return null;
        return typeof url.toWellFormed === "function" ? url.toWellFormed() : url;
    }
    function intersects(rect, width, height) {
        return rect.right > 0 && rect.bottom > 0 && rect.left < width && rect.top < height;
    }
    function placeOf(el, width, height) {
        var rects = el.getClientRects();
        for (var i = 0; i < rects.length; ++i) {
            if (rects[i].width > 0 && rects[i].height > 0 && intersects(rects[i], width, height))
                return rects[i];
        }
        return el.getBoundingClientRect();
    }
    function clearLabels() {
        if (host && host.parentNode)
            host.parentNode.removeChild(host);
        host = null;
        labels = [];
    }
    function narrow(mask, typed) {
        for (var i = 0; i < labels.length; ++i) {
            var label = labels[i];
            label.typed.textContent = label.text.slice(0, typed);
            label.rest.textContent = label.text.slice(typed);
            label.box.style.display = mask.charAt(i) === "1" ? "block" : "none";
        }
        return "narrowed";
    }
    function draw(texts, mask, typed) {
        clearLabels();
        var root = d.documentElement;
        if (!root)
            return "no document";
        host = d.createElement("@HOST@");
        for (var i = 0; i < hostStyle.length; i += 2)
            host.style.setProperty(hostStyle[i], hostStyle[i + 1], "important");
        var shadow = host.attachShadow({ mode: "closed" });
        var width = w.innerWidth, height = w.innerHeight, places = [];
        for (i = 0; i < texts.length && i < elements.length; ++i) {
            var box = d.createElement("span"), typedPart = d.createElement("span"), rest = d.createElement("span");
            box.style.cssText = labelStyle;
            typedPart.style.cssText = "color:#d04000";
            box.appendChild(typedPart);
            box.appendChild(rest);
            shadow.appendChild(box);
            labels.push({ box: box, typed: typedPart, rest: rest, text: texts[i] });
            places.push(placeOf(elements[i], width, height));
        }
        narrow(texts.map(function () { return "1"; }).join(""), 0);
        root.appendChild(host);
        var origin = host.getBoundingClientRect(), sizes = [];
        for (i = 0; i < labels.length; ++i)
            sizes.push([labels[i].box.offsetWidth, labels[i].box.offsetHeight]);
        for (i = 0; i < labels.length; ++i) {
            var left = Math.max(0, Math.min(places[i].left, width - sizes[i][0]));
            var top = Math.max(0, Math.min(places[i].top, height - sizes[i][1]));
            labels[i].box.style.left = (left - origin.left) + "px";
            labels[i].box.style.top = (top - origin.top) + "px";
        }
        narrow(mask, typed);
        return "drawn";
    }
    function placeCaret(el) {
        try {
            if (el.localName === "input" || el.localName === "textarea") {
                var length = el.value.length;
                el.setSelectionRange(length, length);
            } else {
                var selection = w.getSelection();
                selection.selectAllChildren(el);
                selection.collapseToEnd();
            }
        } catch (e) {
        }
    }
    function clickPoint(el) {
        var width = w.innerWidth, height = w.innerHeight, rects = el.getClientRects(), fractions = [0.5, 0.25, 0.75];
        for (var i = 0; i < rects.length; ++i) {
            var left = Math.max(rects[i].left, 0), right = Math.min(rects[i].right, width);
            var top = Math.max(rects[i].top, 0), bottom = Math.min(rects[i].bottom, height);
            if (right - left < 1 || bottom - top < 1)
                continue;
            for (var row = 0; row < fractions.length; ++row) {
                for (var column = 0; column < fractions.length; ++column) {
                    var x = left + (right - left) * fractions[column], y = top + (bottom - top) * fractions[row];
                    var hit = d.elementFromPoint(x, y);
                    while (hit && hit.shadowRoot && typeof hit.shadowRoot.elementFromPoint === "function") {
                        var inner = hit.shadowRoot.elementFromPoint(x, y);
                        if (!inner || inner === hit)
                            break;
                        hit = inner;
                    }
                    if (hit && (hit === el || el.contains(hit)))
                        return [x, y];
                }
            }
        }
        return null;
    }
    function end(index) {
        clearLabels();
        try {
            delete w[name];
        } catch (e) {
        }
        var el = index >= 0 && index < elements.length ? elements[index] : null;
        elements = [];
        if (!el || !el.isConnected)
            return JSON.stringify({ action: "none" });
        if (isTextField(el)) {
            el.focus();
            placeCaret(el);
            return JSON.stringify({ action: "focus" });
        }
        try {
            el.focus({ preventScroll: true });
        } catch (e) {
        }
        var point = clickPoint(el);
        if (point)
            return JSON.stringify({ action: "click", x: point[0], y: point[1] });
        if (typeof el.click === "function")
            el.click();
        else
            el.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true, view: w }));
        return JSON.stringify({ action: "script" });
    }

    var selector = "a,area,button,input,select,textarea,summary,label,[contenteditable],[onclick],[tabindex],[role]";
    var width = w.innerWidth, height = w.innerHeight;
    function consider(el) {
        if (!el.matches(selector) || !canActivate(el) || isDisabled(el))
            return;
        var rect = el.getBoundingClientRect();
        if (!(rect.width >= 1 && rect.height >= 1 && intersects(rect, width, height)))
            return;
        var style = w.getComputedStyle(el);
        if (style.visibility !== "visible" || style.display === "none")
            return;
        elements.push(el);
        items.push({ kind: kindOf(el), text: textOf(el), url: urlOf(el), x: rect.left, y: rect.top, width: rect.width, height: rect.height });
    }
    // An element of an open shadow root comes right after its host.
    function walk(root) {
        var all = root.querySelectorAll("*");
        for (var i = 0; i < all.length && elements.length < @LIMIT@; ++i) {
            consider(all[i]);
            if (all[i].shadowRoot)
                walk(all[i].shadowRoot);
        }
    }
    if (d.documentElement)
        walk(d);
    Object.defineProperty(w, name, { value: { draw: draw, narrow: narrow, end: end }, configurable: true });
    return JSON.stringify({ items: items });
})())~~~"sv;

// What every hint script is run inside: whatever the page did to the script, the answer is the token of the request,
// a newline, and a string of bounded length.
static constexpr auto hint_script_wrapper = R"~~~((function () {
    var token = "@TOKEN@", result = null;
    try {
        result = @BODY@;
    } catch (e) {
        result = null;
    }
    if (typeof result !== "string" || result.length > @MAXIMUM@)
        result = "null";
    return token + "\n" + result;
})())~~~"sv;

static constexpr auto hint_answer_timeout = AK::Duration::from_seconds(5);
// How long the token of an unanswered script is known after its answer is due, so that a late answer is still taken.
static constexpr auto hint_token_lifetime = AK::Duration::from_seconds(60);
// The longest answer a hint script gives, in UTF-16 code units: a JSON item per element, of at most 200 characters of
// text and 16 KiB of URL.
static constexpr size_t maximum_hint_answer_length = 20'000'000;

static String random_hex_name()
{
    return MUST(String::formatted("{:016x}{:016x}", get_random<u64>(), get_random<u64>()));
}

static String replace_placeholder(String const& text, StringView placeholder, StringView value)
{
    return MUST(text.replace(placeholder, value, ReplaceMode::All));
}

void EmacsWebView::run_hint_script(StringView body, HintScript kind)
{
    auto token = random_hex_name();
    auto script = MUST(String::from_utf8(hint_script_wrapper));
    script = replace_placeholder(script, "@TOKEN@"sv, token);
    script = replace_placeholder(script, "@MAXIMUM@"sv, String::number(maximum_hint_answer_length));
    script = replace_placeholder(script, "@BODY@"sv, body);

    auto now = MonotonicTime::now();
    m_pending_hint_scripts.remove_all_matching([&](auto const&, auto const& pending) { return pending.deadline + hint_token_lifetime < now; });
    m_pending_hint_scripts.set(token, { kind, m_hint_session.has_value() ? m_hint_session->serial : 0, now + hint_answer_timeout });

    install_hint_dispatcher();
    js_console_input(script);
}

// The console's answers also go to DevTools, which puts its own callback in place while it evaluates something and
// removes it once it has its answer. While a hint script is unanswered, the server's callback is in place: it takes the
// answers that start with the token of one of its scripts, and passes on the others to the callback it replaced.
void EmacsWebView::install_hint_dispatcher()
{
    if (m_hint_dispatcher_installed && on_received_js_console_result)
        return;
    m_console_result_callback_before_hints = move(on_received_js_console_result);
    on_received_js_console_result = [this](DebuggerValue result) { dispatch_console_result(move(result)); };
    m_hint_dispatcher_installed = true;
}

void EmacsWebView::dispatch_console_result(DebuggerValue result)
{
    Function<void(DebuggerValue)> callback;
    if (!did_receive_hint_script_result(result) && m_console_result_callback_before_hints) {
        // NB: DevTools' callback removes the one in place, which is this one, once it is called.
        callback = move(m_console_result_callback_before_hints);
        callback(move(result));
    }

    // NB: The callback in place cannot be replaced while it runs.
    Core::deferred_invoke([weak_this = make_weak_ptr<EmacsWebView>(), callback = move(callback)]() mutable {
        if (weak_this)
            weak_this->settle_console_result_callback(move(callback));
    });
}

void EmacsWebView::settle_console_result_callback(Function<void(DebuggerValue)> callback_that_was_called)
{
    if (!on_received_js_console_result)
        m_hint_dispatcher_installed = false;
    else if (callback_that_was_called)
        m_console_result_callback_before_hints = move(callback_that_was_called);

    auto now = MonotonicTime::now();
    m_pending_hint_scripts.remove_all_matching([&](auto const&, auto const& pending) { return pending.deadline + hint_token_lifetime < now; });
    if (!m_pending_hint_scripts.is_empty()) {
        install_hint_dispatcher();
    } else if (m_hint_dispatcher_installed) {
        on_received_js_console_result = move(m_console_result_callback_before_hints);
        m_hint_dispatcher_installed = false;
    }
}

bool EmacsWebView::did_receive_hint_script_result(DebuggerValue const& result)
{
    if (result.type != DebuggerValueType::String || result.text.length_in_code_units() > maximum_hint_answer_length + 64)
        return false;
    auto text = String::from_utf16_with_replacement_character(result.text.utf16_view());
    if (text.is_error())
        return false;

    auto answer = text.value().bytes_as_string_view();
    auto newline = answer.find('\n');
    if (!newline.has_value())
        return false;
    auto pending = m_pending_hint_scripts.take(MUST(String::from_utf8(answer.substring_view(0, *newline))));
    if (!pending.has_value())
        return false;
    if (pending->kind == HintScript::Ignore || pending->deadline < MonotonicTime::now())
        return true;
    auto payload = answer.substring_view(*newline + 1);

    if (pending->kind == HintScript::Activate) {
        did_end_hints(payload);
        return true;
    }
    if (!m_hint_session.has_value() || m_hint_session->serial != pending->session)
        return true;
    if (pending->kind == HintScript::Collect)
        did_collect_hints(payload);
    else
        did_draw_hints(payload);
    return true;
}

// The answers of the scripts that were sent are not acted on.
void EmacsWebView::ignore_hint_scripts()
{
    for (auto& it : m_pending_hint_scripts)
        it.value.kind = HintScript::Ignore;
}

EmacsWebView::HintSession* EmacsWebView::hint_session_named_by(JsonObject const& object)
{
    auto id = EmacsProtocol::hint_session_id(object.get("id"sv).value_or({}));
    if (!m_hint_session.has_value() || m_hint_session->id != id)
        return nullptr;
    return &*m_hint_session;
}

// The error names the session the refused message did, so that the client can end it.
void EmacsWebView::send_hint_error(StringView text, JsonObject const& request)
{
    auto error = message("error"sv);
    error.set("message"sv, wire_text(text));
    if (auto id = request.get("id"sv); id.has_value())
        error.set("id"sv, *id);
    send(error);
}

void EmacsWebView::handle_hints(JsonObject const& object)
{
    auto id = EmacsProtocol::hint_session_id(object.get("id"sv).value_or({}));
    if (!id.has_value()) {
        send_hint_error("hints needs an id that is a non-negative integer below 2^53"sv, object);
        return;
    }
    auto alphabet = object.get_string("alphabet"sv);
    if (!alphabet.has_value() || !EmacsProtocol::is_valid_hint_alphabet(*alphabet)) {
        send_hint_error("hints needs an alphabet of 2 to 36 distinct characters of a-z and 0-9"sv, object);
        return;
    }

    end_hint_session(HintSessionEnd::Silently);

    auto serial = m_next_hint_session_serial++;
    auto timeout = Core::Timer::create_single_shot(hint_answer_timeout, [weak_this = make_weak_ptr<EmacsWebView>(), serial] {
        if (!weak_this || !weak_this->m_hint_session.has_value() || weak_this->m_hint_session->serial != serial || weak_this->m_hint_session->listed)
            return;
        dbgln("Emacs: the page did not answer a hints request in time");
        weak_this->end_hint_session(HintSessionEnd::PageFailed);
    });
    m_hint_session = HintSession {
        .serial = serial,
        .id = *id,
        .name = MUST(String::formatted("__ladybird_hints_{}", random_hex_name())),
        .alphabet = *alphabet,
        .collection = {},
        .labels = {},
        .listed = false,
        .prefix = {},
        .timeout = timeout,
    };
    timeout->start();

    auto script = MUST(String::from_utf8(hint_collect_script));
    script = replace_placeholder(script, "@NAME@"sv, m_hint_session->name);
    script = replace_placeholder(script, "@HOST@"sv, MUST(String::formatted("ladybird-hints-{}", random_hex_name())));
    script = replace_placeholder(script, "@LIMIT@"sv, String::number(EmacsProtocol::maximum_hint_count + 1));
    run_hint_script(script, HintScript::Collect);
}

void EmacsWebView::did_collect_hints(StringView json)
{
    auto collection = EmacsProtocol::parse_hint_collection(json);
    if (collection.is_error()) {
        dbgln("Emacs: the page's hint script answered with something unusable: {}", collection.error());
        end_hint_session(HintSessionEnd::PageFailed);
        return;
    }

    auto& session = *m_hint_session;
    if (collection.value().items.is_empty()) {
        send_hints_list({}, false);
        end_hint_session(HintSessionEnd::Silently);
        return;
    }

    session.labels = EmacsProtocol::hint_labels(session.alphabet, collection.value().items.size());
    session.collection = collection.release_value();

    JsonArray labels;
    for (auto const& label : session.labels)
        labels.must_append(label);
    auto mask = EmacsProtocol::hint_label_mask(session.labels, session.prefix);
    auto typed_length = mask.contains('1') ? session.prefix.bytes().size() : 0;
    auto body = MUST(String::formatted("(function () {{ var hints = window[\"{}\"]; return hints ? hints.draw({}, \"{}\", {}) : null; }})()",
        session.name, labels.serialized(), mask, typed_length));
    run_hint_script(body, HintScript::Draw);
}

void EmacsWebView::did_draw_hints(StringView result)
{
    if (result != "drawn"sv) {
        dbgln("Emacs: the page's hint script did not draw the labels: {}", result.substring_view(0, min(result.length(), 100uz)));
        end_hint_session(HintSessionEnd::PageFailed);
        return;
    }

    auto& session = *m_hint_session;
    auto collection = session.collection.release_value();
    JsonArray items;
    for (size_t index = 0; index < collection.items.size(); ++index) {
        auto const& item = collection.items[index];
        JsonObject object;
        object.set("index"sv, static_cast<u64>(index));
        object.set("label"sv, session.labels[index]);
        object.set("kind"sv, EmacsProtocol::hint_kind_name(item.kind));
        object.set("text"sv, item.text);
        object.set("url"sv, string_or_null(item.url));
        object.set("x"sv, item.x);
        object.set("y"sv, item.y);
        object.set("width"sv, item.width);
        object.set("height"sv, item.height);
        items.must_append(move(object));
    }
    session.listed = true;
    if (session.timeout) {
        session.timeout->stop();
        session.timeout = nullptr;
    }
    send_hints_list(move(items), collection.truncated);
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
        send_hint_error("hints_narrow names no hint session of the view"sv, object);
        return;
    }
    // NB: Until the page has answered, the labels are at most as long as those of the most items there can be.
    auto label_length = session->labels.is_empty()
        ? EmacsProtocol::hint_label_length(session->alphabet.bytes().size(), EmacsProtocol::maximum_hint_count)
        : session->labels.first().bytes().size();
    auto prefix = object.get_string("prefix"sv);
    if (!prefix.has_value() || !EmacsProtocol::is_valid_hint_prefix(*prefix, session->alphabet, label_length)) {
        send_hint_error("hints_narrow needs a prefix of characters of the alphabet, no longer than the labels"sv, object);
        return;
    }

    session->prefix = *prefix;
    // NB: Labels that are yet to be drawn are drawn narrowed.
    if (session->labels.is_empty())
        return;
    auto mask = EmacsProtocol::hint_label_mask(session->labels, session->prefix);
    auto typed_length = mask.contains('1') ? session->prefix.bytes().size() : 0;
    run_hint_script(MUST(String::formatted("(function () {{ var hints = window[\"{}\"]; return hints ? hints.narrow(\"{}\", {}) : null; }})()",
        session->name, mask, typed_length)));
}

void EmacsWebView::handle_hints_end(JsonObject const& object)
{
    auto* session = hint_session_named_by(object);
    if (!session) {
        send_hint_error("hints_end names no hint session of the view"sv, object);
        return;
    }

    Optional<size_t> index;
    if (session->listed)
        index = EmacsProtocol::hint_activation_index(object.get("activate"sv).value_or({}), session->labels.size());
    end_hint_session(HintSessionEnd::Silently, index);
}

void EmacsWebView::end_hint_session(HintSessionEnd how, Optional<size_t> activate)
{
    if (!m_hint_session.has_value())
        return;
    auto session = m_hint_session.release_value();
    if (session.timeout) {
        session.timeout->stop();
        // NB: The timer may be the one whose callback this is.
        Core::deferred_invoke([timer = move(session.timeout)] { });
    }
    for (auto& it : m_pending_hint_scripts) {
        if (it.value.session == session.serial && it.value.kind != HintScript::Activate)
            it.value.kind = HintScript::Ignore;
    }

    if (how != HintSessionEnd::Silently) {
        auto cancelled = message("hints_cancelled"sv);
        cancelled.set("id"sv, session.id);
        send(cancelled);
        if (how == HintSessionEnd::PageGone)
            return;
    }

    // NB: Scripts run in the order they are sent, so this one finds what the session's earlier ones left, in whichever
    //     document they ran.
    auto index = activate.has_value() ? static_cast<i64>(*activate) : -1;
    run_hint_script(MUST(String::formatted("(function () {{ var hints = window[\"{}\"]; return hints ? hints.end({}) : null; }})()", session.name, index)),
        activate.has_value() ? HintScript::Activate : HintScript::Ignore);
}

void EmacsWebView::did_end_hints(StringView json)
{
    auto answer = JsonValue::from_string(json);
    if (answer.is_error() || !answer.value().is_object())
        return;
    auto const& object = answer.value().as_object();
    if (object.get_string("action"sv) != "click"sv)
        return;

    auto x = number(object, "x"sv);
    auto y = number(object, "y"sv);
    if (!x.has_value() || !y.has_value() || !isfinite(*x) || !isfinite(*y))
        return;

    // NB: The page reports CSS pixels of its viewport, where the element is. A point outside the view is no point of it.
    auto scale = m_device_pixel_ratio * zoom_level();
    auto device_x = round(*x * scale);
    auto device_y = round(*y * scale);
    auto viewport = viewport_size().to_type<int>();
    if (device_x < 0 || device_y < 0 || device_x >= viewport.width() || device_y >= viewport.height()) {
        dbgln("Emacs: not clicking at {},{}, which is outside the view", device_x, device_y);
        return;
    }
    click_at({ static_cast<int>(device_x), static_cast<int>(device_y) });
}

// A click as the user's mouse would make it, so that the page sees a trusted event with user activation.
void EmacsWebView::click_at(Gfx::IntPoint position)
{
    enqueue_input_event(mouse_event(Web::MouseEvent::Type::MouseMove, position, MouseButton::None, MouseButton::None, KeyModifier::Mod_None));
    auto down = mouse_event(Web::MouseEvent::Type::MouseDown, position, MouseButton::Primary, MouseButton::Primary, KeyModifier::Mod_None);
    down.click_count = 1;
    enqueue_input_event(move(down));
    auto up = mouse_event(Web::MouseEvent::Type::MouseUp, position, MouseButton::Primary, MouseButton::None, KeyModifier::Mod_None);
    up.click_count = 1;
    enqueue_input_event(move(up));
}

Optional<u64> EmacsWebView::open_request(RequestKind kind, JsonObject request)
{
    static constexpr Array type_names = { "dialog"sv, "select"sv, "file_picker"sv, "color_picker"sv, "date_picker"sv, "permission"sv, "external_url"sv };
    auto type = type_names[to_underlying(kind)];

    auto id = m_next_request_id++;
    request.set("type"sv, type);
    request.set("view"sv, m_id);
    request.set("id"sv, id);

    // What the client cannot be asked, the page is not left waiting for.
    if (EmacsProtocol::encode_control_record(request).is_error()) {
        dbgln("Emacs: a {} request does not fit a record", type);
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

// What the page or the browser would open in a tab or window of its own is not shown. The client learns the URL, and
// may open it in a view.
ViewImplementation* EmacsWebView::create_view_for_new_tab_or_window(IsPrivate is_private)
{
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

ErrorOr<NonnullOwnPtr<EmacsServer>> EmacsServer::create(Core::AnonymousBuffer theme, BrowserOptions const& options)
{
    auto kind = options.emacs_socket_path.has_value() ? EmacsTransport::Kind::Socket : EmacsTransport::Kind::Stdio;
    auto transport = TRY(EmacsTransport::create(kind, options.emacs_socket_path));
    auto server = adopt_own(*new EmacsServer(move(theme), move(transport)));

    // NB: Without a URL on the command line, the options hold the new tab page.
    if (!options.raw_urls.is_empty() && !options.urls.is_empty()) {
        auto width = clamp(options.window_width, 1, maximum_viewport_dimension);
        auto height = min(clamp(options.window_height, 1, maximum_viewport_dimension), static_cast<int>(maximum_viewport_area / width));
        auto device_pixel_ratio = clamp(options.device_pixel_ratio, minimum_device_pixel_ratio, maximum_device_pixel_ratio);
        auto& view = server->create_view({ width, height }, device_pixel_ratio);
        view.load_from_browser_ui(options.urls.first());
    }
    return server;
}

static bool signal_is_ignored(int signal_number)
{
    struct sigaction action {};
    return ::sigaction(signal_number, nullptr, &action) == 0 && action.sa_handler == SIG_IGN;
}

EmacsServer::EmacsServer(Core::AnonymousBuffer theme, NonnullOwnPtr<EmacsTransport> transport)
    : m_theme(move(theme))
    , m_transport(move(transport))
{
    s_the_server = this;

    m_transport->on_validate_hello = [](JsonObject const& hello) -> Optional<String> {
        if (hello.get_u32("protocol"sv) != EmacsProtocol::protocol_version)
            return "unsupported protocol version"_string;
        return {};
    };

    m_transport->on_client_connected = [this] {
        m_client = {};
        if (m_view)
            m_view->client_went_away();
    };

    m_transport->on_control = [this](JsonObject const& message) {
        handle_control(message);
    };

    m_transport->on_client_disconnected = [this] {
        dbgln("Emacs: client disconnected");
        m_client = {};
        if (m_view)
            m_view->client_went_away();
        // Nothing can connect to the standard streams again.
        if (m_transport->kind() == EmacsTransport::Kind::Stdio)
            shut_down();
    };

    for (auto signal_number : { SIGINT, SIGTERM, SIGHUP }) {
        // A server started under nohup outlives its terminal.
        if (signal_number == SIGHUP && signal_is_ignored(SIGHUP))
            continue;
        m_signal_handler_ids.append(Core::EventLoop::register_signal(signal_number, [this](int) {
            shut_down();
        }));
    }
}

EmacsServer::~EmacsServer()
{
    for (auto id : m_signal_handler_ids)
        Core::EventLoop::unregister_signal(id);
    m_view = nullptr;
    m_transport->shutdown();
    s_the_server = nullptr;
}

void EmacsServer::did_block_download(StringView name)
{
    dbgln("Emacs: blocked the download of '{}'", name);
    if (!s_the_server)
        return;
    if (s_the_server->m_view)
        s_the_server->m_view->did_block_download(name);
    else
        s_the_server->send_error({}, wire_formatted("download blocked: {}", name));
}

void EmacsServer::shut_down()
{
    // The socket and the shared memory go now, in case the rest of the process does not get to exit in order.
    m_transport->shutdown();
    if (m_view)
        m_view->client_went_away();
    Core::EventLoop::current().quit(0);
}

bool EmacsServer::send(JsonObject const& message)
{
    auto record = EmacsProtocol::encode_control_record(message);
    if (record.is_error()) {
        dbgln("Emacs: dropping a '{}' message that does not fit a record", message.get_string("type"sv).value_or({}));
        return false;
    }
    send_record(record.release_value());
    return true;
}

void EmacsServer::send_record(ByteBuffer&& record)
{
    if (has_ready_client())
        m_transport->send_record(move(record));
}

void EmacsServer::send_error(Optional<u32> view, StringView text, JsonValue request)
{
    JsonObject error;
    error.set("type"sv, "error"sv);
    if (view.has_value())
        error.set("view"sv, *view);
    error.set("message"sv, wire_text(text));
    if (!request.is_null())
        error.set("request"sv, move(request));
    send(error);
}

void EmacsServer::view_did_close(Badge<EmacsWebView>)
{
    // Deferred, as this is called from the view.
    Core::deferred_invoke([this, id = m_view->id()] {
        if (m_view && m_view->id() == id)
            m_view = nullptr;
    });
}

EmacsWebView& EmacsServer::create_view(Web::DevicePixelSize size, double device_pixel_ratio)
{
    VERIFY(!m_view);
    m_view = EmacsWebView::create(m_theme, size, device_pixel_ratio, *this, m_next_view_id++);
    return *m_view;
}

void EmacsServer::handle_control(JsonObject const& message)
{
    auto type = message.get_string("type"sv);
    if (!type.has_value())
        return;

    // NB: The transport makes a connection the client only for a hello, which is then the first message.
    if (!m_client.hello_done) {
        if (*type == "hello"sv)
            handle_hello(message);
        return;
    }

    if (*type == "ping"sv) {
        JsonObject pong;
        pong.set("type"sv, "pong"sv);
        pong.set("id"sv, echoed(message, "id"sv));
        send(pong);
        return;
    }
    if (*type == "hello"sv) {
        send_error({}, "hello was already sent"sv);
        return;
    }
    if (*type == "create_view"sv) {
        handle_create_view(message);
        return;
    }
    if (*type == "close_view"sv) {
        handle_close_view(message);
        return;
    }

    auto view_id = message.get_u32("view"sv);

    if (*type == "shm_failed"sv) {
        m_client.uses_shared_memory = false;
        if (m_view && view_id == m_view->id())
            m_view->client_cannot_use_shared_memory();
        return;
    }

    if (m_view && view_id == m_view->id()) {
        m_view->handle_message(*type, message);
        return;
    }

    // An answer or acknowledgement for a view that is gone needs no reply, and unknown messages are ignored.
    static constexpr Array commands = {
        "mouse"sv, "key"sv, "text"sv, "set_viewport"sv, "set_visibility"sv, "set_focus"sv, "load"sv, "load_html"sv,
        "reload"sv, "stop"sv, "history"sv, "zoom"sv, "find"sv, "find_step"sv, "edit"sv, "paste"sv,
        "request_selection"sv, "screenshot"sv, "set_console"sv, "request_source"sv, "hints"sv, "hints_narrow"sv,
        "hints_end"sv
    };
    if (type->starts_with_bytes("hints"sv) && commands.contains_slow(type->bytes_as_string_view())) {
        // NB: An error that answers a hints message names the session it did.
        JsonObject error;
        error.set("type"sv, "error"sv);
        if (view_id.has_value())
            error.set("view"sv, *view_id);
        error.set("message"sv, "unknown view"sv);
        if (auto id = message.get("id"sv); id.has_value())
            error.set("id"sv, *id);
        send(error);
    } else if (commands.contains_slow(type->bytes_as_string_view())) {
        send_error(view_id, "unknown view"sv);
    }
}

void EmacsServer::handle_hello(JsonObject const& message)
{
    // The client lists what it decodes in the order it prefers.
    m_client.accepts_zstd = false;
    if (auto encodings = message.get_array("encodings"sv); encodings.has_value()) {
        for (auto const& encoding : encodings->values()) {
            if (!encoding.is_string())
                continue;
            if (encoding.as_string() == "zstd"sv)
                m_client.accepts_zstd = true;
            if (encoding.as_string() == "zstd"sv || encoding.as_string() == "raw"sv)
                break;
        }
    }
    // NB: Only the client knows whether it runs on this machine.
    m_client.uses_shared_memory = message.get_bool("shm"sv).value_or(false);
    m_client.hello_done = true;

    dbgln("Emacs: client '{}' connected over {}", message.get_string("client"sv).value_or({}), m_transport->kind() == EmacsTransport::Kind::Stdio ? "stdio"sv : "a socket"sv);

    char hostname[256] {};
    (void)::gethostname(hostname, sizeof(hostname) - 1);
#if defined(AK_OS_MACOS)
    auto platform = "macos"sv;
#elif defined(AK_OS_LINUX)
    auto platform = "linux"sv;
#else
    auto platform = "unknown"sv;
#endif

    JsonObject host;
    host.set("hostname"sv, wire_text(StringView { hostname, strlen(hostname) }));
    host.set("platform"sv, platform);

    JsonArray views;
    if (m_view)
        views.must_append(m_view->description());

    JsonObject welcome;
    welcome.set("type"sv, "welcome"sv);
    welcome.set("protocol"sv, EmacsProtocol::protocol_version);
    welcome.set("server"sv, BROWSER_PRODUCT_NAME " " BROWSER_VERSION ""sv);
    welcome.set("encoding"sv, m_client.accepts_zstd ? "zstd"sv : "raw"sv);
    welcome.set("shm"sv, m_client.uses_shared_memory);
    welcome.set("log"sv, wire_text(EmacsTransport::log_path()));
    welcome.set("host"sv, move(host));
    JsonArray features;
    features.must_append("hints"sv);
    welcome.set("features"sv, move(features));
    welcome.set("views"sv, move(views));
    send(welcome);

    if (m_view)
        m_view->client_changed();
}

void EmacsServer::handle_create_view(JsonObject const& message)
{
    auto request = echoed(message, "request"sv);
    if (m_view) {
        send_error(m_view->id(), "only one view is supported"sv, request);
        return;
    }

    auto viewport = viewport_from(message, { { 800, 600 }, 1.0 });
    if (viewport.is_error()) {
        send_error({}, viewport.error().string_literal(), request);
        return;
    }

    Optional<URL::URL> url;
    if (auto text = message.get_string("url"sv); text.has_value() && !text->is_empty()) {
        url = sanitize_url(*text, Application::settings().search_engine_settings().engine);
        if (!url.has_value()) {
            send_error({}, wire_formatted("not a URL: {}", *text), request);
            return;
        }
    }

    auto& view = create_view(viewport.value().size, viewport.value().device_pixel_ratio);

    auto created = view.description();
    created.set("type"sv, "view_created"sv);
    if (!request.is_null())
        created.set("request"sv, request);
    send(created);

    // The load starts before the first state goes out, so that state says the view is loading.
    if (url.has_value())
        view.load_from_browser_ui(*url);
    view.client_changed();
}

void EmacsServer::handle_close_view(JsonObject const& message)
{
    auto view_id = message.get_u32("view"sv);
    if (!m_view || view_id != m_view->id()) {
        send_error(view_id, "unknown view"sv);
        return;
    }

    m_view = nullptr;

    JsonObject closed;
    closed.set("type"sv, "view_closed"sv);
    closed.set("view"sv, *view_id);
    closed.set("reason"sv, "client"sv);
    send(closed);
}

}
