/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibGfx/Rect.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/UIEvents/MouseButton.h>
#include <LibWebView/EmacsFrameEncoder.h>
#include <LibWebView/EmacsProtocol.h>
#include <LibWebView/EmacsTransport.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Options.h>

namespace WebView {

class EmacsServer;

// A view whose page an Emacs client shows: frames go to the client as frame records, the client's control messages
// become input and commands, and what a browser window would ask its user goes to the client as requests it answers.
class WEBVIEW_API EmacsWebView final : public HeadlessWebView {
public:
    static NonnullOwnPtr<EmacsWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize, double device_pixel_ratio, EmacsServer&, u32 id, Optional<CanonicalTraversable&> = {});
    virtual ~EmacsWebView() override;

    u32 id() const { return m_id; }

    // The view as a welcome or view_created message describes it.
    JsonObject description() const;

    // A client that just finished its handshake knows nothing of the view: it gets the state, every request still
    // waiting for an answer, and a whole frame.
    void client_changed();
    void client_went_away();
    // The client cannot map the segment of this view, or of another one: frames take the stream from now on.
    void client_cannot_use_shared_memory();
    void client_stopped_using_shared_memory();

    // The view was waiting for its turn to send a frame on the connection, and may ask again.
    void did_get_turn_to_send() { schedule_frame_flush(); }
    // The client has read some of what it was sent.
    void connection_wrote() { send_held_messages(); }

    using HeadlessWebView::viewport_size;
    // How many device pixels the viewport has.
    i64 viewport_area() const { return static_cast<i64>(m_viewport_size.width().value()) * m_viewport_size.height().value(); }

    void did_block_download(StringView name);
    void did_get_opened_by_page() { m_opened_by_page = true; }

    void handle_message(StringView type, JsonObject const&);

private:
    enum class RequestKind : u8 {
        Dialog,
        Select,
        FilePicker,
        ColorPicker,
        DatePicker,
        Permission,
        ExternalURL,
    };

    // Something the client was asked and has yet to answer.
    struct OpenRequest {
        u64 id { 0 };
        RequestKind kind { RequestKind::Dialog };
        JsonObject message;
    };

    struct BeforeUnload {
        u64 request_id { 0 };
        Function<void(bool)> on_complete;
    };

    struct PendingScreenshot {
        JsonValue request;
        Optional<ByteString> path;
    };

    // An element a hint session labels, in the process hosting its document.
    struct HintTarget {
        NonnullRefPtr<WebContentPage> page;
        Web::UniqueNodeID node { 0 };
    };

    // The labels the client asked for. Each process hosting a document of the tab finds what can be clicked in its
    // documents and draws the labels there.
    struct HintSession {
        u64 serial { 0 };
        u64 id { 0 };
        String alphabet;
        u64 collection_request { 0 };
        // The processes asked, and those yet to answer.
        Vector<NonnullRefPtr<WebContentPage>> pages;
        Vector<WebContentPage*> pages_to_answer;
        Vector<EmacsProtocol::HintAnswerFromProcess> answers;
        Vector<HintTarget> targets;
        Vector<String> labels;
        bool listed { false };
        String prefix;
        RefPtr<Core::Timer> timeout;
        // Started once the process of the top-level document answered, for the others to follow.
        RefPtr<Core::Timer> grace;
    };

    // An activation a process was asked to prepare, which only that process answers, once.
    struct HintActivation {
        u64 session_id { 0 };
        NonnullRefPtr<WebContentPage> page;
        Web::UniqueNodeID node { 0 };
        MonotonicTime deadline;
    };

    // A click on its way: the processes guarding it, of which the one hosting the element says when it is done.
    struct HintClick {
        u64 session_id { 0 };
        Vector<NonnullRefPtr<WebContentPage>> pages;
        NonnullRefPtr<WebContentPage> element_page;
        MonotonicTime deadline;
    };

    enum class HintSessionEnd : u8 {
        // The client ended it, or is gone: the processes remove the labels.
        Silently,
        // Another document is active, the page closed its view, or a process did not answer in time or usably: the
        // client is told, and the processes remove what is left of the labels.
        Cancelled,
    };

    EmacsWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize, double device_pixel_ratio, EmacsServer&, u32 id);

    // ^ViewImplementation
    virtual void did_accept_presented_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect) override;
    virtual void did_change_state() override;
    virtual void update_zoom() override;
    virtual void did_request_context_menu(ContextMenuRequest) override;
    virtual void insert_clipboard_item(Web::Clipboard::SystemClipboardItem) override;
    virtual ViewImplementation* create_view_for_new_tab_or_window(IsPrivate) override;
    virtual void did_receive_screenshot(Badge<WebContentPage>, Gfx::ShareableBitmap const&) override;

    JsonObject message(StringView type) const;
    void send(JsonObject const&);
    // Sends a message that says only how something is now. The view has one batch of these on the connection at a
    // time, and adds none while the connection is behind; meanwhile the latest of each type is held back.
    void send_latest(JsonObject);
    void send_held_messages();
    void send_error(StringView text);
    // The page closed its view, or never showed anything in it.
    void close_for_page();

    void schedule_state();
    void send_state();
    void resend_input_method_state();
    void update_visibility();
    // Ends a frame that was being sent in bands; the view's next frame is a whole one.
    void stop_sending_frame();
    void release_hidden_frame_buffers();

    void schedule_frame_flush();
    void flush_frame();
    ErrorOr<void> send_frame(Gfx::Bitmap const&, Gfx::IntSize frame_size, double device_pixel_ratio);
    // Returns whether the frame went into a segment the client has yet to see a frame in.
    ErrorOr<bool> write_frame_to_shared_memory(Gfx::Bitmap const&, Gfx::IntSize frame_size, Gfx::IntRect, bool whole_frame);
    void handle_frame_ack(JsonObject const&);

    void set_viewport(JsonObject const&);
    void handle_mouse(JsonObject const&);
    void handle_key(JsonObject const&);
    void handle_load(JsonObject const&);
    void handle_find(JsonObject const&);
    void handle_edit(JsonObject const&);
    void handle_paste(JsonObject const&);
    void handle_request_selection(JsonObject const&);
    void handle_screenshot(JsonObject const&);
    void save_screenshot(PendingScreenshot const&, Gfx::Bitmap const&);

    void handle_hints(JsonObject const&);
    void handle_hints_narrow(JsonObject const&);
    void handle_hints_end(JsonObject const&);
    HintSession* hint_session_named_by(JsonObject const&);
    void send_hint_error(StringView text, JsonValue id);
    void did_collect_hints(WebContentPage&, u64 request_id, JsonValue const&);
    void list_hints();
    void send_hints_list(JsonArray items, bool truncated);
    void end_hint_session(HintSessionEnd, Optional<size_t> activate = {});
    void did_prepare_hint_activation(WebContentPage&, u64 request_id, JsonValue const&);
    void did_finish_hint_activation(WebContentPage&, u64 request_id, bool clicked);

    // Asks the client, unless the request does not fit a record.
    Optional<u64> open_request(RequestKind, JsonObject);
    // Asks the client about an alert, a confirmation or a prompt. One that cannot be asked is closed for the page.
    bool open_dialog(Web::PendingDialog, JsonObject);
    void close_pending_dialog(bool accepted, Optional<Utf16String> prompt_response);
    Optional<OpenRequest> take_request(RequestKind, JsonObject const& answer);
    void withdraw_request(OpenRequest const&);
    void withdraw_requests(RequestKind);
    void withdraw_page_requests();

    void handle_dialog_closed(JsonObject const&);
    void handle_select_closed(JsonObject const&);
    void handle_file_picker_closed(JsonObject const&);
    void handle_color_picker_closed(JsonObject const&);
    void handle_date_picker_closed(JsonObject const&);
    void handle_permission_closed(JsonObject const&);
    void handle_external_url_closed(JsonObject const&);

    void watch_blocked_popup(HeadlessWebView&);
    void report_blocked_popup(u64 popup_view_id, Optional<URL::URL> const&);

    EmacsServer& m_server;
    u32 m_id { 0 };

    // Whether a client has been told of the view, in a welcome or a view_created message. Until then nothing about the
    // view is sent.
    bool m_client_knows_view { false };
    bool m_opened_by_page { false };
    // Whether the client wants the view shown, as it last said with set_visibility.
    bool m_visible_to_client { true };
    // Whether the view renders, which it does while it is visible to a client.
    bool m_shown { false };

    bool m_state_scheduled { false };
    String m_last_sent_state;
    Vector<JsonObject> m_held_messages;
    u64 m_end_of_sent_latest_messages { 0 };
    String m_last_sent_input_method_state;
    Optional<Gfx::Color> m_last_sent_background_color;

    bool m_frame_flush_scheduled { false };
    bool m_frame_needs_full { true };
    bool m_frame_in_flight { false };
    u32 m_frame_seq { 0 };
    Optional<Gfx::IntRect> m_pending_damage;
    // What is left to send of a frame too large for one record.
    Optional<Gfx::IntRect> m_remaining_bands;
    Gfx::IntSize m_last_sent_frame_size;
    EmacsFrameEncoder m_frame_encoder;

    OwnPtr<EmacsSharedFrameBuffer> m_shared_frame_buffer;
    // The segment the client read frames from before the current one, which lasts until the client has a frame in
    // the current one.
    OwnPtr<EmacsSharedFrameBuffer> m_retired_shared_frame_buffer;
    Gfx::IntSize m_shared_frame_size;

    Web::UIEvents::MouseButton m_held_mouse_buttons { Web::UIEvents::MouseButton::None };
    int m_click_count { 1 };

    String m_find_query;
    bool m_console_enabled { false };
    // The screenshot of the whole page that the page has been asked for.
    Optional<PendingScreenshot> m_pending_screenshot;

    Vector<OpenRequest> m_open_requests;
    // The request that the alert, confirmation or prompt the page waits in was asked with.
    Optional<u64> m_dialog_request_id;
    Optional<BeforeUnload> m_before_unload;
    Optional<Web::HTML::CrossProcessId> m_active_document_id;
    HashMap<u64, PermissionPromptCallback> m_permission_callbacks;
    HashMap<u64, PermissionPromptRequest> m_permission_requests;
    HashMap<u64, Function<void(bool)>> m_external_url_callbacks;

    HashMap<u64, NonnullRefPtr<Core::Timer>> m_popup_timers;

    Optional<HintSession> m_hint_session;
    u64 m_next_hint_session_serial { 1 };
    HashMap<u64, HintActivation> m_hint_activations;
    HashMap<u64, HintClick> m_hint_clicks;
};

}
