/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/GuardedClick.h>
#include <LibWeb/Page/Page.h>

namespace Web {

// A click whose events are still to come.
struct ActiveGuardedClick {
    GuardedClick click;
    // The local root the press went to, once it reached the page.
    Optional<Optional<HTML::CrossProcessId>> pressed_root;
    // Once the click ended, what is still to come of it is dropped until it expires.
    bool ended { false };
};

// NB: A process has a click or two waiting at a time, so a list is enough.
static Vector<ActiveGuardedClick>& guarded_clicks()
{
    static NeverDestroyed<Vector<ActiveGuardedClick>> clicks;
    return *clicks;
}

static void prune_expired_clicks()
{
    auto now = MonotonicTime::now();
    for (size_t index = 0; index < guarded_clicks().size();) {
        if (guarded_clicks()[index].click.expiry >= now) {
            ++index;
            continue;
        }
        auto expired = guarded_clicks().take(index);
        if (expired.click.on_finish)
            expired.click.on_finish(false);
    }
}

void guard_click(GuardedClick click)
{
    prune_expired_clicks();
    guarded_clicks().append({ move(click), {} });
}

static GC::Ptr<HTML::LocalNavigable> root_with_id(Page& page, Optional<HTML::CrossProcessId> const& root_id)
{
    if (!root_id.has_value())
        return page.has_local_traversable() ? page.local_traversable().ptr() : nullptr;
    return as_if<HTML::LocalNavigable>(page.navigable_with_id(*root_id).ptr());
}

void cancel_guarded_click(Page& page, u64 request_id)
{
    auto index = guarded_clicks().find_first_index_if([&](auto const& active) {
        return active.click.page_id == page.client().id() && active.click.request_id == request_id;
    });
    if (!index.has_value())
        return;
    auto& active = guarded_clicks()[*index];
    if (active.pressed_root.has_value() && !active.ended) {
        if (auto root = root_with_id(page, *active.pressed_root))
            (void)page.handle_mousecancel(*root);
    }
    active.ended = true;
    active.click.on_finish = nullptr;
}

void forget_guarded_clicks(PageId page_id)
{
    guarded_clicks().remove_all_matching([&](auto const& active) { return active.click.page_id == page_id; });
}

GC::Ptr<DOM::Node> node_hit_from_local_root(HTML::LocalNavigable& root, CSSPixelPoint visual_viewport_position)
{
    auto document = root.active_document();
    if (!document || !document->is_fully_active())
        return {};
    // NB: The hit test goes into the documents of same-process frames, which are laid out as they would be for an event.
    for (auto& navigable : root.hosted_inclusive_descendant_navigables()) {
        if (auto navigable_document = navigable->active_document(); navigable_document && navigable_document->is_fully_active())
            navigable_document->update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseDown);
    }
    return root.event_handler().target_node_for_mouse_position(visual_viewport_position);
}

static bool hits_target(GuardedClick const& click, DOM::Node const* hit)
{
    if (!hit)
        return false;
    return click.target.visit(
        [&](UniqueNodeID element_id) {
            auto const* element = DOM::Node::from_unique_id(element_id);
            return element && element->is_shadow_including_inclusive_ancestor_of(*hit);
        },
        [&](HTML::CrossProcessId frame_id) {
            auto const* container = as_if<HTML::NavigableContainer>(*hit);
            return container && container->content_navigable() && container->content_navigable()->id() == frame_id;
        });
}

GuardedClickDecision decide_guarded_click(Page& page, HTML::LocalNavigable& root, Optional<HTML::CrossProcessId> const& root_id, MouseEvent const& event)
{
    prune_expired_clicks();
    auto page_id = page.client().id();
    auto index = guarded_clicks().find_first_index_if([&](auto const& active) {
        return active.click.page_id == page_id && active.click.root == root_id && active.click.event_ids.contains_slow(event.id);
    });
    if (!index.has_value())
        return GuardedClickDecision::NotGuarded;

    auto& active = guarded_clicks()[*index];
    auto is_release = event.type == MouseEvent::Type::MouseUp;
    if (active.ended) {
        if (is_release)
            guarded_clicks().remove(*index);
        return GuardedClickDecision::Drop;
    }

    auto hit = node_hit_from_local_root(root, page.device_to_css_point(event.position));
    if (!hits_target(active.click, hit.ptr())) {
        auto cancel_press = is_release && active.pressed_root.has_value();
        auto on_finish = move(active.click.on_finish);
        active.ended = true;
        if (is_release)
            guarded_clicks().remove(*index);
        if (on_finish)
            on_finish(false);
        return cancel_press ? GuardedClickDecision::DropAndCancelPress : GuardedClickDecision::Drop;
    }

    if (event.type == MouseEvent::Type::MouseDown)
        active.pressed_root = root_id;
    if (is_release) {
        auto finished = guarded_clicks().take(*index);
        if (finished.click.on_finish && finished.click.target.has<UniqueNodeID>())
            finished.click.on_finish(true);
    }
    return GuardedClickDecision::Dispatch;
}

}
