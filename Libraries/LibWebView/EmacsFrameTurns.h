/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>

namespace WebView {

// The views of an Emacs server take turns sending frame records on the one connection. A view gets a turn only while
// the connection holds fewer unread bytes than a budget, no other view is between the bands of a frame, and no view
// that has waited longer still waits. A view that does not get a turn waits, and is told when to ask again.
//
// A turn that was given is ended with end_turn(), whatever the view did with it. The unread bytes then exceed the
// budget by at most one frame record, as each turn adds at most one and none is given at or above the budget.
class WEBVIEW_API EmacsFrameTurns {
public:
    using ViewId = u32;

    explicit EmacsFrameTurns(size_t budget)
        : m_budget(budget)
    {
    }

    size_t budget() const { return m_budget; }

    // Whether the view may send a frame record now. A view that may not is added to the end of the waiting views.
    bool begin_turn(ViewId, size_t unread_bytes);
    // Ends the view's turn. A view between the bands of a frame keeps the others waiting until its frame ends.
    void end_turn(ViewId, bool between_bands);

    // The view has nothing to send for now, so it does not hold up the views waiting behind it.
    void stop_waiting(ViewId);
    // The view sends no more of a frame: it was hidden or closed, or its page crashed. The rest of a frame it was
    // sending in bands is not sent.
    void stop_sending(ViewId);

    // Called as the connection's unread bytes go down.
    void connection_wrote(size_t unread_bytes);
    // The client went away; nothing it was sent is waited for any more.
    void reset();

    Optional<ViewId> view_sending_bands() const { return m_view_sending_bands; }
    Vector<ViewId> const& waiting_views() const { return m_waiting_views; }

    // Tells a waiting view to ask for its turn again.
    Function<void(ViewId)> on_turn;

private:
    bool wait(ViewId);
    void hand_on();

    size_t m_budget { 0 };
    Optional<ViewId> m_view_sending_bands;
    Optional<ViewId> m_view_with_turn;
    // The longest waiting first.
    Vector<ViewId> m_waiting_views;
};

}
