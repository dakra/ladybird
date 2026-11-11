/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
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

static HintAnswer parse_answer(StringView json)
{
    return MUST(parse_hint_answer(MUST(JsonValue::from_string(json))));
}

TEST_CASE(hint_navigable_names)
{
    auto id = hint_navigable_from_name("2:17"sv);
    VERIFY(id.has_value());
    EXPECT_EQ(id->namespace_id, 2u);
    EXPECT_EQ(id->local_id, 17u);
    EXPECT(hint_navigable_from_name("18446744073709551615:1"sv).has_value());
    EXPECT(!hint_navigable_from_name(""sv).has_value());
    EXPECT(!hint_navigable_from_name("2"sv).has_value());
    EXPECT(!hint_navigable_from_name("2:"sv).has_value());
    EXPECT(!hint_navigable_from_name("2:3:4"sv).has_value());
    EXPECT(!hint_navigable_from_name(" 2:3"sv).has_value());
    EXPECT(!hint_navigable_from_name("-2:3"sv).has_value());
    EXPECT(!hint_navigable_from_name("18446744073709551616:1"sv).has_value());
}

TEST_CASE(hint_answers_are_checked)
{
    EXPECT(parse_hint_answer(JsonValue {}).is_error());
    EXPECT(parse_hint_answer(JsonArray {}).is_error());
    EXPECT(parse_hint_answer(MUST(JsonValue::from_string("{\"roots\":[]}"sv))).is_error());
    EXPECT(parse_hint_answer(MUST(JsonValue::from_string("{\"roots\":7,\"frames\":[]}"sv))).is_error());

    auto empty = parse_answer("{\"roots\":[],\"frames\":[]}"sv);
    EXPECT(empty.roots.is_empty());
    EXPECT(empty.frames.is_empty());

    auto answer = parse_answer(R"~({"roots":[
        {"navigable":null,"frame":null,"items":[
            {"node":5,"kind":"link","text":"Home","url":"https://example.org/a/../b","x":1.5,"y":2,"width":30,"height":10},
            {"node":6,"kind":"button","text":"OK","url":"https://example.org/","x":"1","y":null,"width":1e400,"height":-1},
            {"node":"7","kind":"button"},
            {"node":8,"kind":"input","text":7},
            {"node":9,"kind":"evil","text":"x"},
            {"node":10,"kind":"link","url":"not a url"},
            {"node":11,"kind":"link","url":"javascript:alert(1)"},
            "junk"
        ]},
        {"navigable":"3:4","frame":"3:5","items":[{"node":1,"kind":"other"}]},
        {"navigable":"bad","items":[{"node":2}]},
        {"navigable":"3:4","frame":7,"items":[{"node":3}]},
        "junk"
    ],"frames":[{"navigable":"3:4","exposed":true},7,{"navigable":"3:6"},{"navigable":"x","exposed":true}]})~"sv);

    VERIFY(answer.roots.size() == 3);
    auto const& top = answer.roots[0];
    EXPECT(!top.navigable.has_value());
    EXPECT(!top.frame.has_value());
    VERIFY(top.items.size() == 6);

    auto const& link = top.items[0];
    EXPECT_EQ(link.node, 5);
    EXPECT(link.kind == HintKind::Link);
    EXPECT_EQ(link.text, "Home"_string);
    EXPECT_EQ(link.url, "https://example.org/b"_string);
    EXPECT_EQ(link.x, 1.5);
    EXPECT_EQ(link.y, 2.0);
    EXPECT_EQ(link.width, 30.0);
    EXPECT_EQ(link.height, 10.0);

    auto const& button = top.items[1];
    EXPECT(button.kind == HintKind::Button);
    EXPECT(!button.url.has_value());
    EXPECT_EQ(button.x, 0.0);
    EXPECT_EQ(button.y, 0.0);
    EXPECT_EQ(button.height, -1.0);

    EXPECT_EQ(top.items[2].node, 8);
    EXPECT(top.items[2].kind == HintKind::Input);
    EXPECT(top.items[2].text.is_empty());
    EXPECT(top.items[3].kind == HintKind::Other);
    EXPECT(!top.items[4].url.has_value());
    EXPECT_EQ(top.items[5].url, "javascript:alert(1)"_string);
    EXPECT_EQ(hint_kind_name(top.items[3].kind), "other"sv);

    auto const& frame_root = answer.roots[1];
    EXPECT_EQ(frame_root.navigable->local_id, 4u);
    EXPECT_EQ(frame_root.frame->local_id, 5u);
    // A frame name that is no name is the top-level document's, which only the page displaying the tab may list.
    EXPECT(!answer.roots[2].frame.has_value());

    VERIFY(answer.frames.size() == 2);
    EXPECT(answer.frames[0].exposed);
    EXPECT_EQ(answer.frames[1].navigable.local_id, 6u);
    EXPECT(!answer.frames[1].exposed);
}

TEST_CASE(hint_answers_are_bounded)
{
    StringBuilder json;
    json.append("{\"frames\":[],\"roots\":[{\"items\":["sv);
    for (size_t i = 0; i < maximum_hint_count + 5; ++i) {
        if (i != 0)
            json.append(',');
        json.appendff("{{\"node\":{},\"kind\":\"link\",\"text\":\"{}\",\"url\":\"https://example.org/{}\",\"x\":1e300}}", i, MUST(String::repeated('t', 300)), MUST(String::repeated('p', i == 0 ? maximum_hint_url_length : 10)));
    }
    json.append("]},{\"items\":[{\"node\":1}]}]}"sv);

    auto answer = parse_answer(json.string_view());
    VERIFY(answer.roots.size() == 2);
    EXPECT_EQ(answer.roots[0].items.size(), maximum_hint_count + 1);
    EXPECT(answer.roots[1].items.is_empty());
    EXPECT_EQ(answer.roots[0].items[0].text.bytes().size(), maximum_hint_text_length);
    EXPECT(!answer.roots[0].items[0].url.has_value());
    EXPECT(answer.roots[0].items[1].url.has_value());
    EXPECT_EQ(answer.roots[0].items[1].x, 1e9);

    // Text is cut at a code point, and a lone surrogate becomes a replacement character.
    StringBuilder text;
    for (size_t i = 0; i < maximum_hint_text_length + 3; ++i)
        text.append("\xc3\xa9"sv);
    auto cut = parse_answer(MUST(String::formatted("{{\"frames\":[],\"roots\":[{{\"items\":[{{\"node\":1,\"text\":\"{}\"}},{{\"node\":2,\"text\":\"a\\ud800b\"}}]}}]}}", text.string_view())));
    EXPECT_EQ(cut.roots[0].items[0].text.bytes().size(), maximum_hint_text_length * 2);
    EXPECT_EQ(cut.roots[0].items[1].text, "a�b"_string);
}

static Web::HTML::CrossProcessId navigable_id(u64 local_id)
{
    return { .namespace_id = 1, .local_id = local_id };
}

static HintItem hint_item(i64 node, double x, double y, double width = 10, double height = 10)
{
    HintItem item;
    item.node = node;
    item.x = x;
    item.y = y;
    item.width = width;
    item.height = height;
    return item;
}

// The top-level document (host 0) holds frame A of another site (host 1), which holds frame B of its own site and frame
// D of the top-level document's site (host 0 again); the top-level document also holds frame C (host 2).
static Vector<HintNavigable> example_navigables()
{
    return {
        { navigable_id(10), {}, 0, false, {}, {} },
        { navigable_id(11), 0, 1, true, { 100, 50 }, { 0, 0, 200, 100 } },
        { navigable_id(12), 1, 1, false, { 100, 50 }, {} },
        { navigable_id(14), 1, 0, true, { 120, 70 }, { 0, 0, 50, 50 } },
        { navigable_id(13), 0, 2, true, { 400, 0 }, { 0, 0, 50, 50 } },
    };
}

TEST_CASE(hint_answers_are_merged_in_tree_order)
{
    HintAnswer top_level;
    top_level.roots.append({ {}, {}, { hint_item(1, 0, 0) } });
    top_level.roots.append({ navigable_id(14), navigable_id(14), { hint_item(4, 10, 10) } });
    top_level.frames.append({ navigable_id(11), true });
    top_level.frames.append({ navigable_id(13), true });

    HintAnswer frame;
    frame.roots.append({ navigable_id(11), navigable_id(12), { hint_item(3, 20, 20) } });
    frame.roots.append({ navigable_id(11), navigable_id(11), { hint_item(2, 190, 0, 20, 20), hint_item(5, 300, 0) } });
    frame.frames.append({ navigable_id(14), true });

    auto list = merge_hint_answers({ { 0, top_level }, { 1, frame } }, example_navigables(), { 800, 600 }, 2);
    VERIFY(list.items.size() == 4);
    EXPECT(!list.truncated);
    EXPECT_EQ(list.items[0].item.node, 1);
    EXPECT_EQ(list.items[0].host, 0u);
    // In the frame's visible part, moved by its offset, in CSS pixels.
    EXPECT_EQ(list.items[1].item.node, 2);
    EXPECT_EQ(list.items[1].host, 1u);
    EXPECT_EQ(list.items[1].item.x, 145.0);
    EXPECT_EQ(list.items[1].item.y, 25.0);
    EXPECT_EQ(list.items[1].item.width, 5.0);
    EXPECT_EQ(list.items[1].item.height, 10.0);
    EXPECT_EQ(list.items[2].item.node, 3);
    EXPECT_EQ(list.items[3].item.node, 4);
    EXPECT_EQ(list.items[3].host, 0u);
    EXPECT_EQ(list.items[3].item.x, 65.0);
}

TEST_CASE(hint_answers_speak_only_for_what_their_process_hosts)
{
    HintAnswer top_level;
    top_level.roots.append({ {}, {}, { hint_item(1, 0, 0) } });
    // Frame D's parent is in host 1, which alone says whether D shows; and the items of frame C are host 2's to report.
    top_level.roots.append({ navigable_id(14), navigable_id(14), { hint_item(4, 10, 10) } });
    top_level.roots.append({ navigable_id(13), navigable_id(13), { hint_item(6, 0, 0) } });
    top_level.frames.append({ navigable_id(11), true });
    top_level.frames.append({ navigable_id(13), true });
    top_level.frames.append({ navigable_id(14), true });

    HintAnswer frame;
    // A document of host 1 named under a local root that does not hold it, and the top-level document.
    frame.roots.append({ navigable_id(14), navigable_id(12), { hint_item(7, 0, 0) } });
    frame.roots.append({ {}, {}, { hint_item(8, 0, 0) } });
    frame.roots.append({ navigable_id(11), navigable_id(11), { hint_item(2, 0, 0) } });
    frame.frames.append({ navigable_id(14), false });
    frame.truncated = true;

    auto list = merge_hint_answers({ { 0, top_level }, { 1, frame } }, example_navigables(), { 800, 600 }, 1);
    VERIFY(list.items.size() == 2);
    EXPECT_EQ(list.items[0].item.node, 1);
    EXPECT_EQ(list.items[1].item.node, 2);
    EXPECT(list.truncated);

    // A frame whose parent's process did not say it shows lists nothing, nor does anything in it.
    HintAnswer silent_parent;
    silent_parent.roots.append({ {}, {}, { hint_item(1, 0, 0) } });
    silent_parent.roots.append({ navigable_id(14), navigable_id(14), { hint_item(4, 10, 10) } });
    auto without_frames = merge_hint_answers({ { 0, silent_parent } }, example_navigables(), { 800, 600 }, 1);
    VERIFY(without_frames.items.size() == 1);
    EXPECT_EQ(without_frames.items[0].item.node, 1);
}

TEST_CASE(hint_answers_are_cut_to_the_viewport_and_the_limit)
{
    HintAnswer top_level;
    Vector<HintItem> items;
    items.append(hint_item(1, -20, -20, 10, 10));
    items.append(hint_item(2, 795, 595, 10, 10));
    for (size_t i = 0; i < maximum_hint_count + 2; ++i)
        items.append(hint_item(100 + i, 0, 0));
    top_level.roots.append({ {}, {}, move(items) });
    auto list = merge_hint_answers({ { 0, top_level } }, example_navigables(), { 800, 600 }, 1);
    EXPECT_EQ(list.items.size(), maximum_hint_count);
    EXPECT(list.truncated);
    EXPECT_EQ(list.items[0].item.node, 2);
    EXPECT_EQ(list.items[0].item.width, 5.0);
}
