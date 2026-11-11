/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibWebView/EmacsFrameTurns.h>

namespace WebView {

bool EmacsFrameTurns::wait(ViewId view)
{
    if (!m_waiting_views.contains_slow(view))
        m_waiting_views.append(view);
    return false;
}

bool EmacsFrameTurns::begin_turn(ViewId view, size_t unread_bytes)
{
    if (m_view_with_turn.has_value()) {
        dbgln("Emacs: the turn of view {} was not ended", *m_view_with_turn);
        end_turn(*m_view_with_turn, false);
    }

    // The bands of a frame follow each other on the connection.
    if (m_view_sending_bands.has_value() && *m_view_sending_bands != view)
        return wait(view);
    if (unread_bytes >= m_budget)
        return wait(view);
    if (!m_view_sending_bands.has_value() && !m_waiting_views.is_empty() && m_waiting_views.first() != view)
        return wait(view);

    m_waiting_views.remove_first_matching([&](auto id) { return id == view; });
    m_view_with_turn = view;
    return true;
}

void EmacsFrameTurns::end_turn(ViewId view, bool between_bands)
{
    if (m_view_with_turn == view)
        m_view_with_turn.clear();
    if (between_bands) {
        m_view_sending_bands = view;
        return;
    }
    if (m_view_sending_bands == view)
        m_view_sending_bands.clear();
    hand_on();
}

void EmacsFrameTurns::stop_waiting(ViewId view)
{
    auto index = m_waiting_views.find_first_index(view);
    if (!index.has_value())
        return;
    m_waiting_views.remove(*index);
    if (*index == 0)
        hand_on();
}

void EmacsFrameTurns::stop_sending(ViewId view)
{
    if (m_view_with_turn == view)
        m_view_with_turn.clear();
    m_waiting_views.remove_first_matching([&](auto id) { return id == view; });
    if (m_view_sending_bands == view)
        m_view_sending_bands.clear();
    hand_on();
}

void EmacsFrameTurns::connection_wrote(size_t unread_bytes)
{
    if (unread_bytes < m_budget)
        hand_on();
}

void EmacsFrameTurns::reset()
{
    m_view_sending_bands.clear();
    m_view_with_turn.clear();
    m_waiting_views.clear();
}

void EmacsFrameTurns::hand_on()
{
    if (!on_turn)
        return;
    // While a frame is sent in bands, only its view can go on.
    if (m_view_sending_bands.has_value()) {
        if (m_waiting_views.contains_slow(*m_view_sending_bands))
            on_turn(*m_view_sending_bands);
        return;
    }
    // NB: A view told to ask again can stop waiting meanwhile, which changes the list.
    auto waiting_views = m_waiting_views;
    for (auto view : waiting_views)
        on_turn(view);
}

}
