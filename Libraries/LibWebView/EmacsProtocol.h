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
    HintKind kind { HintKind::Other };
    String text;
    Optional<String> url;
    double x { 0 };
    double y { 0 };
    double width { 0 };
    double height { 0 };
};

struct HintCollection {
    Vector<HintItem> items;
    bool truncated { false };
};

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
// What the page's hint script reports of the elements it found, as JSON. The page can write anything there: a field of
// the wrong type gets a default, text is cut to its limit, and an item beyond the limit makes the collection truncated.
WEBVIEW_API ErrorOr<HintCollection> parse_hint_collection(StringView json);

}
