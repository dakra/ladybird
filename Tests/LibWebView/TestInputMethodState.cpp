/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <stdlib.h>

// WebContent pushes fresh IME state (caret, cursoranchor positions, surrounding text) to the UI on every editing step,
// and platform IMEs compose against the UI-side copy of that state. The UI view must be notified of every push (not
// just the first) so it can invalidate whatever the platform IME has cached: The Qt port answers inputMethodQuery()
// from input_method_state(). And without an updateMicroFocus() nudge per push, the macOS IME composes against the stale
// 1st-keystroke state and drops everything after the first committed character. See #9712: on_input_method_state_change
// must fire for each state push, after the new state is already readable through input_method_state().

TEST_CASE(input_method_state)
{
    Test::HeadlessTestEnvironment environment { "TestInputMethodState"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };

    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    view->load_html("<!DOCTYPE html><input>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 2; });

    // Runs synchronously inside WebContent's handler, so it is ordered before the commits below.
    view->run_javascript("document.querySelector('input').focus()"_string);

    size_t state_changes = 0;
    WebView::ViewImplementation::InputMethodState observed_state;
    view->on_input_method_state_change = [&]() {
        ++state_changes;
        // The new state must already be stored when the notification fires: The notified view re-queries the state from
        // inside the callback. Qt's updateMicroFocus() makes the platform IME call inputMethodQuery(), which answers
        // from input_method_state().
        observed_state = view->input_method_state();
    };

    // Focus moves and new documents push states of their own, so each step waits for a push after it that has the
    // expected state.
    auto any_state = [](auto const&) { return true; };
    auto wait_for_push = [&](auto expected) {
        auto before = state_changes;
        auto pushed = [&] { return state_changes > before && expected(observed_state); };
        auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [] { });
        deadline->start();
        Core::EventLoop::current().spin_until([&]() { return pushed() || !deadline->is_active(); });
        VERIFY(pushed());
    };

    // Each committed character round-trips through WebContent, which pushes a fresh input-method state.
    view->commit_text_from_input_method("あ"_utf16);
    wait_for_push([](auto const& s) { return s.cursor_position == 1; });
    VERIFY(observed_state.is_enabled);
    VERIFY(observed_state.cursor_position == 1);
    VERIFY(observed_state.text_before_cursor == "あ"_utf16);
    VERIFY(observed_state.text_after_cursor.is_empty());

    // Composing past the first character depends on the UI being notified for the SECOND push as well (#9712).
    view->commit_text_from_input_method("い"_utf16);
    wait_for_push([](auto const& s) { return s.cursor_position == 2; });
    VERIFY(observed_state.cursor_position == 2);
    VERIFY(observed_state.text_before_cursor == "あい"_utf16);

    // The disabled transition must be pushed and notified too, so the platform IME is turned off for the view.
    view->run_javascript("document.querySelector('input').blur()"_string);
    view->commit_text_from_input_method("x"_utf16);
    wait_for_push([](auto const& s) { return !s.is_enabled; });
    VERIFY(!observed_state.is_enabled);
    VERIFY(observed_state.text_before_cursor.is_empty());

    // Editing hosts must report IME state too: The Qt UI refuses to start composition at all when ImEnabled is false —
    // which drops input from dead keys and IMEs into contenteditable editors entirely (#11182). The state values are
    // derived from the DOM selection; the surrounding text is the content of the Text node holding the selection focus.
    view->load_html("<!DOCTYPE html><div contenteditable>abc</div>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 3; });

    view->run_javascript("document.querySelector('div').focus(); getSelection().collapse(document.querySelector('div').firstChild, 2)"_string);

    wait_for_push([](auto const& s) { return s.text_before_cursor == "ab"_utf16; });

    // Unmarking with no composition in progress leaves the page alone, but still pushes fresh IME state.
    view->unmark_text_from_input_method();
    wait_for_push(any_state);
    VERIFY(observed_state.is_enabled);
    VERIFY(observed_state.cursor_position == 2);
    VERIFY(observed_state.anchor_position == 2);
    VERIFY(observed_state.text_before_cursor == "ab"_utf16);
    VERIFY(observed_state.text_after_cursor == "c"_utf16);

    // Committing composes into the editing host; the pushed state reflects the mutated Text node.
    view->commit_text_from_input_method("ü"_utf16);
    wait_for_push([](auto const& s) { return s.cursor_position == 3; });
    VERIFY(observed_state.is_enabled);
    VERIFY(observed_state.cursor_position == 3);
    VERIFY(observed_state.text_before_cursor == "abü"_utf16);
    VERIFY(observed_state.text_after_cursor == "c"_utf16);

    // A range selection within one Text node reports the focus as the cursor and the anchor separately.
    view->run_javascript("getSelection().setBaseAndExtent(document.querySelector('div').firstChild, 1, document.querySelector('div').firstChild, 3)"_string);
    view->unmark_text_from_input_method();
    wait_for_push([](auto const& s) { return s.anchor_position == 1; });
    VERIFY(observed_state.cursor_position == 3);
    VERIFY(observed_state.anchor_position == 1);
    VERIFY(observed_state.text_before_cursor == "abü"_utf16);
    VERIFY(observed_state.text_after_cursor == "c"_utf16);

    // A selection whose anchor lies in a different node is reported as collapsed at the cursor.
    view->load_html("<!DOCTYPE html><div contenteditable><b>xy</b>zw</div>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 4; });
    view->run_javascript("document.querySelector('div').focus(); getSelection().setBaseAndExtent(document.querySelector('b').firstChild, 1, document.querySelector('div').lastChild, 1)"_string);
    wait_for_push([](auto const& s) { return s.text_before_cursor == "z"_utf16; });
    view->unmark_text_from_input_method();
    wait_for_push(any_state);
    VERIFY(observed_state.is_enabled);
    VERIFY(observed_state.cursor_position == 1);
    VERIFY(observed_state.anchor_position == 1);
    VERIFY(observed_state.text_before_cursor == "z"_utf16);
    VERIFY(observed_state.text_after_cursor == "w"_utf16);

    // An empty editing host is still IME-enabled, with no surrounding text to draw context from.
    view->load_html("<!DOCTYPE html><div contenteditable></div>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 5; });
    view->run_javascript("document.querySelector('div').focus()"_string);
    wait_for_push([](auto const& s) { return s.is_enabled && s.cursor_position == 0; });
    view->unmark_text_from_input_method();
    wait_for_push(any_state);
    VERIFY(observed_state.is_enabled);
    VERIFY(observed_state.cursor_position == 0);
    VERIFY(observed_state.anchor_position == 0);
    VERIFY(observed_state.text_before_cursor.is_empty());
    VERIFY(observed_state.text_after_cursor.is_empty());

    // Composing into it creates the Text node; the pushed state reflects it.
    view->commit_text_from_input_method("あ"_utf16);
    wait_for_push([](auto const& s) { return s.cursor_position == 1; });
    VERIFY(observed_state.is_enabled);
    VERIFY(observed_state.cursor_position == 1);
    VERIFY(observed_state.text_before_cursor == "あ"_utf16);

    // designMode makes the document an editing host.
    view->load_html("<!DOCTYPE html><p>hi</p>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 6; });
    view->run_javascript("document.designMode = 'on'; getSelection().collapse(document.querySelector('p').firstChild, 2)"_string);
    wait_for_push([](auto const& s) { return s.text_before_cursor == "hi"_utf16; });
    view->unmark_text_from_input_method();
    wait_for_push(any_state);
    VERIFY(observed_state.is_enabled);
    VERIFY(observed_state.cursor_position == 2);
    VERIFY(observed_state.text_before_cursor == "hi"_utf16);
    VERIFY(observed_state.text_after_cursor.is_empty());

    // A focused element that's not editable must not enable the IME.
    view->load_html("<!DOCTYPE html><input><div tabindex=\"0\">plain</div>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 7; });
    view->run_javascript("document.querySelector('input').focus()"_string);
    wait_for_push([](auto const& s) { return s.is_enabled; });
    view->run_javascript("document.querySelector('div').focus()"_string);
    view->unmark_text_from_input_method();
    wait_for_push([](auto const& s) { return !s.is_enabled; });
    VERIFY(!observed_state.is_enabled);
    VERIFY(observed_state.text_before_cursor.is_empty());

    // A focus that no input caused is pushed on its own, without transient activation: autofocus, a script's
    // focus() and blur(), and a new document taking the place of one with a focused field.
    view->load_html("<!DOCTYPE html><input autofocus>"sv);
    wait_for_push([](auto const& s) { return s.is_enabled; });
    VERIFY(!observed_state.has_transient_activation);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 8; });
    view->run_javascript("document.querySelector('input').blur()"_string);
    wait_for_push([](auto const& s) { return !s.is_enabled; });
    view->run_javascript("document.querySelector('input').focus()"_string);
    wait_for_push([](auto const& s) { return s.is_enabled; });
    VERIFY(!observed_state.has_transient_activation);
    view->load_html("<!DOCTYPE html><p>plain</p>"sv);
    wait_for_push([](auto const& s) { return !s.is_enabled; });
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 9; });

    // Email and number inputs take text without the selection APIs.
    view->load_html("<!DOCTYPE html><p tabindex=\"0\">plain</p><input type=\"email\"><input type=\"number\">"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 10; });
    view->run_javascript("document.querySelector('[type=email]').focus()"_string);
    wait_for_push([](auto const& s) { return s.is_enabled; });
    view->run_javascript("document.querySelector('p').focus()"_string);
    wait_for_push([](auto const& s) { return !s.is_enabled; });
    view->run_javascript("document.querySelector('[type=number]').focus()"_string);
    wait_for_push([](auto const& s) { return s.is_enabled; });

    // A focused field that stops or starts taking text is pushed too.
    view->load_html("<!DOCTYPE html><input><textarea></textarea><div tabindex=\"0\" contenteditable>x</div>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 11; });
    view->run_javascript("document.querySelector('textarea').focus()"_string);
    wait_for_push([](auto const& s) { return s.is_enabled; });
    view->run_javascript("document.querySelector('textarea').readOnly = true"_string);
    wait_for_push([](auto const& s) { return !s.is_enabled; });
    view->run_javascript("document.querySelector('input').focus()"_string);
    wait_for_push([](auto const& s) { return s.is_enabled; });
    view->run_javascript("document.querySelector('input').readOnly = true"_string);
    wait_for_push([](auto const& s) { return !s.is_enabled; });
    view->run_javascript("document.querySelector('input').readOnly = false"_string);
    wait_for_push([](auto const& s) { return s.is_enabled; });
    view->run_javascript("document.querySelector('input').type = 'button'"_string);
    wait_for_push([](auto const& s) { return !s.is_enabled; });
    view->run_javascript("document.querySelector('div').focus()"_string);
    wait_for_push([](auto const& s) { return s.is_enabled; });
    view->run_javascript("document.querySelector('div').removeAttribute('contenteditable')"_string);
    wait_for_push([](auto const& s) { return !s.is_enabled; });

    // A click that focuses a field gives the document transient activation.
    view->load_html("<!DOCTYPE html><input style=\"position: absolute; left: 0; top: 0; width: 100px; height: 30px\">"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 12; });
    for (auto type : { Web::MouseEvent::Type::MouseDown, Web::MouseEvent::Type::MouseUp }) {
        view->enqueue_input_event(Web::MouseEvent {
            .type = type,
            .position = { 10, 10 },
            .screen_position = { 10, 10 },
            .button = Web::UIEvents::MouseButton::Primary,
            .buttons = type == Web::MouseEvent::Type::MouseDown ? Web::UIEvents::MouseButton::Primary : Web::UIEvents::MouseButton::None,
            .click_count = 1,
            .browser_data = nullptr,
        });
    }
    wait_for_push([](auto const& s) { return s.is_enabled && s.has_transient_activation; });

    outln("PASS: every input-method state change notified the view");
}
