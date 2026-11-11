/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteBuffer.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebView/EmacsFrameTurns.h>
#include <LibWebView/EmacsTransport.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Options.h>

namespace WebView {

class EmacsWebView;

// The Emacs headless mode: one client at a time on a transport, and the views it shows. The server keeps running, and
// the views keep their pages, while no client is connected.
class WEBVIEW_API EmacsServer {
public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr size_t default_maximum_views = 16;
    static constexpr size_t maximum_maximum_views = 64;

    static ErrorOr<NonnullOwnPtr<EmacsServer>> create(Core::AnonymousBuffer theme, BrowserOptions const&);
    ~EmacsServer();

    // Whether a client has finished its handshake, so that it can be sent messages and frames.
    bool has_ready_client() const { return m_transport->has_client() && m_client.hello_done; }
    bool client_accepts_zstd() const { return m_client.accepts_zstd; }
    bool client_uses_shared_memory() const { return m_client.uses_shared_memory; }

    // A message that does not fit a record is dropped, which this reports.
    bool send(JsonObject const&);
    void send_record(ByteBuffer&&);
    void send_error(Optional<u32> view, StringView text, JsonValue request = {});

    // Pages save nothing in this mode, as the client is not asked where to. The client is told what was not saved.
    static void did_block_download(StringView name);

    // The views take turns sending frames on the connection, as EmacsFrameTurns describes. A view that may not send
    // now is told when to ask again. A turn that was given is ended, whatever the view did with it.
    bool begin_frame_turn(EmacsWebView&);
    void end_frame_turn(EmacsWebView&, bool between_bands);
    void stop_waiting_for_frame_turn(EmacsWebView&);
    void stop_sending_frames(EmacsWebView&);

    // Whether the connection holds at least the budget of unread bytes. The views are told whenever the client reads.
    bool connection_is_behind() const;
    // Whether the client has read the stream up to a position that end_of_stream() gave.
    bool client_has_read(u64 position) const { return m_transport->written_bytes() >= position; }
    u64 end_of_stream() const { return m_transport->end_of_stream(); }

    // The viewport area the view can have, with what the other views have.
    i64 viewport_area_left_for(EmacsWebView const*) const;

    // Request ids and screenshot names are the server's, so that those of different views differ.
    u64 next_request_id() { return m_next_request_id++; }
    u64 next_screenshot_number() { return ++m_screenshots_taken; }

    void view_did_close(Badge<EmacsWebView>, u32 view_id);

private:
    struct ClientState {
        bool hello_done { false };
        bool accepts_zstd { false };
        bool uses_shared_memory { false };
    };

    EmacsServer(Core::AnonymousBuffer theme, NonnullOwnPtr<EmacsTransport>, size_t maximum_views);

    void handle_control(JsonObject const&);
    void handle_hello(JsonObject const&);
    void handle_create_view(JsonObject const&);
    void handle_close_view(JsonObject const&);

    EmacsWebView* view_with_id(Optional<u32>);
    EmacsWebView& create_view(Web::DevicePixelSize, double device_pixel_ratio);
    void remove_view(u32 view_id);
    void connection_wrote();
    void client_went_away();
    void shut_down();

    Core::AnonymousBuffer m_theme;
    NonnullOwnPtr<EmacsTransport> m_transport;
    size_t m_maximum_views { default_maximum_views };
    Vector<NonnullOwnPtr<EmacsWebView>> m_views;
    // The views their pages closed, which go once the callbacks they are closed from have returned.
    Vector<NonnullOwnPtr<EmacsWebView>> m_closing_views;
    u32 m_next_view_id { 1 };
    EmacsFrameTurns m_frame_turns;
    u64 m_next_request_id { 1 };
    u64 m_screenshots_taken { 0 };
    ClientState m_client;
    Vector<int> m_signal_handler_ids;
};

}
