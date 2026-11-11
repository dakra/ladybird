/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <AK/JsonArray.h>
#include <AK/Math.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibURL/Parser.h>
#include <LibWebView/EmacsProtocol.h>

namespace WebView::EmacsProtocol {

using Web::UIEvents::KeyCode;
using Web::UIEvents::KeyModifier;
using Web::UIEvents::MouseButton;

template<typename T>
static void append_little_endian(ByteBuffer& buffer, T value)
{
    LittleEndian<T> little_endian { value };
    buffer.append(&little_endian, sizeof(little_endian));
}

static void append_record_header_unchecked(ByteBuffer& buffer, RecordKind kind, size_t payload_length)
{
    append_little_endian<u32>(buffer, payload_length + 1);
    buffer.append(to_underlying(kind));
}

ErrorOr<void> append_record_header(ByteBuffer& buffer, RecordKind kind, size_t payload_length)
{
    if (payload_length + 1 > maximum_record_length)
        return Error::from_string_literal("The payload is too long for a record");
    append_record_header_unchecked(buffer, kind, payload_length);
    return {};
}

void begin_record(ByteBuffer& buffer, RecordKind kind)
{
    VERIFY(buffer.is_empty());
    append_record_header_unchecked(buffer, kind, 0);
}

ErrorOr<void> finish_record(ByteBuffer& buffer)
{
    VERIFY(buffer.size() >= record_header_size);
    auto length = buffer.size() - sizeof(u32);
    if (length > maximum_record_length)
        return Error::from_string_literal("The payload is too long for a record");
    LittleEndian<u32> little_endian { static_cast<u32>(length) };
    __builtin_memcpy(buffer.data(), &little_endian, sizeof(little_endian));
    return {};
}

void append_frame_header(ByteBuffer& buffer, FrameHeader const& header)
{
    auto size_before = buffer.size();
    append_little_endian<u32>(buffer, header.seq);
    append_little_endian<u32>(buffer, header.view);
    append_little_endian<u32>(buffer, header.width);
    append_little_endian<u32>(buffer, header.height);
    append_little_endian<u64>(buffer, bit_cast<u64>(header.dpr));
    append_little_endian<u32>(buffer, header.damage_x);
    append_little_endian<u32>(buffer, header.damage_y);
    append_little_endian<u32>(buffer, header.damage_width);
    append_little_endian<u32>(buffer, header.damage_height);
    buffer.append(to_underlying(header.encoding));
    buffer.append(header.flags);
    append_little_endian<u16>(buffer, 0);
    VERIFY(buffer.size() - size_before == frame_header_size);
}

ErrorOr<ByteBuffer> encode_control_record(JsonObject const& message)
{
    auto json = message.serialized();
    ByteBuffer record;
    TRY(record.try_ensure_capacity(record_header_size + json.byte_count()));
    TRY(append_record_header(record, RecordKind::Control, json.byte_count()));
    TRY(record.try_append(json.bytes()));
    return record;
}

void RecordReader::feed(ReadonlyBytes bytes)
{
    if (m_has_error)
        return;

    if (m_offset == m_buffer.size()) {
        m_buffer.clear();
        m_offset = 0;
    } else if (m_offset >= 64 * KiB) {
        auto remaining = m_buffer.size() - m_offset;
        __builtin_memmove(m_buffer.data(), m_buffer.data() + m_offset, remaining);
        m_buffer.resize(remaining);
        m_offset = 0;
    }
    m_buffer.append(bytes);
}

Optional<Record> RecordReader::next()
{
    if (m_has_error)
        return {};

    auto available = m_buffer.bytes().slice(m_offset);
    if (available.size() < record_header_size)
        return {};

    u32 length = 0;
    __builtin_memcpy(&length, available.data(), sizeof(length));
    length = AK::convert_between_host_and_little_endian(length);
    auto kind = available[4];

    if (length == 0 || length > maximum_record_length || (kind != to_underlying(RecordKind::Control) && kind != to_underlying(RecordKind::Frame))) {
        m_has_error = true;
        m_buffer.clear();
        m_offset = 0;
        return {};
    }

    auto record_size = sizeof(u32) + length;
    if (available.size() < record_size)
        return {};

    Record record;
    record.kind = static_cast<RecordKind>(kind);
    record.payload.append(available.slice(record_header_size, length - 1));
    m_offset += record_size;
    return record;
}

Optional<KeyCode> key_code_from_name(StringView name)
{
#define __ENUMERATE_KEY_CODE(key, ui_name, code) \
    if (name == #key##sv)                        \
        return KeyCode::Key_##key;
    ENUMERATE_KEY_CODES
#undef __ENUMERATE_KEY_CODE
    return {};
}

KeyModifier modifiers_from_protocol(u32 bits)
{
    auto modifiers = KeyModifier::Mod_None;
    if (bits & 1)
        modifiers |= KeyModifier::Mod_Shift;
    if (bits & 2)
        modifiers |= KeyModifier::Mod_Ctrl;
    if (bits & 4)
        modifiers |= KeyModifier::Mod_Alt;
    if (bits & 8)
        modifiers |= KeyModifier::Mod_Super;
    if (bits & 16)
        modifiers |= KeyModifier::Mod_Keypad;
    return modifiers;
}

Optional<MouseButton> mouse_button_from_name(StringView name)
{
    if (name == "left"sv)
        return MouseButton::Primary;
    if (name == "right"sv)
        return MouseButton::Secondary;
    if (name == "middle"sv)
        return MouseButton::Middle;
    if (name == "back"sv)
        return MouseButton::Backward;
    if (name == "forward"sv)
        return MouseButton::Forward;
    return {};
}

StringView css_cursor_name(Gfx::Cursor const& cursor)
{
    auto const* standard_cursor = cursor.get_pointer<Gfx::StandardCursor>();
    if (!standard_cursor)
        return "default"sv;

    switch (*standard_cursor) {
    case Gfx::StandardCursor::None:
    case Gfx::StandardCursor::Hidden:
        return "none"sv;
    case Gfx::StandardCursor::Arrow:
        return "default"sv;
    case Gfx::StandardCursor::Crosshair:
    case Gfx::StandardCursor::Eyedropper:
        return "crosshair"sv;
    case Gfx::StandardCursor::IBeam:
        return "text"sv;
    case Gfx::StandardCursor::ResizeHorizontal:
        return "ew-resize"sv;
    case Gfx::StandardCursor::ResizeVertical:
        return "ns-resize"sv;
    case Gfx::StandardCursor::ResizeDiagonalTLBR:
        return "nwse-resize"sv;
    case Gfx::StandardCursor::ResizeDiagonalBLTR:
        return "nesw-resize"sv;
    case Gfx::StandardCursor::ResizeColumn:
        return "col-resize"sv;
    case Gfx::StandardCursor::ResizeRow:
        return "row-resize"sv;
    case Gfx::StandardCursor::Hand:
        return "pointer"sv;
    case Gfx::StandardCursor::Help:
        return "help"sv;
    case Gfx::StandardCursor::OpenHand:
        return "grab"sv;
    case Gfx::StandardCursor::Drag:
        return "grabbing"sv;
    case Gfx::StandardCursor::DragCopy:
        return "copy"sv;
    case Gfx::StandardCursor::Move:
        return "move"sv;
    case Gfx::StandardCursor::Wait:
        return "wait"sv;
    case Gfx::StandardCursor::Disallowed:
        return "not-allowed"sv;
    case Gfx::StandardCursor::ZoomIn:
        return "zoom-in"sv;
    case Gfx::StandardCursor::ZoomOut:
        return "zoom-out"sv;
    }
    // NB: The cursor comes from a page's process, which can send a value that is none of these.
    return "default"sv;
}

bool is_valid_hint_alphabet(StringView alphabet)
{
    if (alphabet.length() < 2 || alphabet.length() > 36)
        return false;
    for (size_t i = 0; i < alphabet.length(); ++i) {
        auto character = alphabet[i];
        if (!is_ascii_lower_alpha(character) && !is_ascii_digit(character))
            return false;
        if (alphabet.substring_view(0, i).contains(character))
            return false;
    }
    return true;
}

Optional<u64> hint_session_id(JsonValue const& id)
{
    static constexpr double maximum_id = 9007199254740992.0;
    if (!id.is_number())
        return {};
    auto number = id.get_double_with_precision_loss().value_or(AK::NaN<double>);
    if (!(number >= 0) || number >= maximum_id || number != AK::trunc(number))
        return {};
    return static_cast<u64>(number);
}

size_t hint_label_length(size_t alphabet_length, size_t count)
{
    VERIFY(alphabet_length >= 2);
    size_t length = 1;
    for (size_t capacity = alphabet_length; capacity < count; capacity *= alphabet_length)
        ++length;
    return length;
}

bool is_valid_hint_prefix(StringView prefix, StringView alphabet, size_t label_length)
{
    if (prefix.length() > label_length)
        return false;
    for (auto character : prefix) {
        if (!alphabet.contains(character))
            return false;
    }
    return true;
}

Vector<String> hint_labels(StringView alphabet, size_t count)
{
    VERIFY(is_valid_hint_alphabet(alphabet));

    Vector<String> labels;
    if (count == 0)
        return labels;

    auto length = hint_label_length(alphabet.length(), count);

    labels.ensure_capacity(count);
    Vector<char, 16> characters;
    characters.resize(length);
    for (size_t index = 0; index < count; ++index) {
        auto rest = index;
        for (size_t position = length; position > 0; --position) {
            characters[position - 1] = alphabet[rest % alphabet.length()];
            rest /= alphabet.length();
        }
        labels.unchecked_append(MUST(String::from_utf8(StringView { characters.data(), characters.size() })));
    }
    return labels;
}

String hint_label_mask(Vector<String> const& labels, StringView prefix)
{
    StringBuilder mask;
    for (auto const& label : labels)
        mask.append(label.starts_with_bytes(prefix) ? '1' : '0');
    return mask.to_string_without_validation();
}

Optional<size_t> hint_activation_index(JsonValue const& activate, size_t count)
{
    if (!activate.is_number())
        return {};
    auto index = activate.get_double_with_precision_loss().value_or(AK::NaN<double>);
    if (!(index >= 0) || index >= static_cast<double>(count) || index != AK::trunc(index))
        return {};
    return static_cast<size_t>(index);
}

StringView hint_kind_name(HintKind kind)
{
    switch (kind) {
    case HintKind::Link:
        return "link"sv;
    case HintKind::Button:
        return "button"sv;
    case HintKind::Input:
        return "input"sv;
    case HintKind::Other:
        return "other"sv;
    }
    VERIFY_NOT_REACHED();
}

static String hint_text(StringView text)
{
    // NB: JsonParser writes an escaped lone surrogate as WTF-8.
    auto valid = String::from_wtf8_with_replacement_character(text);
    Utf8View view { valid };
    if (view.length() <= maximum_hint_text_length)
        return valid;
    return MUST(String::from_utf8(valid.bytes_as_string_view().substring_view(0, view.byte_offset_of(maximum_hint_text_length))));
}

static double hint_coordinate(JsonObject const& object, StringView key)
{
    // NB: A page can report any number; one beyond what a viewport could be is as useless as none.
    static constexpr double maximum_coordinate = 1e9;
    auto value = object.get(key);
    if (!value.has_value() || !value->is_number())
        return 0;
    auto number = value->get_double_with_precision_loss().value_or(0);
    if (!__builtin_isfinite(number))
        return 0;
    return clamp(number, -maximum_coordinate, maximum_coordinate);
}

ErrorOr<HintCollection> parse_hint_collection(StringView json)
{
    auto parsed = TRY(JsonValue::from_string(json));
    if (!parsed.is_object())
        return Error::from_string_literal("The hint collection is not an object");
    auto items = parsed.as_object().get_array("items"sv);
    if (!items.has_value())
        return Error::from_string_literal("The hint collection has no items");

    HintCollection collection;
    collection.truncated = items->size() > maximum_hint_count;
    collection.items.ensure_capacity(min(items->size(), maximum_hint_count));
    for (size_t index = 0; index < items->size() && index < maximum_hint_count; ++index) {
        HintItem item;
        if (auto const& value = items->at(index); value.is_object()) {
            auto const& object = value.as_object();
            auto kind = object.get_string("kind"sv).value_or({});
            if (kind == "link"sv)
                item.kind = HintKind::Link;
            else if (kind == "button"sv)
                item.kind = HintKind::Button;
            else if (kind == "input"sv)
                item.kind = HintKind::Input;

            if (auto text = object.get_string("text"sv); text.has_value())
                item.text = hint_text(*text);

            // NB: The URL is parsed again, so that what goes to the client is an absolute URL whatever the page wrote.
            if (auto url = object.get_string("url"sv); item.kind == HintKind::Link && url.has_value() && url->byte_count() <= maximum_hint_url_length) {
                if (auto parsed_url = URL::Parser::basic_parse(*url); parsed_url.has_value()) {
                    auto serialized = parsed_url->serialize();
                    if (serialized.byte_count() <= maximum_hint_url_length)
                        item.url = move(serialized);
                }
            }

            item.x = hint_coordinate(object, "x"sv);
            item.y = hint_coordinate(object, "y"sv);
            item.width = hint_coordinate(object, "width"sv);
            item.height = hint_coordinate(object, "height"sv);
        }
        collection.items.unchecked_append(move(item));
    }
    return collection;
}

}
