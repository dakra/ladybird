/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Span.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibWebView/Export.h>

namespace WebView {

// A POSIX shared memory object that a client on this machine maps by name to read whole frames from. It is never
// resized; a larger frame gets a new segment.
class WEBVIEW_API EmacsSharedFrameBuffer {
    AK_MAKE_NONCOPYABLE(EmacsSharedFrameBuffer);
    AK_MAKE_NONMOVABLE(EmacsSharedFrameBuffer);

public:
    AK_ALLOC_WITH_KMALLOC;

    // The segment has room for a frame somewhat larger than the one asked for.
    static ErrorOr<NonnullOwnPtr<EmacsSharedFrameBuffer>> create(size_t frame_size_in_bytes);
    ~EmacsSharedFrameBuffer();

    ByteString const& name() const { return m_name; }
    size_t size() const { return m_size; }
    Bytes bytes() { return { m_data, m_size }; }

private:
    EmacsSharedFrameBuffer() = default;

    ByteString m_name;
    u8* m_data { nullptr };
    size_t m_size { 0 };
};

// Turns rectangles of a presented frame into the pixel payloads of frame records: BGRA8888 rows, tightly packed.
class WEBVIEW_API EmacsFrameEncoder {
    AK_MAKE_NONCOPYABLE(EmacsFrameEncoder);
    AK_MAKE_NONMOVABLE(EmacsFrameEncoder);

public:
    EmacsFrameEncoder() = default;
    ~EmacsFrameEncoder();

    // Copies the rectangle into rows that are row_stride bytes apart.
    static void copy_pixels(Gfx::Bitmap const&, Gfx::IntRect, Bytes destination, size_t row_stride);

    ErrorOr<void> append_raw(ByteBuffer&, Gfx::Bitmap const&, Gfx::IntRect);
    // Appends one zstd frame that states the size of the pixels it holds, unless that would be no smaller than the
    // pixels are. Returns whether it did.
    ErrorOr<bool> append_zstd(ByteBuffer&, Gfx::Bitmap const&, Gfx::IntRect);

    // Frees the pixels kept for the next frame to compress.
    void release_scratch() { m_scratch.clear(); }

private:
    void* m_compression_context { nullptr };
    ByteBuffer m_scratch;
};

}
