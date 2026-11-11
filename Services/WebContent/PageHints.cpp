/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/NeverDestroyed.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLabelElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/GuardedClick.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/SVGAElement.h>
#include <LibWeb/Selection/Selection.h>
#include <WebContent/PageHints.h>

namespace WebContent::PageHints {

using Web::CSSPixelPoint;
using Web::CSSPixelRect;
using Web::CSSPixels;
using Web::TraversalDecision;
using Web::UniqueNodeID;

// An element the last collection found, with what places its document's viewport in its local root's viewport.
struct Candidate {
    Optional<Web::HTML::CrossProcessId> root;
    CSSPixelPoint offset;
    CSSPixelRect clip;
    CSSPixelPoint label_position;
};

struct Label {
    UniqueNodeID element { 0 };
    UniqueNodeID host { 0 };
    UniqueNodeID box { 0 };
    UniqueNodeID typed { 0 };
    UniqueNodeID rest { 0 };
    String text;
    // Relative to the host, which was at the host origin in its document's viewport.
    CSSPixelPoint position;
    CSSPixelPoint host_origin;
    bool shown { true };
};

struct State {
    HashMap<UniqueNodeID, Candidate> candidates;
    Vector<UniqueNodeID> label_hosts;
    Vector<Label> labels;
};

static HashMap<Web::PageId, State>& states()
{
    static NeverDestroyed<HashMap<Web::PageId, State>> states;
    return *states;
}

static constexpr size_t maximum_text_length = 200;
// A page has at most this many labels; the UI process lists no more either.
static constexpr size_t maximum_candidates = 1001;
static constexpr auto guarded_click_lifetime = AK::Duration::from_seconds(5);
// A label sits this far up and to the left of the corner of what it labels, so that it covers less of it.
static constexpr int label_overhang = 6;

static constexpr StringView host_style = "all: initial !important; display: block !important; position: absolute !important; "
                                         "left: 0 !important; top: 0 !important; width: 0 !important; height: 0 !important; "
                                         "margin: 0 !important; padding: 0 !important; border: 0 !important; overflow: visible !important; "
                                         "z-index: 2147483647 !important; pointer-events: none !important; visibility: visible !important; "
                                         "opacity: 1 !important; transform: none !important; filter: none !important;"sv;
static constexpr StringView label_style = "position: absolute; box-sizing: border-box; margin: 0; padding: 0 2px; "
                                          "border: 1px solid #a08a00; border-radius: 2px; background: #fff3a3; color: #1a1a1a; "
                                          "font: bold 11px/13px monospace; letter-spacing: 0; white-space: nowrap; text-transform: none; "
                                          "text-decoration: none; pointer-events: none;"sv;

static String root_name(Optional<Web::HTML::CrossProcessId> const& root)
{
    if (!root.has_value())
        return {};
    return MUST(String::formatted("{}:{}", root->namespace_id, root->local_id));
}

static Optional<Web::HTML::CrossProcessId> root_id_of(Web::Page& page, Web::HTML::LocalNavigable& root)
{
    if (page.has_local_traversable() && page.local_traversable().ptr() == &root)
        return {};
    return root.id();
}

static GC::Ptr<Web::HTML::LocalNavigable> resolve_root(Web::Page& page, Optional<Web::HTML::CrossProcessId> const& root)
{
    if (!root.has_value())
        return page.has_local_traversable() ? page.local_traversable().ptr() : nullptr;
    return as_if<Web::HTML::LocalNavigable>(page.navigable_with_id(*root).ptr());
}

// The part of the local root's viewport that the top-level viewport shows, in the local root's viewport.
static CSSPixelRect visible_part_of_root(Web::HTML::LocalNavigable& root)
{
    CSSPixelRect viewport { {}, root.viewport_size() };
    if (auto intersection = root.viewport_intersection(); intersection.has_value())
        return viewport.intersected(*intersection);
    return viewport;
}

static Utf16FlyString const& role_attribute()
{
    static NeverDestroyed<Utf16FlyString> name { "role"_utf16_fly_string };
    return *name;
}

static Utf16FlyString const& aria_label_attribute()
{
    static NeverDestroyed<Utf16FlyString> name { "aria-label"_utf16_fly_string };
    return *name;
}

static bool is_summary(Web::DOM::Element const& element)
{
    return is<Web::HTML::HTMLElement>(element) && element.local_name() == Web::HTML::TagNames::summary;
}

static String utf8(Optional<Utf16String> const& text)
{
    if (!text.has_value())
        return {};
    return text->to_utf8_but_should_be_ported_to_utf16();
}

static String role_of(Web::DOM::Element const& element)
{
    auto role = utf8(element.get_attribute(role_attribute()));
    auto first = role.bytes_as_string_view().trim_whitespace().split_view(' ');
    if (first.is_empty())
        return {};
    return MUST(String::from_utf8(first.first())).to_ascii_lowercase();
}

static Optional<Utf16String> href_of(Web::DOM::Element const& element)
{
    if (is<Web::HTML::HTMLAnchorElement>(element) || is<Web::HTML::HTMLAreaElement>(element))
        return element.get_attribute(Web::HTML::AttributeNames::href);
    if (is<Web::SVG::SVGAElement>(element)) {
        if (auto href = element.get_attribute(Web::HTML::AttributeNames::href); href.has_value())
            return href;
        return element.get_attribute_ns(Web::Namespace::XLink, Web::HTML::AttributeNames::href);
    }
    return {};
}

static bool is_link(Web::DOM::Element const& element)
{
    return href_of(element).has_value();
}

static bool is_text_field(Web::DOM::Element const& element)
{
    using enum Web::HTML::HTMLInputElement::TypeAttributeState;
    if (is<Web::HTML::HTMLTextAreaElement>(element))
        return true;
    if (auto const* input = as_if<Web::HTML::HTMLInputElement>(element))
        return first_is_one_of(input->type_state(), Text, Search, Telephone, URL, Email, Password, Number);
    return element.is_editing_host();
}

enum class Qualification : u8 {
    None,
    // Something a user clicks for what it is.
    Strong,
    // Something that may only be a wrapper of what a user clicks: a label, or an element that can be clicked by its
    // tabindex, onclick or role.
    Weak,
};

static Qualification qualification_of(Web::DOM::Element const& element)
{
    if (is_link(element))
        return Qualification::Strong;
    if (auto const* input = as_if<Web::HTML::HTMLInputElement>(element))
        return input->type_state() == Web::HTML::HTMLInputElement::TypeAttributeState::Hidden ? Qualification::None : Qualification::Strong;
    if (is<Web::HTML::HTMLButtonElement>(element) || is<Web::HTML::HTMLSelectElement>(element) || is<Web::HTML::HTMLTextAreaElement>(element) || is_summary(element))
        return Qualification::Strong;
    if (auto editable = utf8(element.get_attribute(Web::HTML::AttributeNames::contenteditable)); element.has_attribute(Web::HTML::AttributeNames::contenteditable) && !editable.equals_ignoring_ascii_case("false"sv))
        return Qualification::Strong;
    if (is<Web::HTML::HTMLLabelElement>(element) || element.has_attribute(Web::HTML::AttributeNames::onclick))
        return Qualification::Weak;
    if (auto tabindex = utf8(element.get_attribute(Web::HTML::AttributeNames::tabindex)).to_number<i32>(); tabindex.has_value() && *tabindex >= 0)
        return Qualification::Weak;
    static constexpr Array roles = { "button"sv, "link"sv, "tab"sv, "menuitem"sv, "checkbox"sv, "radio"sv, "switch"sv, "option"sv };
    if (auto role = role_of(element); roles.contains_slow(role.bytes_as_string_view()))
        return Qualification::Weak;
    return Qualification::None;
}

static StringView kind_of(Web::DOM::Element const& element)
{
    using enum Web::HTML::HTMLInputElement::TypeAttributeState;
    auto role = role_of(element);
    if (is_link(element) || role == "link"sv)
        return "link"sv;
    if (is_text_field(element))
        return "input"sv;
    if (is<Web::HTML::HTMLButtonElement>(element) || is_summary(element) || role == "button"sv)
        return "button"sv;
    if (auto const* input = as_if<Web::HTML::HTMLInputElement>(element); input && first_is_one_of(input->type_state(), SubmitButton, ResetButton, Button, ImageButton))
        return "button"sv;
    return "other"sv;
}

// Whitespace runs become one space, and the text is cut to its limit.
static String collapsed(StringView text)
{
    StringBuilder builder;
    size_t code_points = 0;
    bool pending_space = false;
    for (auto code_point : Utf8View { text }) {
        if (is_ascii_space(code_point) || code_point == 0xa0) {
            pending_space = !builder.is_empty();
            continue;
        }
        if (pending_space) {
            if (++code_points > maximum_text_length)
                break;
            builder.append(' ');
            pending_space = false;
        }
        if (++code_points > maximum_text_length)
            break;
        builder.append_code_point(code_point);
    }
    return builder.to_string_without_validation();
}

static String text_of_element(Web::DOM::Element& element)
{
    if (auto* html_element = as_if<Web::HTML::HTMLElement>(element))
        return html_element->inner_text().to_utf8_but_should_be_ported_to_utf16();
    return element.text_content().value_or({}).to_utf8_but_should_be_ported_to_utf16();
}

static String label_text_of(Web::DOM::Element& element)
{
    auto* html_element = as_if<Web::HTML::HTMLElement>(element);
    if (!html_element)
        return {};
    auto labels = html_element->labels();
    if (!labels || labels->length() == 0)
        return {};
    if (auto* label = as_if<Web::DOM::Element>(const_cast<Web::DOM::Node*>(labels->item(0))))
        return text_of_element(*label);
    return {};
}

static String hint_text_of(Web::DOM::Element& element)
{
    using enum Web::HTML::HTMLInputElement::TypeAttributeState;
    auto attribute = [&](Utf16FlyString const& name) { return utf8(element.get_attribute(name)); };
    auto first_of = [](std::initializer_list<String> texts) {
        for (auto const& text : texts) {
            if (auto result = collapsed(text); !result.is_empty())
                return result;
        }
        return String {};
    };

    if (auto label = collapsed(attribute(aria_label_attribute())); !label.is_empty())
        return label;

    if (auto* input = as_if<Web::HTML::HTMLInputElement>(element)) {
        String own;
        if (first_is_one_of(input->type_state(), SubmitButton, ResetButton, Button))
            own = input->value().to_utf8_but_should_be_ported_to_utf16();
        else if (input->type_state() == ImageButton)
            own = attribute(Web::HTML::AttributeNames::alt);
        else if (!first_is_one_of(input->type_state(), Checkbox, RadioButton, Password, FileUpload))
            own = input->value().to_utf8_but_should_be_ported_to_utf16();
        return first_of({ own, label_text_of(element), attribute(Web::HTML::AttributeNames::placeholder), attribute(Web::HTML::AttributeNames::title) });
    }
    if (auto* text_area = as_if<Web::HTML::HTMLTextAreaElement>(element))
        return first_of({ text_area->value().to_utf8_but_should_be_ported_to_utf16(), label_text_of(element), attribute(Web::HTML::AttributeNames::placeholder), attribute(Web::HTML::AttributeNames::title) });
    if (auto* select = as_if<Web::HTML::HTMLSelectElement>(element)) {
        String selected;
        select->for_each_in_subtree_of_type<Web::HTML::HTMLOptionElement>([&](Web::HTML::HTMLOptionElement& option) {
            if (!option.selected())
                return TraversalDecision::Continue;
            selected = option.text().to_utf8_but_should_be_ported_to_utf16();
            return TraversalDecision::Break;
        });
        return first_of({ selected, label_text_of(element), attribute(Web::HTML::AttributeNames::title) });
    }

    String image_text;
    element.for_each_in_subtree_of_type<Web::DOM::Element>([&](Web::DOM::Element& descendant) {
        if (!descendant.has_attribute(Web::HTML::AttributeNames::alt))
            return TraversalDecision::Continue;
        image_text = utf8(descendant.get_attribute(Web::HTML::AttributeNames::alt));
        return image_text.is_empty() ? TraversalDecision::Continue : TraversalDecision::Break;
    });
    return first_of({ text_of_element(element), image_text, attribute(Web::HTML::AttributeNames::title) });
}

static Optional<String> url_of(Web::DOM::Element const& element)
{
    auto href = href_of(element);
    if (!href.has_value())
        return {};
    auto url = element.document().encoding_parse_url(*href);
    if (!url.has_value())
        return {};
    return url->serialize();
}

static bool has_opacity_zero(Web::DOM::Node const& node, Web::Layout::BegunRead const& read)
{
    for (auto const* ancestor = &node; ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        if (auto const* layout_node = as_if<Web::Layout::NodeWithStyle>(ancestor->layout_node(read)); layout_node && layout_node->opacity() == 0)
            return true;
    }
    return false;
}

// The boxes of an element, in its document's viewport. An element with `display: contents` has none of its own, and
// shows as the boxes of its children.
static Vector<CSSPixelRect> client_rects_of(Web::DOM::Element& element)
{
    if (!element.has_display_contents())
        return element.get_client_rects();
    Vector<CSSPixelRect> rects;
    for (auto* child = element.first_child(); child; child = child->next_sibling()) {
        if (auto* child_element = as_if<Web::DOM::Element>(child))
            rects.extend(client_rects_of(*child_element));
    }
    return rects;
}

static CSSPixelRect bounding_rect_of(Vector<CSSPixelRect> const& rects)
{
    Optional<CSSPixelRect> bounds;
    for (auto const& rect : rects) {
        if (!rect.is_empty())
            bounds = bounds.has_value() ? bounds->united(rect) : rect;
    }
    return bounds.value_or({});
}

struct HitPoint {
    CSSPixelPoint point;
    CSSPixelPoint label_position;
};

// The points of a visible box a hit test tries, cheapest first: the middle, the corners and the middles of the edges
// just inside, then a grid over the rest.
static Vector<CSSPixelPoint, 34> points_to_try(CSSPixelRect const& box)
{
    Vector<CSSPixelPoint, 34> points;
    CSSPixels inset_x = min<CSSPixels>(box.width() / 2, 2);
    CSSPixels inset_y = min<CSSPixels>(box.height() / 2, 2);
    auto left = box.x() + inset_x, right = box.right() - inset_x, top = box.y() + inset_y, bottom = box.bottom() - inset_y;
    auto middle = box.center();
    points.append(middle);
    for (auto point : { CSSPixelPoint { left, top }, CSSPixelPoint { right, top }, CSSPixelPoint { left, bottom }, CSSPixelPoint { right, bottom },
             CSSPixelPoint { middle.x(), top }, CSSPixelPoint { middle.x(), bottom }, CSSPixelPoint { left, middle.y() }, CSSPixelPoint { right, middle.y() } })
        points.append(point);
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 5; ++column)
            points.append({ box.x() + box.width() * (2 * column + 1) / 10, box.y() + box.height() * (2 * row + 1) / 10 });
    }
    return points;
}

// A point of one of the element's boxes, in the local root's viewport, inside the clip, where a mouse event would hit
// the element or something in it. The first box that shows any of itself gives the label its place.
// NB: A mouse event is at a whole device pixel, so that is where the point is tried.
static Optional<HitPoint> hit_point_of(Web::Page& page, Web::HTML::LocalNavigable& root, Web::DOM::Element& element, Vector<CSSPixelRect> const& rects, CSSPixelPoint offset, CSSPixelRect const& clip)
{
    Optional<CSSPixelPoint> label_position;
    for (auto const& rect : rects) {
        auto visible = rect.translated(offset).intersected(clip);
        if (visible.width() < 1 || visible.height() < 1)
            continue;
        if (!label_position.has_value())
            label_position = visible.location();
        for (auto candidate : points_to_try(visible)) {
            auto point = page.device_to_css_point(page.css_to_device_point(candidate));
            if (!visible.contains(point))
                continue;
            auto hit = Web::node_hit_from_local_root(root, point);
            if (hit && element.is_shadow_including_inclusive_ancestor_of(*hit))
                return HitPoint { point, *label_position };
        }
    }
    return {};
}

struct Found {
    GC::Root<Web::DOM::Element> element;
    // The navigable of the element's document, or none for the page's traversable.
    Optional<Web::HTML::CrossProcessId> frame;
    Qualification qualification;
    Candidate candidate;
    CSSPixelRect rect;
};

struct Collection {
    Web::Page& page;
    Web::HTML::LocalNavigable& root;
    Optional<Web::HTML::CrossProcessId> root_id;
    Vector<Found> found;
    JsonArray frames;
    bool truncated { false };
};

// NB: Dropping redundant candidates leaves fewer, so more are found than are listed; past this many, the rest of the
//     documents are only searched for frames.
static constexpr size_t maximum_found = maximum_candidates * 4;

// Finds what can be clicked in the document and, after it, in the documents of its frames in this process. A document
// that is not HTML, such as an SVG document, gets no labels, as the elements that make them would not show in it.
static void collect_document(Collection& collection, Web::DOM::Document& document, Optional<Web::HTML::CrossProcessId> frame, CSSPixelPoint offset, CSSPixelRect clip)
{
    if (clip.is_empty() || !is<Web::HTML::HTMLElement>(document.document_element()))
        return;

    Web::Layout::ForcedReadScope read { document };
    document.update_layout(Web::DOM::UpdateLayoutReason::Debugging);

    struct Frame {
        GC::Root<Web::HTML::NavigableContainer> container;
        CSSPixelPoint origin;
        CSSPixelRect visible;
    };
    Vector<Frame> local_frames;

    document.for_each_shadow_including_inclusive_descendant([&](Web::DOM::Node& node) {
        // NB: What is inside a form control or the labels is no candidate.
        if (auto const* shadow_root = as_if<Web::DOM::ShadowRoot>(node); shadow_root && shadow_root->is_user_agent_internal())
            return TraversalDecision::SkipChildrenAndContinue;
        auto* element = as_if<Web::DOM::Element>(node);
        if (!element)
            return TraversalDecision::Continue;

        if (auto* container = as_if<Web::HTML::NavigableContainer>(*element); container && container->content_navigable()) {
            auto const* layout_node = container->layout_node(read);
            if (layout_node && Web::Painting::is_navigable_container_viewport_paintable(*layout_node)) {
                auto content_box = Web::Painting::absolute_rect(*layout_node);
                auto origin = Web::Painting::rect_to_viewport(*layout_node, { content_box.location(), {} }, Web::Layout::RustFFI::ViewportSpace::VisualViewport).location().translated(offset);
                auto visible = Web::Painting::rect_to_viewport(*layout_node, content_box, Web::Layout::RustFFI::ViewportSpace::VisualViewport).translated(offset).intersected(clip);
                auto transparent = has_opacity_zero(*container, read);
                if (is<Web::HTML::LocalNavigable>(*container->content_navigable())) {
                    if (!transparent)
                        local_frames.append({ GC::make_root(*container), origin, visible });
                } else {
                    // The page shows a frame another process hosts where a mouse event in the middle of its visible
                    // part would go to it.
                    auto exposed = false;
                    if (!visible.is_empty() && !transparent) {
                        auto hit = Web::node_hit_from_local_root(collection.root, visible.center());
                        exposed = hit.ptr() == container;
                    }
                    JsonObject frame;
                    frame.set("navigable"sv, root_name(container->content_navigable()->id()));
                    frame.set("exposed"sv, exposed);
                    collection.frames.must_append(move(frame));
                }
            }
        }

        auto qualification = qualification_of(*element);
        if (qualification == Qualification::None || element->is_actually_disabled())
            return TraversalDecision::Continue;
        if (collection.found.size() >= maximum_found) {
            collection.truncated = true;
            return TraversalDecision::Continue;
        }
        if (!element->has_display_contents() && !element->layout_node(read))
            return TraversalDecision::Continue;
        if (has_opacity_zero(*element, read))
            return TraversalDecision::Continue;
        auto rects = client_rects_of(*element);
        auto rect = bounding_rect_of(rects);
        if (rect.width() < 3 || rect.height() < 3)
            return TraversalDecision::Continue;
        auto hit_point = hit_point_of(collection.page, collection.root, *element, rects, offset, clip);
        if (!hit_point.has_value())
            return TraversalDecision::Continue;
        collection.found.append({
            GC::make_root(*element),
            frame,
            qualification,
            Candidate { collection.root_id, offset, clip, hit_point->label_position.translated(-offset) },
            rect.translated(offset),
        });
        return TraversalDecision::Continue;
    });

    for (auto& frame : local_frames) {
        auto* navigable = as_if<Web::HTML::LocalNavigable>(frame.container->content_navigable().ptr());
        if (navigable && navigable->active_document())
            collect_document(collection, *navigable->active_document(), navigable->id(), frame.origin, frame.visible);
    }
}

// A label whose control is listed, and a weak candidate around another listed one, add nothing.
static void drop_redundant(Vector<Found>& found)
{
    HashTable<Web::DOM::Node const*> labels_of_listed_controls;
    for (auto const& item : found) {
        auto* html_element = as_if<Web::HTML::HTMLElement>(*item.element);
        if (!html_element || is<Web::HTML::HTMLLabelElement>(*html_element))
            continue;
        auto labels = html_element->labels();
        for (u32 index = 0; labels && index < labels->length(); ++index)
            labels_of_listed_controls.set(labels->item(index));
    }
    found.remove_all_matching([&](Found const& item) {
        return labels_of_listed_controls.contains(item.element.ptr());
    });

    HashTable<Web::DOM::Node const*> containing_another;
    for (auto const& item : found) {
        for (auto const* ancestor = item.element->parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host())
            containing_another.set(ancestor);
    }
    found.remove_all_matching([&](Found const& item) {
        return item.qualification == Qualification::Weak && containing_another.contains(item.element.ptr());
    });
}

JsonValue collect(Web::Page& page)
{
    clear(page);
    auto& state = states().ensure(page.client().id());
    auto scale = page.client().device_pixels_per_css_pixel();

    // NB: Page::local_roots() leaves out a local root inside a frame another process hosts.
    Vector<GC::Root<Web::HTML::LocalNavigable>> local_roots;
    if (page.has_top_level_traversable()) {
        for (auto& navigable : page.top_level_traversable()->active_document_inclusive_descendant_navigables()) {
            auto* local_navigable = as_if<Web::HTML::LocalNavigable>(*navigable);
            if (local_navigable && !local_navigable->has_been_destroyed() && local_navigable->is_local_root())
                local_roots.append(*local_navigable);
        }
    }

    JsonArray roots;
    JsonArray frames;
    bool truncated = false;
    for (auto& root : local_roots) {
        auto document = root->active_document();
        if (!document)
            continue;
        Collection collection { page, *root, root_id_of(page, *root), {}, {}, false };
        collect_document(collection, *document, collection.root_id, {}, visible_part_of_root(*root));
        drop_redundant(collection.found);
        truncated |= collection.truncated;
        collection.frames.for_each([&](JsonValue const& frame) { frames.must_append(frame); });

        // One entry per document, in the order the documents were visited, which names the local root its boxes are
        // in and the navigable the document is in.
        Optional<JsonObject> entry;
        JsonArray items;
        Optional<Web::HTML::CrossProcessId> entry_frame;
        auto finish_entry = [&] {
            if (!entry.has_value())
                return;
            entry->set("items"sv, move(items));
            items = {};
            roots.must_append(entry.release_value());
        };
        for (auto& found : collection.found) {
            if (state.candidates.size() >= maximum_candidates) {
                truncated = true;
                break;
            }
            if (!entry.has_value() || entry_frame != found.frame) {
                finish_entry();
                entry = JsonObject {};
                entry_frame = found.frame;
                entry->set("navigable"sv, collection.root_id.has_value() ? JsonValue { root_name(collection.root_id) } : JsonValue {});
                entry->set("frame"sv, found.frame.has_value() ? JsonValue { root_name(found.frame) } : JsonValue {});
            }
            auto node_id = found.element->unique_id();
            JsonObject item;
            item.set("node"sv, static_cast<i64>(node_id.value()));
            item.set("kind"sv, kind_of(*found.element));
            item.set("text"sv, hint_text_of(*found.element));
            if (auto url = url_of(*found.element); url.has_value())
                item.set("url"sv, url.release_value());
            else
                item.set("url"sv, JsonValue {});
            item.set("x"sv, found.rect.x().to_double() * scale);
            item.set("y"sv, found.rect.y().to_double() * scale);
            item.set("width"sv, found.rect.width().to_double() * scale);
            item.set("height"sv, found.rect.height().to_double() * scale);
            items.must_append(move(item));
            state.candidates.set(node_id, move(found.candidate));
        }
        finish_entry();
    }

    JsonObject result;
    result.set("roots"sv, move(roots));
    result.set("frames"sv, move(frames));
    result.set("truncated"sv, truncated);
    return result;
}

static GC::Ref<Web::DOM::Element> create_element(Web::DOM::Document& document, Utf16FlyString const& name, StringView style)
{
    auto element = MUST(Web::DOM::create_element(document, name, Web::Namespace::HTML));
    element->set_attribute_value(Web::HTML::AttributeNames::style, Utf16String::from_utf8(style));
    return element;
}

static void update_label(Label& label, bool shown, size_t typed_length)
{
    auto* box = as_if<Web::DOM::Element>(Web::DOM::Node::from_unique_id(label.box));
    auto* typed = as_if<Web::DOM::Text>(Web::DOM::Node::from_unique_id(label.typed));
    auto* rest = as_if<Web::DOM::Text>(Web::DOM::Node::from_unique_id(label.rest));
    if (!box || !typed || !rest)
        return;
    label.shown = shown;
    typed_length = min(typed_length, label.text.bytes().size());
    typed->set_data(Utf16String::from_utf8(label.text.bytes_as_string_view().substring_view(0, typed_length)));
    rest->set_data(Utf16String::from_utf8(label.text.bytes_as_string_view().substring_view(typed_length)));
    auto style = MUST(String::formatted("{} left: {}px; top: {}px; display: {};", label_style, label.position.x(), label.position.y(), shown ? "block"sv : "none"sv));
    box->set_attribute_value(Web::HTML::AttributeNames::style, Utf16String::from_utf8(style));
}

// The page can change the host's attributes, which are in its reach; the labels' own styles are not.
static void restore_host_styles(State const& state)
{
    for (auto host_id : state.label_hosts) {
        if (auto* host = as_if<Web::DOM::Element>(Web::DOM::Node::from_unique_id(host_id)))
            host->set_attribute_value(Web::HTML::AttributeNames::style, Utf16String::from_utf8(host_style));
    }
}

void draw(Web::Page& page, JsonValue const& labels)
{
    auto state = states().find(page.client().id());
    if (state == states().end() || !labels.is_array())
        return;

    struct Host {
        GC::Root<Web::DOM::Element> element;
        GC::Root<Web::DOM::ShadowRoot> shadow_root;
    };
    HashMap<Web::DOM::Document*, Host> hosts;
    struct Placement {
        size_t label_index;
        GC::Root<Web::DOM::Element> box;
        GC::Root<Web::DOM::Element> host;
        CSSPixelPoint position;
        CSSPixelRect clip;
    };
    Vector<Placement> placements;

    labels.as_array().for_each([&](JsonValue const& pair) {
        if (!pair.is_array() || pair.as_array().size() != 2 || !pair.as_array()[0].is_integer<i64>() || !pair.as_array()[1].is_string())
            return;
        UniqueNodeID node_id { pair.as_array()[0].as_integer<i64>() };
        auto candidate = state->value.candidates.get(node_id);
        auto* element = as_if<Web::DOM::Element>(Web::DOM::Node::from_unique_id(node_id));
        if (!candidate.has_value() || !element || !element->is_connected() || !element->document().document_element())
            return;
        auto& document = element->document();

        auto host = hosts.get(&document);
        if (!host.has_value()) {
            auto host_element = create_element(document, "ladybirdhints"_utf16_fly_string, host_style);
            auto shadow_root = Web::DOM::ShadowRoot::create(document, host_element, Web::DOM::ShadowRootMode::Closed);
            shadow_root->set_user_agent_internal(true);
            host_element->set_shadow_root(shadow_root);
            MUST(document.document_element()->append_child(host_element));
            state->value.label_hosts.append(host_element->unique_id());
            hosts.set(&document, { GC::make_root(host_element), GC::make_root(shadow_root) });
            host = hosts.get(&document);
        }

        auto box = create_element(document, Web::HTML::TagNames::span, label_style);
        auto typed_part = create_element(document, Web::HTML::TagNames::span, "color: #d04000;"sv);
        auto typed = Web::DOM::Text::create(document, {});
        auto rest = Web::DOM::Text::create(document, Utf16String::from_utf8(pair.as_array()[1].as_string()));
        MUST(typed_part->append_child(typed));
        MUST(box->append_child(typed_part));
        MUST(box->append_child(rest));
        MUST(host->shadow_root->append_child(box));

        state->value.labels.append({ node_id, host->element->unique_id(), box->unique_id(), typed->unique_id(), rest->unique_id(), pair.as_array()[1].as_string(), {}, {}, true });
        placements.append({ state->value.labels.size() - 1, GC::make_root(box), host->element, candidate->label_position, candidate->clip.translated(-candidate->offset) });
    });

    // A label straddles the top-left corner of the first box of its element that shows, moved into what shows of the
    // element's document.
    // NB: Every box is measured before any moves, so that the document is laid out once.
    for (auto& placement : placements) {
        auto host_rect = placement.host->get_bounding_client_rect();
        auto box_rect = placement.box->get_bounding_client_rect();
        auto x = max(placement.clip.x(), min(placement.position.x() - label_overhang, placement.clip.right() - box_rect.width()));
        auto y = max(placement.clip.y(), min(placement.position.y() - label_overhang, placement.clip.bottom() - box_rect.height()));
        auto& label = state->value.labels[placement.label_index];
        label.position = { x - host_rect.x(), y - host_rect.y() };
        label.host_origin = host_rect.location();
    }
    for (auto& label : state->value.labels)
        update_label(label, true, 0);
}

void narrow(Web::Page& page, StringView prefix)
{
    auto state = states().find(page.client().id());
    if (state == states().end())
        return;
    restore_host_styles(state->value);
    for (auto& label : state->value.labels) {
        auto shown = label.text.starts_with_bytes(prefix);
        update_label(label, shown, shown ? prefix.length() : 0);
    }
}

void clear(Web::Page& page)
{
    auto state = states().take(page.client().id());
    if (!state.has_value())
        return;
    for (auto host_id : state->label_hosts) {
        // NB: The page may have taken the host out already.
        if (auto* host = Web::DOM::Node::from_unique_id(host_id); host && host->parent())
            host->remove();
    }
}

void forget(Web::PageId page_id)
{
    states().remove(page_id);
    Web::forget_guarded_clicks(page_id);
}

static void place_caret(Web::DOM::Element& element)
{
    if (auto* input = as_if<Web::HTML::HTMLInputElement>(element)) {
        auto length = input->relevant_value().length_in_code_units();
        input->set_the_selection_range(length, length);
        return;
    }
    if (auto* text_area = as_if<Web::HTML::HTMLTextAreaElement>(element)) {
        auto length = text_area->relevant_value().length_in_code_units();
        text_area->set_the_selection_range(length, length);
        return;
    }
    if (auto selection = element.document().get_selection())
        (void)selection->collapse(&element, element.length());
}

// Whether the label of the element is where it was drawn, in a host that is attached and styled as it was put: the
// page can move or hide the host, which would make the user type the label of one element over another.
static bool label_is_intact(State const& state, UniqueNodeID element_id)
{
    static constexpr double tolerance = 0.5;
    auto label = state.labels.find_if([&](auto const& label) { return label.element == element_id; });
    if (label.is_end())
        return false;
    auto* host = as_if<Web::DOM::Element>(Web::DOM::Node::from_unique_id(label->host));
    auto* box = as_if<Web::DOM::Element>(Web::DOM::Node::from_unique_id(label->box));
    if (!host || !box || !host->parent() || host->parent() != host->document().document_element())
        return false;
    if (host->get_attribute(Web::HTML::AttributeNames::style) != Utf16String::from_utf8(host_style))
        return false;
    auto host_rect = host->get_bounding_client_rect();
    if (fabs((host_rect.x() - label->host_origin.x()).to_double()) > tolerance || fabs((host_rect.y() - label->host_origin.y()).to_double()) > tolerance)
        return false;
    if (!label->shown)
        return true;
    Web::Layout::ForcedReadScope read { box->document() };
    auto box_rect = box->get_bounding_client_rect();
    auto expected = host_rect.location().translated(label->position);
    return box->layout_node(read) && !box_rect.is_empty()
        && fabs((box_rect.x() - expected.x()).to_double()) <= tolerance && fabs((box_rect.y() - expected.y()).to_double()) <= tolerance;
}

JsonValue prepare_activation(Web::Page& page, UniqueNodeID node_id)
{
    JsonObject answer;
    answer.set("action"sv, "none"sv);

    auto state = states().find(page.client().id());
    if (state == states().end())
        return answer;
    auto candidate = state->value.candidates.get(node_id);
    auto* element = as_if<Web::DOM::Element>(Web::DOM::Node::from_unique_id(node_id));
    if (!candidate.has_value() || !element || !element->is_connected())
        return answer;
    auto root = resolve_root(page, candidate->root);
    if (!root)
        return answer;
    if (!label_is_intact(state->value, node_id)) {
        answer.set("action"sv, "tampered"sv);
        return answer;
    }

    // NB: The element is clicked where it is now, which a change of layout since the list may have moved it to.
    auto clip = candidate->clip.intersected(visible_part_of_root(*root));
    auto hit_point = hit_point_of(page, *root, *element, client_rects_of(*element), candidate->offset, clip);
    if (!hit_point.has_value())
        return answer;

    if (is_text_field(*element)) {
        Web::HTML::run_focusing_steps(element, nullptr, Web::HTML::FocusTrigger::Other, Web::HTML::ScrollIntoView::No);
        place_caret(*element);
        answer.set("action"sv, "focus"sv);
        return answer;
    }

    auto position = page.css_to_device_point(hit_point->point);
    answer.set("action"sv, "click"sv);
    if (candidate->root.has_value())
        answer.set("navigable"sv, root_name(candidate->root));
    else
        answer.set("navigable"sv, JsonValue {});
    answer.set("x"sv, position.x().value());
    answer.set("y"sv, position.y().value());
    return answer;
}

void guard_click(Web::Page& page, u64 request_id, Optional<UniqueNodeID> element, Optional<Web::HTML::CrossProcessId> frame, Optional<Web::HTML::CrossProcessId> root, Vector<u64> event_ids, Function<void(bool clicked)> on_finish)
{
    if (element.has_value() == frame.has_value() || event_ids.size() != 3)
        return;
    Web::guard_click({
        .page_id = page.client().id(),
        .request_id = request_id,
        .target = element.has_value() ? Variant<UniqueNodeID, Web::HTML::CrossProcessId> { *element } : Variant<UniqueNodeID, Web::HTML::CrossProcessId> { *frame },
        .root = root,
        .event_ids = move(event_ids),
        .expiry = MonotonicTime::now() + guarded_click_lifetime,
        .on_finish = move(on_finish),
    });
}

void cancel_click(Web::Page& page, u64 request_id)
{
    Web::cancel_guarded_click(page, request_id);
}

}
