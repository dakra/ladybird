/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/Rect.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>
#include <LibWebView/Export.h>

// The wire format between the Emacs headless mode and its client: length-prefixed records that hold either a JSON
// control message or a frame of pixels.
namespace WebView::EmacsProtocol {

constexpr u32 protocol_version = 1;

// The length field of a record counts the kind byte and the payload.
constexpr size_t maximum_record_length = 64 * MiB;
constexpr size_t record_header_size = 5;
constexpr size_t frame_header_size = 44;

enum class RecordKind : u8 {
    Control = 1,
    Frame = 2,
};

enum class FrameEncoding : u8 {
    Raw = 0,
    Zstd = 1,
    Shm = 2,
};

constexpr u8 frame_flag_full = 1 << 0;
constexpr u8 frame_flag_more = 1 << 1;

// The Compositor pads a backing store by 256 pixels each way while a view is resized, and a Metal texture is at most
// 16384 pixels wide or high.
constexpr int maximum_viewport_dimension = 16000;
// A frame of this many pixels takes 256 MiB, of which the Compositor keeps three.
constexpr i64 maximum_viewport_area = 64'000'000;
// The viewports of all views of a server together have at most this many pixels, for which the Compositor keeps about
// 3 GiB.
constexpr i64 maximum_total_viewport_area = 4 * maximum_viewport_area;
constexpr double minimum_device_pixel_ratio = 0.25;
constexpr double maximum_device_pixel_ratio = 8.0;

// Text that comes from a page is cut to this many bytes of UTF-8 before it goes into a control message, which keeps
// every message well within a record.
constexpr size_t maximum_text_bytes = 1 * MiB;

// Text made fit for the wire: valid UTF-8, with a replacement character for what is not, of a bounded length.
WEBVIEW_API String wire_text(StringView);

struct Viewport {
    Web::DevicePixelSize size;
    double device_pixel_ratio { 1.0 };
};

// The viewport a message asks for (`width`, `height`, `dpr`), with what it leaves out from the fallback. The size is
// brought within what the Compositor can allocate and within the area that is left; a device pixel ratio out of range
// is refused, and so is any viewport when no area is left.
WEBVIEW_API ErrorOr<Viewport> viewport_from(JsonObject const&, Viewport fallback, i64 area_left = maximum_viewport_area);

struct FrameHeader {
    u32 seq { 0 };
    u32 view { 0 };
    u32 width { 0 };
    u32 height { 0 };
    double dpr { 1.0 };
    u32 damage_x { 0 };
    u32 damage_y { 0 };
    u32 damage_width { 0 };
    u32 damage_height { 0 };
    FrameEncoding encoding { FrameEncoding::Raw };
    u8 flags { 0 };
};

// These fail for a payload that is too long for a record.
WEBVIEW_API ErrorOr<void> append_record_header(ByteBuffer&, RecordKind, size_t payload_length);
WEBVIEW_API ErrorOr<ByteBuffer> encode_control_record(JsonObject const&);
// Starts a record whose payload is appended next, and gives it its length once the payload is whole.
WEBVIEW_API void begin_record(ByteBuffer&, RecordKind);
WEBVIEW_API ErrorOr<void> finish_record(ByteBuffer&);
WEBVIEW_API void append_frame_header(ByteBuffer&, FrameHeader const&);

struct Record {
    RecordKind kind { RecordKind::Control };
    ByteBuffer payload;
};

// Splits a byte stream that arrives in arbitrary pieces into records. A stream with a bad length or kind cannot be
// read any further: the reader then yields nothing more and reports the error.
class WEBVIEW_API RecordReader {
public:
    void feed(ReadonlyBytes);
    Optional<Record> next();
    bool has_error() const { return m_has_error; }

private:
    ByteBuffer m_buffer;
    size_t m_offset { 0 };
    bool m_has_error { false };
};

// A key is named by its KeyCode enumerator without the Key_ prefix.
WEBVIEW_API Optional<Web::UIEvents::KeyCode> key_code_from_name(StringView);
WEBVIEW_API Web::UIEvents::KeyModifier modifiers_from_protocol(u32);
WEBVIEW_API Optional<Web::UIEvents::MouseButton> mouse_button_from_name(StringView);
WEBVIEW_API StringView css_cursor_name(Gfx::Cursor const&);

// Hints: labels the server puts on the elements of a page that can be activated.
constexpr size_t maximum_hint_count = 1000;
constexpr size_t maximum_hint_text_length = 200;
// A link with a longer URL is listed without one.
constexpr size_t maximum_hint_url_length = 16 * KiB;

enum class HintKind : u8 {
    Link,
    Button,
    Input,
    Other,
};

struct HintItem {
    // The element, as the process hosting its document names it.
    i64 node { 0 };
    HintKind kind { HintKind::Other };
    String text;
    Optional<String> url;
    // In the device pixels of the viewport of the item's local root.
    double x { 0 };
    double y { 0 };
    double width { 0 };
    double height { 0 };
};

// The items of one document of a process.
struct HintRoot {
    // The local root whose viewport the boxes are in: the top-level traversable (none), or a frame whose parent another
    // process hosts.
    Optional<Web::HTML::CrossProcessId> navigable;
    // The navigable of the document, which is the local root's or that of a frame the same process hosts in it.
    Optional<Web::HTML::CrossProcessId> frame;
    Vector<HintItem> items;
};

// A frame another process hosts, and whether its parent's document shows it.
struct HintFrame {
    Web::HTML::CrossProcessId navigable;
    bool exposed { false };
};

// What a process found that can be clicked in the documents it hosts.
struct HintAnswer {
    Vector<HintRoot> roots;
    Vector<HintFrame> frames;
    // The process left candidates out.
    bool truncated { false };
};

// A navigable of the tab as the UI process knows it, against which it checks what the processes report.
struct HintNavigable {
    Web::HTML::CrossProcessId id;
    // The index of the parent in the list of navigables, which is in tree order.
    Optional<size_t> parent;
    // The process hosting its document, as an index the caller chooses.
    size_t host { 0 };
    // Its parent's document is in another process.
    bool remote { false };
    // The origin of the viewport of the local root that holds it, in the device pixels of the top-level viewport.
    Gfx::IntPoint local_root_offset;
    // For a remote navigable, the part of its viewport the top-level viewport shows, in its own device pixels.
    Gfx::IntRect visible;
};

struct HintAnswerFromProcess {
    size_t host { 0 };
    HintAnswer answer;
};

struct HintListItem {
    size_t host { 0 };
    // In the CSS pixels of the top-level viewport, cut to what the item's frames show of it.
    HintItem item;
};

struct HintList {
    Vector<HintListItem> items;
    bool truncated { false };
};

// The items of the answers, in the tree order of their documents' navigables, with what a process may not report left
// out: a process speaks only for the documents it hosts, and only the process hosting a frame's parent says whether it
// shows the frame. A frame another process hosts adds items only when that process says it shows the frame, and the same
// holds for every such frame it is in. The first navigable is the top-level traversable.
WEBVIEW_API HintList merge_hint_answers(Vector<HintAnswerFromProcess> const&, Vector<HintNavigable> const& navigables, Gfx::IntSize viewport, double device_pixels_per_css_pixel);

// 2 to 36 distinct characters of a-z and 0-9.
WEBVIEW_API bool is_valid_hint_alphabet(StringView);
// A client names a hint session with a non-negative integer below 2^53.
WEBVIEW_API Optional<u64> hint_session_id(JsonValue const&);
// The shortest length of labels that gives each of COUNT items its own.
WEBVIEW_API size_t hint_label_length(size_t alphabet_length, size_t count);
// Characters of the alphabet, no more than the labels have.
WEBVIEW_API bool is_valid_hint_prefix(StringView prefix, StringView alphabet, size_t label_length);
// The labels of COUNT items: all of the shortest length that gives each its own, in the order of the alphabet.
WEBVIEW_API Vector<String> hint_labels(StringView alphabet, size_t count);
// One character per label: '1' when it starts with the prefix, else '0'.
WEBVIEW_API String hint_label_mask(Vector<String> const& labels, StringView prefix);
// The index of an item that hints_end asks to activate, if `activate` names one of COUNT items.
WEBVIEW_API Optional<size_t> hint_activation_index(JsonValue const& activate, size_t count);
WEBVIEW_API StringView hint_kind_name(HintKind);
// A process names a navigable as "NAMESPACE:LOCAL".
WEBVIEW_API Optional<Web::HTML::CrossProcessId> hint_navigable_from_name(StringView);
// The answer of a process to a hint collection. A field of the wrong type gets a default, text is cut to its limit, an
// item without a node is left out, and items beyond 1001 are.
WEBVIEW_API ErrorOr<HintAnswer> parse_hint_answer(JsonValue const&);

}
