// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/StateCodec.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

// T19:TLV 容器 + CRVS 节编解码。little-endian,确定性(roundtrip 逐字节一致)。
namespace scvb::state
{
namespace
{

// ---- 确定性 little-endian 字节读写器。写入端不做隐式 padding(4 字节对齐由调用方显式处理);
//      读取端严格做长度/边界校验再返回(不可信字节,CLAUDE.md §7.3)。----
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

    void i32(std::int32_t v)
    {
        std::uint32_t u = 0;
        std::memcpy(&u, &v, sizeof(u));
        u32(u);
    }

    void i64(std::int64_t v)
    {
        std::uint64_t u = 0;
        std::memcpy(&u, &v, sizeof(u));
        for (int i = 0; i < 8; ++i)
            buf_.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xffu));
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

    std::int32_t i32()
    {
        const std::uint32_t u = u32();
        std::int32_t v = 0;
        std::memcpy(&v, &u, sizeof(v));
        return v;
    }

    std::int64_t i64()
    {
        const std::uint64_t u = read<8>();
        std::int64_t v = 0;
        std::memcpy(&v, &u, sizeof(v));
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

} // namespace

SegmentOrigin segmentOrigin(std::uint32_t flags) noexcept
{
    const auto raw = flags & kSegmentOriginMask;
    return raw <= static_cast<std::uint32_t>(SegmentOrigin::UserCreated) ? static_cast<SegmentOrigin>(raw)
                                                                         : SegmentOrigin::Auto;
}

bool segmentLocked(std::uint32_t flags) noexcept
{
    return (flags & kSegmentLockedBit) != 0u;
}

std::uint32_t makeSegmentFlags(SegmentOrigin origin, bool locked) noexcept
{
    std::uint32_t flags = static_cast<std::uint32_t>(origin) & kSegmentOriginMask;
    if (locked)
        flags |= kSegmentLockedBit;
    return flags;
}

const Chunk* StateChunks::find(std::uint32_t fourcc) const noexcept
{
    for (const auto& c : chunks)
    {
        if (c.fourcc == fourcc)
            return &c;
    }
    return nullptr;
}

void StateChunks::set(std::uint32_t fourcc, std::vector<std::uint8_t> payload)
{
    for (auto& c : chunks)
    {
        if (c.fourcc == fourcc)
        {
            c.payload = std::move(payload);
            return;
        }
    }
    chunks.push_back(Chunk{fourcc, std::move(payload)});
}

void StateChunks::remove(std::uint32_t fourcc)
{
    chunks.erase(std::remove_if(chunks.begin(), chunks.end(), [fourcc](const Chunk& c) { return c.fourcc == fourcc; }),
                 chunks.end());
}

bool parseHeader(const std::uint8_t* data, std::size_t size, StateHeader& out)
{
    if (data == nullptr || size < 16u)
        return false;
    ByteReader r(data, size);
    out.magic = r.u32();
    out.abi = r.u32();
    out.flags = r.u32();
    out.chunkCount = r.u32();
    return r.ok();
}

DecodeStatus decodeContainer(const std::uint8_t* data, std::size_t size, StateChunks& out)
{
    StateHeader hdr;
    if (!parseHeader(data, size, hdr) || hdr.magic != kStateMagic || hdr.chunkCount > kMaxChunkCount)
        return DecodeStatus::Corrupt;

    StateChunks result;
    result.abi = hdr.abi;
    result.flags = hdr.flags;

    ByteReader r(data, size);
    r.skip(16u); // 头部已由 parseHeader 校验

    for (std::uint32_t i = 0; i < hdr.chunkCount; ++i)
    {
        const std::uint32_t fourcc = r.u32();
        const std::uint32_t sizeBytes = r.u32();
        if (!r.ok())
            return DecodeStatus::Corrupt;
        if (sizeBytes > r.remaining())
            return DecodeStatus::Corrupt;

        Chunk c;
        c.fourcc = fourcc;
        const std::uint8_t* p = r.ptr();
        c.payload.assign(p, p + sizeBytes);
        r.skip(sizeBytes);

        const std::size_t pad = (4u - (sizeBytes % 4u)) % 4u;
        if (pad > r.remaining())
            return DecodeStatus::Corrupt;
        r.skip(pad);

        result.chunks.push_back(std::move(c));
    }

    if (!r.ok())
        return DecodeStatus::Corrupt;

    out = std::move(result);
    return DecodeStatus::Ok;
}

bool encodeContainer(const StateChunks& chunks, std::vector<std::uint8_t>& out)
{
    if (chunks.chunks.size() > std::numeric_limits<std::uint32_t>::max())
        return false;

    ByteWriter w;
    w.u32(kStateMagic);
    w.u32(chunks.abi);
    w.u32(chunks.flags);
    w.u32(static_cast<std::uint32_t>(chunks.chunks.size()));
    for (const auto& c : chunks.chunks)
    {
        if (c.payload.size() > std::numeric_limits<std::uint32_t>::max())
            return false;
        w.u32(c.fourcc);
        w.u32(static_cast<std::uint32_t>(c.payload.size()));
        w.bytes(c.payload.data(), c.payload.size());
        w.align4();
    }
    out = w.data();
    return true;
}

// ---------------------------------------------------------------------------
// CRVS 节(little-endian,自带 u16 minor 版本):
//   u16 minorVersion(=1);u8 versionCount(=2);u8 trackCount(=15)
//   每版本:u16 nameBytes + name; i32 copiedFrom; i64 copiedAtMs
//           每轨:u32 segmentCount + segmentCount×Segment;u32 exclCount + exclCount×ExcludedRange
//           每版本:u16 panPointCount + panPointCount×PanPoint
// ---------------------------------------------------------------------------
bool encodeCrvs(const CrvsData& data, std::vector<std::uint8_t>& out)
{
    ByteWriter w;
    w.u16(kCrvsMinorVersion);
    w.u8(static_cast<std::uint8_t>(data.versions.size()));
    w.u8(kNumTracks);
    for (const auto& v : data.versions)
    {
        if (v.meta.name.size() > kMaxVersionNameBytes)
            return false;
        w.u16(static_cast<std::uint16_t>(v.meta.name.size()));
        w.bytes(v.meta.name.data(), v.meta.name.size());
        w.i32(v.meta.copiedFrom);
        w.i64(v.meta.copiedAtMs);
        for (const auto& t : v.tracks)
        {
            if (t.segments.size() > kMaxSegmentsPerTrack || t.excludedRanges.size() > kMaxExcludedRangesPerTrack)
                return false;
            w.u32(static_cast<std::uint32_t>(t.segments.size()));
            for (const auto& s : t.segments)
            {
                w.i64(s.t0);
                w.i64(s.t1);
                w.f32(s.pan);
                w.f32(s.volDb);
                w.u32(s.flags);
            }
            w.u32(static_cast<std::uint32_t>(t.excludedRanges.size()));
            for (const auto& e : t.excludedRanges)
            {
                w.i64(e.t0);
                w.i64(e.t1);
            }
        }
        if (v.panCurve.size() > kMaxPanPointsPerVersion)
            return false;
        w.u16(static_cast<std::uint16_t>(v.panCurve.size()));
        for (const auto& p : v.panCurve)
        {
            w.f32(p.angle);
            w.f32(p.gainDb);
            w.u8(static_cast<std::uint8_t>(p.shape));
            w.f32(p.q);
            w.u8(static_cast<std::uint8_t>(p.side));
        }
    }
    out = w.data();
    return true;
}

bool decodeCrvs(const std::uint8_t* data, std::size_t size, CrvsData& out)
{
    if (data == nullptr)
        return false;
    ByteReader r(data, size);

    const std::uint16_t minor = r.u16();
    const std::uint8_t versionCount = r.u8();
    const std::uint8_t trackCount = r.u8();
    // minorVersion 高于当前 → 布局未知,拒解(不可静默丢数据);版本/轨道数与 abi=1 定长结构不符 → 损坏。
    if (!r.ok() || minor != kCrvsMinorVersion || versionCount != kNumVersions || trackCount != kNumTracks)
        return false;

    CrvsData result;
    for (std::size_t vi = 0; vi < versionCount; ++vi)
    {
        auto& v = result.versions[vi];
        const std::uint16_t nameBytes = r.u16();
        if (!r.ok() || nameBytes > kMaxVersionNameBytes || nameBytes > r.remaining())
            return false;
        v.meta.name.assign(reinterpret_cast<const char*>(r.ptr()), nameBytes);
        r.skip(nameBytes);
        v.meta.copiedFrom = r.i32();
        v.meta.copiedAtMs = r.i64();
        if (!r.ok())
            return false;

        for (std::size_t ti = 0; ti < trackCount; ++ti)
        {
            auto& t = result.versions[vi].tracks[ti];
            const std::uint32_t segCount = r.u32();
            if (!r.ok() || segCount > kMaxSegmentsPerTrack)
                return false;
            if (segCount > r.remaining() / 28u) // 每段 28 字节,先防分配/索引越界
                return false;
            t.segments.reserve(segCount);
            for (std::uint32_t i = 0; i < segCount; ++i)
            {
                Segment s;
                s.t0 = r.i64();
                s.t1 = r.i64();
                s.pan = r.f32();
                s.volDb = r.f32();
                s.flags = r.u32();
                t.segments.push_back(s);
            }

            const std::uint32_t exclCount = r.u32();
            if (!r.ok() || exclCount > kMaxExcludedRangesPerTrack)
                return false;
            if (exclCount > r.remaining() / 16u) // 每段 {i64,i64} = 16 字节
                return false;
            t.excludedRanges.reserve(exclCount);
            for (std::uint32_t i = 0; i < exclCount; ++i)
            {
                ExcludedRange e;
                e.t0 = r.i64();
                e.t1 = r.i64();
                t.excludedRanges.push_back(e);
            }
        }

        const std::uint16_t pointCount = r.u16();
        if (!r.ok() || pointCount > kMaxPanPointsPerVersion)
            return false;
        if (pointCount > r.remaining() / 14u) // 每点 {f32,f32,u8,f32,u8} = 14 字节
            return false;
        v.panCurve.reserve(pointCount);
        for (std::uint16_t i = 0; i < pointCount; ++i)
        {
            scvb::PanCurvePoint p;
            p.angle = r.f32();
            p.gainDb = r.f32();
            const std::uint8_t shape = r.u8();
            p.q = r.f32();
            const std::uint8_t side = r.u8();
            if (!r.ok() || shape > 2u || side > 2u)
                return false;
            p.shape = static_cast<PanCurveShape>(shape);
            p.side = static_cast<PanCurveSide>(side);
            v.panCurve.push_back(p);
        }
    }

    if (!r.ok())
        return false;
    out = std::move(result);
    return true;
}

} // namespace scvb::state
