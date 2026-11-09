/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
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
