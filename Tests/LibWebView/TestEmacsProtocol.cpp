/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <AK/HashTable.h>
#include <AK/Math.h>
#include <AK/StringBuilder.h>
#include <LibTest/TestCase.h>
#include <LibWebView/EmacsProtocol.h>

using namespace WebView::EmacsProtocol;
using Web::UIEvents::KeyCode;
using Web::UIEvents::KeyModifier;
using Web::UIEvents::MouseButton;

static ByteBuffer record_with_length(u32 length, u8 kind, ReadonlyBytes payload = {})
{
    ByteBuffer record;
    LittleEndian<u32> little_endian_length { length };
    record.append(&little_endian_length, sizeof(little_endian_length));
    record.append(kind);
    record.append(payload);
    return record;
}

static void feed(RecordReader& reader, ByteBuffer const& bytes)
{
    reader.feed(bytes.bytes());
}

TEST_CASE(record_length_counts_the_kind_byte)
{
    JsonObject message;
    message.set("type"sv, "ping"sv);
    message.set("id"sv, 1);

    auto record = MUST(encode_control_record(message));
    auto expected = "\x17\x00\x00\x00\x01{\"type\":\"ping\",\"id\":1}"sv;
    EXPECT_EQ(StringView { record.bytes() }, expected);
}

TEST_CASE(record_length_is_set_once_the_payload_is_whole)
{
    ByteBuffer record;
    begin_record(record, RecordKind::Frame);
    record.append("abcdef"sv.bytes());
    MUST(finish_record(record));

    ByteBuffer expected;
    MUST(append_record_header(expected, RecordKind::Frame, 6));
    expected.append("abcdef"sv.bytes());
    EXPECT_EQ(record, expected);

    RecordReader reader;
    feed(reader, record);
    auto read = reader.next();
    VERIFY(read.has_value());
    EXPECT(read->kind == RecordKind::Frame);
    EXPECT_EQ(StringView { read->payload.bytes() }, "abcdef"sv);
}

TEST_CASE(a_payload_too_long_for_a_record_is_refused)
{
    ByteBuffer header;
    EXPECT(!append_record_header(header, RecordKind::Control, maximum_record_length - 1).is_error());
    EXPECT(append_record_header(header, RecordKind::Control, maximum_record_length).is_error());

    ByteBuffer record;
    begin_record(record, RecordKind::Frame);
    record.resize(record_header_size + maximum_record_length);
    EXPECT(finish_record(record).is_error());
    record.resize(record_header_size + maximum_record_length - 1);
    EXPECT(!finish_record(record).is_error());

    JsonObject message;
    message.set("type"sv, "state"sv);
    message.set("title"sv, MUST(String::repeated('a', maximum_record_length)));
    EXPECT(encode_control_record(message).is_error());
}

TEST_CASE(records_round_trip_across_feed_boundaries)
{
    ByteBuffer stream;
    Vector<Record> expected;
    for (size_t i = 0; i < 20; ++i) {
        Record record;
        record.kind = i % 3 == 0 ? RecordKind::Frame : RecordKind::Control;
        for (size_t j = 0; j < i * 7; ++j)
            record.payload.append(static_cast<u8>(i * 31 + j));
        MUST(append_record_header(stream, record.kind, record.payload.size()));
        stream.append(record.payload);
        expected.append(move(record));
    }

    for (size_t chunk_size : { 1uz, 2uz, 3uz, 4uz, 5uz, 6uz, 7uz, 13uz, 64uz, 4096uz }) {
        RecordReader reader;
        size_t record_index = 0;
        for (size_t offset = 0; offset < stream.size(); offset += chunk_size) {
            reader.feed(stream.bytes().slice(offset, min(chunk_size, stream.size() - offset)));
            for (auto record = reader.next(); record.has_value(); record = reader.next()) {
                VERIFY(record_index < expected.size());
                EXPECT(record->kind == expected[record_index].kind);
                EXPECT_EQ(record->payload, expected[record_index].payload);
                ++record_index;
            }
        }
        EXPECT(!reader.has_error());
        EXPECT_EQ(record_index, expected.size());
    }
}

TEST_CASE(reader_rejects_bad_records)
{
    {
        RecordReader reader;
        feed(reader, record_with_length(0, 1));
        EXPECT(!reader.next().has_value());
        EXPECT(reader.has_error());
    }
    {
        RecordReader reader;
        feed(reader, record_with_length(maximum_record_length + 1, 1));
        EXPECT(!reader.next().has_value());
        EXPECT(reader.has_error());
    }
    {
        RecordReader reader;
        feed(reader, record_with_length(4, 3, "abc"sv.bytes()));
        EXPECT(!reader.next().has_value());
        EXPECT(reader.has_error());
    }
    {
        RecordReader reader;
        feed(reader, record_with_length(1, 1));
        feed(reader, record_with_length(0, 1));
        auto record = reader.next();
        VERIFY(record.has_value());
        EXPECT(record->payload.is_empty());
        EXPECT(!reader.has_error());
        EXPECT(!reader.next().has_value());
        EXPECT(reader.has_error());

        feed(reader, record_with_length(1, 1));
        EXPECT(!reader.next().has_value());
    }
}

TEST_CASE(frame_header_layout)
{
    ByteBuffer buffer;
    append_frame_header(buffer, { .seq = 0x01020304, .view = 7, .width = 1600, .height = 1000, .dpr = 2.0, .damage_x = 1, .damage_y = 2, .damage_width = 3, .damage_height = 4, .encoding = FrameEncoding::Zstd, .flags = frame_flag_full | frame_flag_more });
    EXPECT_EQ(buffer.size(), frame_header_size);

    u8 const expected[] = {
        0x04, 0x03, 0x02, 0x01, 7, 0, 0, 0, 0x40, 0x06, 0, 0, 0xe8, 0x03, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0x40,
        1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0,
        1, 3, 0, 0
    };
    EXPECT_EQ(buffer.bytes(), ReadonlyBytes { expected });
}

TEST_CASE(key_names)
{
    EXPECT(key_code_from_name("Return"sv) == KeyCode::Key_Return);
    EXPECT(key_code_from_name("A"sv) == KeyCode::Key_A);
    EXPECT(key_code_from_name("ExclamationPoint"sv) == KeyCode::Key_ExclamationPoint);
    EXPECT(!key_code_from_name("Bogus"sv).has_value());
    EXPECT(!key_code_from_name("!"sv).has_value());
    EXPECT(!key_code_from_name("return"sv).has_value());
}

TEST_CASE(modifiers)
{
    EXPECT(modifiers_from_protocol(0) == KeyModifier::Mod_None);
    EXPECT(modifiers_from_protocol(1) == KeyModifier::Mod_Shift);
    EXPECT(modifiers_from_protocol(2) == KeyModifier::Mod_Ctrl);
    EXPECT(modifiers_from_protocol(4) == KeyModifier::Mod_Alt);
    EXPECT(modifiers_from_protocol(8) == KeyModifier::Mod_Super);
    EXPECT(modifiers_from_protocol(16) == KeyModifier::Mod_Keypad);
    EXPECT(modifiers_from_protocol(1 | 2 | 4 | 8) == (KeyModifier::Mod_Shift | KeyModifier::Mod_Ctrl | KeyModifier::Mod_Alt | KeyModifier::Mod_Super));
    EXPECT(modifiers_from_protocol(32) == KeyModifier::Mod_None);
}

TEST_CASE(mouse_buttons_and_cursors)
{
    EXPECT(mouse_button_from_name("left"sv) == MouseButton::Primary);
    EXPECT(mouse_button_from_name("right"sv) == MouseButton::Secondary);
    EXPECT(mouse_button_from_name("middle"sv) == MouseButton::Middle);
    EXPECT(mouse_button_from_name("back"sv) == MouseButton::Backward);
    EXPECT(mouse_button_from_name("forward"sv) == MouseButton::Forward);
    EXPECT(!mouse_button_from_name("primary"sv).has_value());

    EXPECT_EQ(css_cursor_name(Gfx::StandardCursor::Hand), "pointer"sv);
    EXPECT_EQ(css_cursor_name(Gfx::StandardCursor::IBeam), "text"sv);
    EXPECT_EQ(css_cursor_name(Gfx::StandardCursor::Arrow), "default"sv);
    EXPECT_EQ(css_cursor_name(Gfx::ImageCursor {}), "default"sv);
}

TEST_CASE(hint_alphabets)
{
    EXPECT(is_valid_hint_alphabet("ab"sv));
    EXPECT(is_valid_hint_alphabet("asdfghjkl"sv));
    EXPECT(is_valid_hint_alphabet("abcdefghijklmnopqrstuvwxyz0123456789"sv));
    EXPECT(!is_valid_hint_alphabet(""sv));
    EXPECT(!is_valid_hint_alphabet("a"sv));
    EXPECT(!is_valid_hint_alphabet("aa"sv));
    EXPECT(!is_valid_hint_alphabet("abca"sv));
    EXPECT(!is_valid_hint_alphabet("aB"sv));
    EXPECT(!is_valid_hint_alphabet("a-"sv));
    EXPECT(!is_valid_hint_alphabet("a\""sv));
    EXPECT(!is_valid_hint_alphabet("a\xc3\xa9"sv));
    EXPECT(!is_valid_hint_alphabet("abcdefghijklmnopqrstuvwxyz0123456789a"sv));
}

TEST_CASE(hint_session_ids)
{
    EXPECT_EQ(hint_session_id(0), 0u);
    EXPECT_EQ(hint_session_id(7), 7u);
    EXPECT_EQ(hint_session_id(7.0), 7u);
    EXPECT_EQ(hint_session_id(9007199254740991.0), 9007199254740991u);
    EXPECT(!hint_session_id(9007199254740992.0).has_value());
    EXPECT(!hint_session_id(NumericLimits<u64>::max()).has_value());
    EXPECT(!hint_session_id(-1).has_value());
    EXPECT(!hint_session_id(1.5).has_value());
    EXPECT(!hint_session_id(AK::NaN<double>).has_value());
    EXPECT(!hint_session_id("1"_string).has_value());
    EXPECT(!hint_session_id(JsonValue {}).has_value());
    EXPECT(!hint_session_id(true).has_value());
    EXPECT(!hint_session_id(JsonObject {}).has_value());
}

TEST_CASE(hint_labels_have_the_shortest_common_length)
{
    EXPECT(hint_labels("ab"sv, 0).is_empty());
    EXPECT_EQ(hint_labels("ab"sv, 1), (Vector<String> { "a"_string }));
    EXPECT_EQ(hint_labels("ab"sv, 2), (Vector<String> { "a"_string, "b"_string }));
    EXPECT_EQ(hint_labels("ab"sv, 3), (Vector<String> { "aa"_string, "ab"_string, "ba"_string }));
    EXPECT_EQ(hint_labels("asd"sv, 4), (Vector<String> { "aa"_string, "as"_string, "ad"_string, "sa"_string }));

    for (auto alphabet : { "ab"sv, "asdf"sv, "0123456789"sv, "abcdefghijklmnopqrstuvwxyz0123456789"sv }) {
        for (size_t count : { 1uz, 2uz, 9uz, 10uz, 11uz, 100uz, 999uz, 1000uz }) {
            auto labels = hint_labels(alphabet, count);
            EXPECT_EQ(labels.size(), count);

            size_t length = 1;
            for (size_t capacity = alphabet.length(); capacity < count; capacity *= alphabet.length())
                ++length;

            HashTable<String> distinct;
            for (auto const& label : labels) {
                EXPECT_EQ(label.bytes().size(), length);
                for (auto character : label.bytes())
                    EXPECT(alphabet.contains(static_cast<char>(character)));
                distinct.set(label);
            }
            EXPECT_EQ(distinct.size(), count);
        }
    }
    EXPECT_EQ(hint_labels("ab"sv, 1000)[999], "bbbbbaabbb"_string);
}

TEST_CASE(hint_prefixes)
{
    EXPECT_EQ(hint_label_length(2, 1), 1u);
    EXPECT_EQ(hint_label_length(2, 2), 1u);
    EXPECT_EQ(hint_label_length(2, 3), 2u);
    EXPECT_EQ(hint_label_length(2, 1000), 10u);
    EXPECT_EQ(hint_label_length(36, 1000), 2u);

    EXPECT(is_valid_hint_prefix(""sv, "asdf"sv, 2));
    EXPECT(is_valid_hint_prefix("a"sv, "asdf"sv, 2));
    EXPECT(is_valid_hint_prefix("df"sv, "asdf"sv, 2));
    EXPECT(!is_valid_hint_prefix("aaa"sv, "asdf"sv, 2));
    EXPECT(!is_valid_hint_prefix("q"sv, "asdf"sv, 2));
    EXPECT(!is_valid_hint_prefix("A"sv, "asdf"sv, 2));
    EXPECT(!is_valid_hint_prefix("\""sv, "asdf"sv, 2));
}

TEST_CASE(hint_label_masks)
{
    auto labels = hint_labels("asd"sv, 5);
    EXPECT_EQ(hint_label_mask(labels, ""sv), "11111"_string);
    EXPECT_EQ(hint_label_mask(labels, "a"sv), "11100"_string);
    EXPECT_EQ(hint_label_mask(labels, "sa"sv), "00010"_string);
    EXPECT_EQ(hint_label_mask(labels, "x"sv), "00000"_string);
    EXPECT_EQ(hint_label_mask(labels, "saa"sv), "00000"_string);
    EXPECT_EQ(hint_label_mask(labels, "\"); alert(1); (\""sv), "00000"_string);
}

TEST_CASE(hint_activation_indices)
{
    EXPECT_EQ(hint_activation_index(0, 3), 0u);
    EXPECT_EQ(hint_activation_index(2, 3), 2u);
    EXPECT_EQ(hint_activation_index(2.0, 3), 2u);
    EXPECT(!hint_activation_index(3, 3).has_value());
    EXPECT(!hint_activation_index(-1, 3).has_value());
    EXPECT(!hint_activation_index(0.5, 3).has_value());
    EXPECT(!hint_activation_index(AK::NaN<double>, 3).has_value());
    EXPECT(!hint_activation_index(1e300, 3).has_value());
    EXPECT(!hint_activation_index("0"_string, 3).has_value());
    EXPECT(!hint_activation_index(JsonValue {}, 3).has_value());
    EXPECT(!hint_activation_index(0, 0).has_value());
}

TEST_CASE(hint_collections_are_checked)
{
    EXPECT(parse_hint_collection("null"sv).is_error());
    EXPECT(parse_hint_collection("[]"sv).is_error());
    EXPECT(parse_hint_collection("{\"items\":7}"sv).is_error());
    EXPECT(parse_hint_collection("{\"items\":["sv).is_error());

    auto empty = MUST(parse_hint_collection("{\"items\":[]}"sv));
    EXPECT(empty.items.is_empty());
    EXPECT(!empty.truncated);

    auto collection = MUST(parse_hint_collection(R"~({"items":[
        {"kind":"link","text":"Home","url":"https://example.org/a/../b","x":1.5,"y":2,"width":30,"height":10},
        {"kind":"button","text":"OK","url":"https://example.org/","x":"1","y":null,"width":1e400,"height":-1},
        {"kind":"input","text":7},
        {"kind":"evil","text":"x"},
        {"kind":"link","url":"not a url"},
        {"kind":"link","url":"javascript:alert(1)"},
        "junk"
    ]})~"sv));
    VERIFY(collection.items.size() == 7);
    EXPECT(!collection.truncated);

    auto const& link = collection.items[0];
    EXPECT(link.kind == HintKind::Link);
    EXPECT_EQ(link.text, "Home"_string);
    EXPECT_EQ(link.url, "https://example.org/b"_string);
    EXPECT_EQ(link.x, 1.5);
    EXPECT_EQ(link.y, 2.0);
    EXPECT_EQ(link.width, 30.0);
    EXPECT_EQ(link.height, 10.0);

    auto const& button = collection.items[1];
    EXPECT(button.kind == HintKind::Button);
    EXPECT(!button.url.has_value());
    EXPECT_EQ(button.x, 0.0);
    EXPECT_EQ(button.y, 0.0);
    EXPECT_EQ(button.height, -1.0);

    EXPECT(collection.items[2].kind == HintKind::Input);
    EXPECT(collection.items[2].text.is_empty());
    EXPECT(collection.items[3].kind == HintKind::Other);
    EXPECT(!collection.items[4].url.has_value());
    EXPECT_EQ(collection.items[5].url, "javascript:alert(1)"_string);
    EXPECT(collection.items[6].kind == HintKind::Other);
    EXPECT_EQ(hint_kind_name(collection.items[6].kind), "other"sv);
}

TEST_CASE(hint_collections_are_bounded)
{
    StringBuilder json;
    json.append("{\"items\":["sv);
    for (size_t i = 0; i < maximum_hint_count + 5; ++i) {
        if (i != 0)
            json.append(',');
        json.appendff("{{\"kind\":\"link\",\"text\":\"{}\",\"url\":\"https://example.org/{}\",\"x\":1e300}}", MUST(String::repeated('t', 300)), MUST(String::repeated('p', i == 0 ? maximum_hint_url_length : 10)));
    }
    json.append("]}"sv);

    auto collection = MUST(parse_hint_collection(json.string_view()));
    EXPECT_EQ(collection.items.size(), maximum_hint_count);
    EXPECT(collection.truncated);
    EXPECT_EQ(collection.items[0].text.bytes().size(), maximum_hint_text_length);
    EXPECT(!collection.items[0].url.has_value());
    EXPECT(collection.items[1].url.has_value());
    EXPECT_EQ(collection.items[1].x, 1e9);

    // Text is cut at a code point, and a lone surrogate becomes a replacement character.
    StringBuilder text;
    for (size_t i = 0; i < maximum_hint_text_length + 3; ++i)
        text.append("\xc3\xa9"sv);
    auto cut = MUST(parse_hint_collection(MUST(String::formatted("{{\"items\":[{{\"text\":\"{}\"}},{{\"text\":\"a\\ud800b\"}}]}}", text.string_view()))));
    EXPECT_EQ(cut.items[0].text.bytes().size(), maximum_hint_text_length * 2);
    EXPECT_EQ(cut.items[1].text, "a\uFFFDb"_string);
}
