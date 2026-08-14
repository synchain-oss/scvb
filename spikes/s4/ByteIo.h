// SPDX-License-Identifier: GPL-3.0-or-later
// S4 spike:确定性 little-endian 字节读写器。仅供 spike 夹具/编解码器使用(J16:由 T19 的 PR 删除)。
//   写入端不做任何隐式 padding(容器/节的 4 字节对齐由调用方显式处理);读取端严格做
//   长度/边界校验再返回(CLAUDE.md §7.3:setStateInformation 处理不可信字节,范围字段先校验再索引)。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace scvb::s4
{

class ByteWriter
{
public:
    void u8(std::uint8_t v) { buf_.push_back(v); }

    void u16(std::uint16_t v)
    {
        buf_.push_back(static_cast<std::uint8_t>(v & 0xffu));
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    }

    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }

    void u64(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    }

    void i16(std::int16_t v)
    {
        std::uint16_t u = 0;
        std::memcpy(&u, &v, sizeof(u));
        u16(u);
    }

    void i64(std::int64_t v)
    {
        std::uint64_t u = 0;
        std::memcpy(&u, &v, sizeof(u));
        u64(u);
    }

    void f32(float v)
    {
        std::uint32_t u = 0;
        std::memcpy(&u, &v, sizeof(u));
        u32(u);
    }

    void bytes(const void* p, std::size_t n)
    {
        const auto* b = static_cast<const std::uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }

    void align4()
    {
        while (buf_.size() % 4u != 0u)
            buf_.push_back(0);
    }

    const std::vector<std::uint8_t>& data() const { return buf_; }
    std::size_t size() const { return buf_.size(); }

private:
    std::vector<std::uint8_t> buf_;
};

class ByteReader
{
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size), pos_(0), ok_(true) {}

    bool ok() const { return ok_; }
    std::size_t remaining() const { return ok_ ? size_ - pos_ : 0u; }

    std::uint8_t u8() { return static_cast<std::uint8_t>(read<1>()); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(read<2>()); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(read<4>()); }
    std::uint64_t u64() { return read<8>(); }

    std::int16_t i16()
    {
        const std::uint64_t u = read<2>();
        std::int16_t v = 0;
        std::memcpy(&v, &u, 2);
        return v;
    }

    std::int64_t i64()
    {
        const std::uint64_t u = read<8>();
        std::int64_t v = 0;
        std::memcpy(&v, &u, 8);
        return v;
    }

    float f32()
    {
        const std::uint32_t u = u32();
        float f = 0.0f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    }

    void skip(std::size_t n)
    {
        if (n > remaining())
        {
            ok_ = false;
            return;
        }
        pos_ += n;
    }

    const std::uint8_t* ptr() const { return data_ + pos_; }

private:
    template<int N>
    std::uint64_t read()
    {
        if (remaining() < static_cast<std::size_t>(N))
        {
            ok_ = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < N; ++i)
            v |= static_cast<std::uint64_t>(data_[pos_++]) << (8 * i);
        return v;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_;
    bool ok_;
};

} // namespace scvb::s4
