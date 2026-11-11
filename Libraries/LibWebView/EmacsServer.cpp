/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/StringBuilder.h>
#include <LibCore/BrowserVersion.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/Application.h>
#include <LibWebView/EmacsProtocol.h>
#include <LibWebView/EmacsServer.h>
#include <LibWebView/EmacsWebView.h>
#include <LibWebView/URL.h>

#include <signal.h>
#include <string.h>
#include <unistd.h>

namespace WebView {

// Frames are not added to a connection that holds this many bytes yet to be written, and messages that say only how
// something is now are held back. With at most one frame record beyond it, the connection holds well below what makes
// the transport give up on a client.
static constexpr size_t frame_bytes_budget = 64 * MiB;

static EmacsServer* s_the_server;

static JsonValue echoed(JsonObject const& object, StringView key)
{
    if (auto value = object.get(key); value.has_value())
        return *value;
    return JsonValue {};
}

ErrorOr<NonnullOwnPtr<EmacsServer>> EmacsServer::create(Core::AnonymousBuffer theme, BrowserOptions const& options)
{
    auto kind = options.emacs_socket_path.has_value() ? EmacsTransport::Kind::Socket : EmacsTransport::Kind::Stdio;
    auto transport = TRY(EmacsTransport::create(kind, options.emacs_socket_path));
    auto maximum_views = options.emacs_maximum_views.value_or(default_maximum_views);
    VERIFY(maximum_views >= 1 && maximum_views <= maximum_maximum_views);
    auto server = adopt_own(*new EmacsServer(move(theme), move(transport), maximum_views));

    // NB: Without a URL on the command line, the options hold the new tab page.
    if (!options.raw_urls.is_empty() && !options.urls.is_empty()) {
        auto width = clamp(options.window_width, 1, EmacsProtocol::maximum_viewport_dimension);
        auto height = min(clamp(options.window_height, 1, EmacsProtocol::maximum_viewport_dimension), static_cast<int>(EmacsProtocol::maximum_viewport_area / width));
        auto device_pixel_ratio = clamp(options.device_pixel_ratio, EmacsProtocol::minimum_device_pixel_ratio, EmacsProtocol::maximum_device_pixel_ratio);
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

EmacsServer::EmacsServer(Core::AnonymousBuffer theme, NonnullOwnPtr<EmacsTransport> transport, size_t maximum_views)
    : m_theme(move(theme))
    , m_transport(move(transport))
    , m_maximum_views(maximum_views)
    , m_frame_turns(frame_bytes_budget)
{
    s_the_server = this;

    m_frame_turns.on_turn = [this](u32 view_id) {
        if (auto* view = view_with_id(view_id))
            view->did_get_turn_to_send();
        else
            m_frame_turns.stop_sending(view_id);
    };

    m_transport->on_validate_hello = [](JsonObject const& hello) -> Optional<String> {
        if (hello.get_u32("protocol"sv) != EmacsProtocol::protocol_version)
            return "unsupported protocol version"_string;
        return {};
    };

    m_transport->on_client_connected = [this] {
        client_went_away();
    };

    m_transport->on_control = [this](JsonObject const& message) {
        handle_control(message);
    };

    m_transport->on_client_disconnected = [this] {
        dbgln("Emacs: client disconnected");
        client_went_away();
        // Nothing can connect to the standard streams again.
        if (m_transport->kind() == EmacsTransport::Kind::Stdio)
            shut_down();
    };

    m_transport->on_wrote = [this] {
        connection_wrote();
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
    m_views.clear();
    m_closing_views.clear();
    m_transport->shutdown();
    s_the_server = nullptr;
}

// NB: A download that is not a view's own, such as one of a window that is not shown, is the only view's if there is
//     one view.
void EmacsServer::did_block_download(StringView name, ViewImplementation const* requesting_view)
{
    dbgln("Emacs: blocked the download of '{}'", name);
    if (!s_the_server)
        return;
    for (auto& view : s_the_server->m_views) {
        if (view.ptr() == requesting_view) {
            view->did_block_download(name);
            return;
        }
    }
    if (s_the_server->m_views.size() == 1) {
        s_the_server->m_views.first()->did_block_download(name);
        return;
    }
    StringBuilder text;
    text.appendff("download blocked: {}", name);
    s_the_server->send_error({}, text.string_view());
}

void EmacsServer::client_went_away()
{
    m_client = {};
    m_frame_turns.reset();
    for (auto& view : m_views)
        view->client_went_away();
}

void EmacsServer::shut_down()
{
    // The socket and the shared memory go now, in case the rest of the process does not get to exit in order.
    m_transport->shutdown();
    client_went_away();
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
    error.set("message"sv, EmacsProtocol::wire_text(text));
    if (!request.is_null())
        error.set("request"sv, move(request));
    send(error);
}

bool EmacsServer::begin_frame_turn(EmacsWebView& view)
{
    return m_frame_turns.begin_turn(view.id(), m_transport->queued_bytes());
}

void EmacsServer::end_frame_turn(EmacsWebView& view, bool between_bands)
{
    m_frame_turns.end_turn(view.id(), between_bands);
}

void EmacsServer::stop_waiting_for_frame_turn(EmacsWebView& view)
{
    m_frame_turns.stop_waiting(view.id());
}

void EmacsServer::stop_sending_frames(EmacsWebView& view)
{
    m_frame_turns.stop_sending(view.id());
}

bool EmacsServer::connection_is_behind() const
{
    return m_transport->queued_bytes() >= frame_bytes_budget;
}

void EmacsServer::connection_wrote()
{
    for (auto& view : m_views)
        view->connection_wrote();
    m_frame_turns.connection_wrote(m_transport->queued_bytes());
}

i64 EmacsServer::viewport_area_left_for(EmacsWebView const* view) const
{
    auto area = EmacsProtocol::maximum_total_viewport_area;
    for (auto const& other : m_views) {
        if (other.ptr() != view)
            area -= other->viewport_area();
    }
    return area;
}

EmacsWebView* EmacsServer::view_with_id(Optional<u32> view_id)
{
    if (!view_id.has_value())
        return nullptr;
    for (auto& view : m_views) {
        if (view->id() == *view_id)
            return view.ptr();
    }
    return nullptr;
}

void EmacsServer::view_did_close(Badge<EmacsWebView>, u32 view_id)
{
    // NB: The view is gone for the client at once, as it was told so. It is destroyed later, as this is called from it.
    auto index = m_views.find_first_index_if([&](auto const& view) { return view->id() == view_id; });
    if (!index.has_value())
        return;
    m_closing_views.append(m_views.take(*index));
    m_frame_turns.stop_sending(view_id);
    Core::deferred_invoke([this, view_id] {
        m_closing_views.remove_first_matching([&](auto const& view) { return view->id() == view_id; });
    });
}

EmacsWebView& EmacsServer::create_view(Web::DevicePixelSize size, double device_pixel_ratio, Optional<CanonicalTraversable&> traversable)
{
    VERIFY(m_views.size() < m_maximum_views);
    m_views.append(EmacsWebView::create(m_theme, size, device_pixel_ratio, *this, m_next_view_id++, traversable));
    return *m_views.last();
}

EmacsWebView* EmacsServer::open_view(Badge<EmacsWebView>, EmacsWebView& opener, Optional<CanonicalTraversable&> traversable)
{
    if (!has_ready_client() || !m_client.takes_popups || m_views.size() >= m_maximum_views)
        return nullptr;
    if (viewport_area_left_for(nullptr) < opener.viewport_area())
        return nullptr;

    auto& view = create_view(opener.viewport_size(), opener.device_pixel_ratio(), traversable);
    view.did_get_opened_by_page();
    auto created = view.description();
    created.set("type"sv, "view_created"sv);
    created.set("opener"sv, opener.id());
    send(created);
    if (traversable.has_value()) {
        view.client_changed();
        return &view;
    }
    // NB: The caller loads a URL in a view for a tab once this returns, which the first state then tells.
    Core::deferred_invoke([this, weak_view = view.make_weak_ptr<EmacsWebView>()] {
        if (weak_view && has_ready_client())
            weak_view->client_changed();
    });
    return &view;
}

void EmacsServer::remove_view(u32 view_id)
{
    m_views.remove_first_matching([&](auto const& view) { return view->id() == view_id; });
    m_frame_turns.stop_sending(view_id);
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

    // A client that cannot map the segment of one view cannot map those of the others either.
    if (*type == "shm_failed"sv) {
        m_client.uses_shared_memory = false;
        for (auto& view : m_views) {
            if (view->id() == view_id)
                view->client_cannot_use_shared_memory();
            else
                view->client_stopped_using_shared_memory();
        }
        return;
    }

    if (auto* view = view_with_id(view_id)) {
        view->handle_message(*type, message);
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
    m_client.takes_popups = message.get_bool("popups"sv).value_or(false);
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
    host.set("hostname"sv, EmacsProtocol::wire_text(StringView { hostname, strlen(hostname) }));
    host.set("platform"sv, platform);

    JsonArray views;
    for (auto const& view : m_views)
        views.must_append(view->description());

    JsonObject welcome;
    welcome.set("type"sv, "welcome"sv);
    welcome.set("protocol"sv, EmacsProtocol::protocol_version);
    welcome.set("server"sv, BROWSER_PRODUCT_NAME " " BROWSER_VERSION ""sv);
    welcome.set("encoding"sv, m_client.accepts_zstd ? "zstd"sv : "raw"sv);
    welcome.set("shm"sv, m_client.uses_shared_memory);
    welcome.set("log"sv, EmacsProtocol::wire_text(EmacsTransport::log_path()));
    welcome.set("host"sv, move(host));
    JsonArray features;
    features.must_append("hints"sv);
    welcome.set("features"sv, move(features));
    welcome.set("max_views"sv, static_cast<u64>(m_maximum_views));
    welcome.set("views"sv, move(views));
    send(welcome);

    for (auto& view : m_views)
        view->client_changed();
}

void EmacsServer::handle_create_view(JsonObject const& message)
{
    auto request = echoed(message, "request"sv);
    if (m_views.size() >= m_maximum_views) {
        StringBuilder text;
        text.appendff("the server has {} views, which is as many as it can have", m_maximum_views);
        send_error({}, text.string_view(), request);
        return;
    }

    auto viewport = EmacsProtocol::viewport_from(message, { { 800, 600 }, 1.0 }, viewport_area_left_for(nullptr));
    if (viewport.is_error()) {
        send_error({}, viewport.error().string_literal(), request);
        return;
    }

    Optional<URL::URL> url;
    if (auto text = message.get_string("url"sv); text.has_value() && !text->is_empty()) {
        url = sanitize_url(*text, Application::settings().search_engine_settings().engine);
        if (!url.has_value()) {
            StringBuilder error;
            error.appendff("not a URL: {}", *text);
            send_error({}, error.string_view(), request);
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
    if (!view_with_id(view_id)) {
        // NB: A view its page closed was announced closed already.
        if (!view_id.has_value() || *view_id >= m_next_view_id)
            send_error(view_id, "unknown view"sv);
        return;
    }

    remove_view(*view_id);

    JsonObject closed;
    closed.set("type"sv, "view_closed"sv);
    closed.set("view"sv, *view_id);
    closed.set("reason"sv, "client"sv);
    send(closed);
}

}
