/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web {

// A click the browser makes on an element it chose, which the UI process sends as the input events it names. In the
// process hosting the element, each of those events reaches the page only if a hit test at its position, in the task
// that dispatches it, finds the element or something in it. In each process hosting a document the events pass through
// on their way to a frame another process hosts, the hit test must find that frame's container. Once one does not, the
// click ends: nothing of it that is still to come reaches the page, and a press that did is cancelled.
struct GuardedClick {
    PageId page_id { 0 };
    u64 request_id { 0 };
    // What the events must hit: the element, or the container of the frame they go on to.
    Variant<UniqueNodeID, HTML::CrossProcessId> target { UniqueNodeID { 0 } };
    // The local root the events come to in this process, or none for the page's traversable.
    Optional<HTML::CrossProcessId> root;
    // The pointer move, the press and the release, in that order.
    Vector<u64> event_ids;
    MonotonicTime expiry;
    // Called once: with false when the click ended without its release, with true when the release reached the
    // element (for a click whose target is an element).
    Function<void(bool clicked)> on_finish;
};

WEB_API void guard_click(GuardedClick);
// Another process refused the click: what is still to come of it is dropped, and a press that reached the page is
// cancelled.
WEB_API void cancel_guarded_click(Page&, u64 request_id);
// Forgets the clicks of a page that is gone.
WEB_API void forget_guarded_clicks(PageId);

enum class GuardedClickDecision : u8 {
    NotGuarded,
    Dispatch,
    Drop,
    // The release is dropped, and the press that went before it is cancelled.
    DropAndCancelPress,
};

GuardedClickDecision decide_guarded_click(Page&, HTML::LocalNavigable& root, Optional<HTML::CrossProcessId> const& root_id, MouseEvent const&);

// What a mouse event at the position would hit, starting from the local root's document and going into the documents of
// same-process frames, as event dispatch does. Over a frame another process hosts, that is the frame's container.
WEB_API GC::Ptr<DOM::Node> node_hit_from_local_root(HTML::LocalNavigable& root, CSSPixelPoint visual_viewport_position);

}
