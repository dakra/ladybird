/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/PageId.h>

namespace WebContent {

// Hints: labels on what the user could click in the documents a page hosts, which the UI process asked for. The labels
// are elements in a user-agent shadow root, and a click on a labelled element is a guarded click (Web::GuardedClick).
namespace PageHints {

// What can be clicked in the documents of each local root of the page, with its boxes in the device pixels of the local
// root's viewport, and whether the page shows each frame another process hosts.
JsonValue collect(Web::Page&);
// Labels the elements the last collection found: an array of [node id, label] pairs.
void draw(Web::Page&, JsonValue const& labels);
// Shows only the labels that start with the prefix, and sets their typed part apart.
void narrow(Web::Page&, StringView prefix);
void clear(Web::Page&);

// Prepares the activation of an element the last collection found. With its label where it was drawn and a point of it
// hit, a text field gets the focus and a caret, and for anything else the answer names the point to click, in the device
// pixels of the local root it names.
JsonValue prepare_activation(Web::Page&, Web::UniqueNodeID);
// Guards the input events of a click (Web::GuardedClick): they must hit the element, or the container of the frame they
// go on to.
void guard_click(Web::Page&, u64 request_id, Optional<Web::UniqueNodeID> element, Optional<Web::HTML::CrossProcessId> frame, Optional<Web::HTML::CrossProcessId> root, Vector<u64> event_ids, Function<void(bool clicked)> on_finish);
void cancel_click(Web::Page&, u64 request_id);
// Forgets a page that is gone.
void forget(Web::PageId);

}

}
