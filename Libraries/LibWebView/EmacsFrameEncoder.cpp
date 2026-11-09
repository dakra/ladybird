/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWebView/EmacsFrameEncoder.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zstd.h>

namespace WebView {

static constexpr int zstd_compression_level = 1;

static u64 s_next_segment_generation = 1;

ErrorOr<NonnullOwnPtr<EmacsSharedFrameBuffer>> EmacsSharedFrameBuffer::create(size_t frame_size_in_bytes)
{
    auto page_size = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    auto size = frame_size_in_bytes + frame_size_in_bytes / 4;
    size = (size + page_size - 1) / page_size * page_size;

    // macOS limits the name to 31 bytes.
    auto name = ByteString::formatted("/ladybird-{}-{}", getpid(), s_next_segment_generation++);

    // NB: macOS rejects O_CLOEXEC here, and lets an object be sized only once.
    auto fd = ::shm_open(name.characters(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return Error::from_syscall("shm_open"sv, errno);
    (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);

    if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
        auto error = Error::from_syscall("ftruncate"sv, errno);
        ::close(fd);
        ::shm_unlink(name.characters());
        return error;
    }

    auto* data = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    auto mmap_errno = errno;
    ::close(fd);
    if (data == MAP_FAILED) {
        ::shm_unlink(name.characters());
        return Error::from_syscall("mmap"sv, mmap_errno);
    }

    auto buffer = adopt_own(*new EmacsSharedFrameBuffer);
    buffer->m_name = move(name);
    buffer->m_data = static_cast<u8*>(data);
    buffer->m_size = size;
    return buffer;
}

EmacsSharedFrameBuffer::~EmacsSharedFrameBuffer()
{
    ::munmap(m_data, m_size);
    ::shm_unlink(m_name.characters());
}

EmacsFrameEncoder::~EmacsFrameEncoder()
{
    if (m_compression_context)
        ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(m_compression_context));
}

static bool red_is_low_byte(Gfx::Bitmap const& bitmap)
{
    return bitmap.format() == Gfx::BitmapFormat::RGBA8888 || bitmap.format() == Gfx::BitmapFormat::RGBx8888;
}

void EmacsFrameEncoder::copy_pixels(Gfx::Bitmap const& bitmap, Gfx::IntRect rect, Bytes destination, size_t row_stride)
{
    VERIFY(Gfx::IntRect { {}, bitmap.size() }.contains(rect));
    auto row_size = static_cast<size_t>(rect.width()) * 4;
    VERIFY(row_stride >= row_size);
    VERIFY(rect.is_empty() || destination.size() >= row_stride * (rect.height() - 1) + row_size);

    auto swaps_red_and_blue = red_is_low_byte(bitmap);
    for (int row = 0; row < rect.height(); ++row) {
        auto const* source = bitmap.scanline_u8(rect.y() + row) + static_cast<size_t>(rect.x()) * 4;
        auto* out = destination.data() + row * row_stride;
        if (!swaps_red_and_blue) {
            __builtin_memcpy(out, source, row_size);
            continue;
        }
        for (size_t offset = 0; offset < row_size; offset += 4) {
            out[offset] = source[offset + 2];
            out[offset + 1] = source[offset + 1];
            out[offset + 2] = source[offset];
            out[offset + 3] = source[offset + 3];
        }
    }
}

ErrorOr<void> EmacsFrameEncoder::append_raw(ByteBuffer& record, Gfx::Bitmap const& bitmap, Gfx::IntRect rect)
{
    auto size = static_cast<size_t>(rect.width()) * rect.height() * 4;
    auto offset = record.size();
    TRY(record.try_resize(offset + size));
    copy_pixels(bitmap, rect, record.bytes().slice(offset), static_cast<size_t>(rect.width()) * 4);
    return {};
}

ErrorOr<bool> EmacsFrameEncoder::append_zstd(ByteBuffer& record, Gfx::Bitmap const& bitmap, Gfx::IntRect rect)
{
    if (!m_compression_context) {
        auto* context = ZSTD_createCCtx();
        if (!context)
            return Error::from_string_literal("Cannot create a zstd compression context");
        ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, zstd_compression_level);
        m_compression_context = context;
    }

    auto size = static_cast<size_t>(rect.width()) * rect.height() * 4;
    ReadonlyBytes pixels;
    // Whole rows of a frame in the wire's pixel format are compressed where they are.
    if (!red_is_low_byte(bitmap) && rect.x() == 0 && rect.width() == bitmap.width() && bitmap.pitch() == static_cast<size_t>(rect.width()) * 4) {
        pixels = { bitmap.scanline_u8(rect.y()), size };
    } else {
        TRY(m_scratch.try_resize(size));
        copy_pixels(bitmap, rect, m_scratch.bytes(), static_cast<size_t>(rect.width()) * 4);
        pixels = m_scratch.bytes();
    }

    // NB: With room for no more than the pixels take, compression fails for pixels it would not make smaller.
    auto offset = record.size();
    TRY(record.try_resize(offset + size));
    auto compressed_size = ZSTD_compress2(static_cast<ZSTD_CCtx*>(m_compression_context), record.data() + offset, size, pixels.data(), pixels.size());
    if (ZSTD_isError(compressed_size)) {
        record.resize(offset);
        return false;
    }
    record.resize(offset + compressed_size);
    return true;
}

}
