// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/FeaturesCodec.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "miniz.h"

// T21:FEAT 节编解码实现。little-endian、确定性(roundtrip 逐字节一致);gzip 用 miniz 的
// zlib 兼容 API(window_bits=15 → RFC 1950 zlib 容器,S4 结论「gzip 实为 zlib 格式」)。
namespace scvb::state
{
namespace
{

// ---- 确定性 little-endian 字节读写器(不可信字节:先校验边界再返回,CLAUDE.md §7.3)。----
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

    void bytes(const void* p, std::size_t n)
    {
        const auto* b = static_cast<const std::uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }

    std::vector<std::uint8_t>& data() { return buf_; }

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
        const std::uint16_t u = u16();
        std::int16_t v = 0;
        std::memcpy(&v, &u, sizeof(v));
        return v;
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

// ---- gzip(zlib 格式)----
} // namespace

std::vector<std::uint8_t> gzipCompress(const void* data, std::size_t size)
{
    if (data == nullptr || size > std::numeric_limits<mz_uint>::max())
        return {};

    mz_stream stream{};
    if (mz_deflateInit2(&stream, MZ_DEFAULT_LEVEL, MZ_DEFLATED, MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) !=
        MZ_OK)
        return {};

    const mz_ulong bound = mz_deflateBound(&stream, static_cast<mz_ulong>(size));
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bound));
    stream.next_in = static_cast<const mz_uint8*>(data);
    stream.avail_in = static_cast<mz_uint>(size);
    stream.next_out = out.data();
    stream.avail_out = static_cast<mz_uint>(out.size());

    const int rc = mz_deflate(&stream, MZ_FINISH);
    mz_deflateEnd(&stream);
    if (rc != MZ_STREAM_END)
        return {};
    out.resize(static_cast<std::size_t>(stream.total_out));
    return out;
}

bool gzipDecompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out, std::size_t maxOutBytes)
{
    out.clear();
    if (data == nullptr || size > std::numeric_limits<mz_uint>::max())
        return false; // 与 gzipCompress 对称:>4GB 输入会被 32 位 avail_in 截断

    mz_stream stream{};
    if (mz_inflateInit2(&stream, MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        return false;

    stream.next_in = static_cast<const mz_uint8*>(data);
    stream.avail_in = static_cast<mz_uint>(size);

    constexpr std::size_t kChunk = 64u * 1024u;
    std::vector<std::uint8_t> buf;
    int rc = MZ_OK;
    do
    {
        const std::size_t start = buf.size();
        buf.resize(start + kChunk);
        stream.next_out = buf.data() + start;
        stream.avail_out = static_cast<mz_uint>(kChunk);
        const mz_uint availInBefore = stream.avail_in;
        rc = mz_inflate(&stream, MZ_NO_FLUSH);
        const mz_uint produced = static_cast<mz_uint>(kChunk) - stream.avail_out;
        buf.resize(start + produced);
        if (buf.size() > maxOutBytes)
            break; // 压缩炸弹
        // 无进展(既未消耗输入、也未产出输出)且未到流末尾 → 截断/坏流。
        // 不用 avail_in==0 判定:输出恰为 64KB 整数倍时,合法流的最后一次产出会先把输入耗尽、
        // 由下一轮再返回 MZ_STREAM_END,误用 avail_in==0 会把合法流判成截断。
        if (rc == MZ_OK && stream.avail_in == availInBefore && produced == 0u)
            break;
    } while (rc == MZ_OK);

    mz_inflateEnd(&stream);
    if (rc != MZ_STREAM_END || buf.size() > maxOutBytes)
        return false;
    out = std::move(buf);
    return true;
}

bool isValidSessionGuid(std::string_view guid) noexcept
{
    if (guid.size() != 36u)
        return false;
    for (std::size_t i = 0; i < 36u; ++i)
    {
        const char c = guid[i];
        if (i == 8u || i == 13u || i == 18u || i == 23u)
        {
            if (c != '-')
                return false;
        }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
        {
            return false;
        }
    }
    return true;
}

namespace
{
// ---- FEAT 节内编解码 ----

// 校验并计算单 channel 覆盖 hop 总数;失败返回 false。
bool validatedHopCount(const ChannelFeatures& c, std::uint64_t& hopCount)
{
    hopCount = 0;
    for (const auto& r : c.coverage)
    {
        if (r.end < r.begin)
            return false;
        const std::uint64_t span = r.end - r.begin;
        if (span > kMaxHopsPerChannel || hopCount > kMaxHopsPerChannel - span)
            return false;
        hopCount += span;
    }
    return true;
}

// 编码 embedded 节(未压缩)。
bool encodeEmbeddedSection(const FeaturesData& data, std::vector<std::uint8_t>& out)
{
    if (data.channels.size() > kMaxFeatChannels)
        return false;
    if (data.sampleRate < kMinSampleRate || data.sampleRate > kMaxSampleRate)
        return false;
    if (data.hopMs < kMinHopMs || data.hopMs > kMaxHopMs)
        return false;

    ByteWriter w;
    w.u32(kFeatTag);
    w.u16(kFeatCodecVer);
    w.u16(kFeatFlagEmbedded | (data.vadPresent ? kFeatFlagVadPresent : 0u));
    w.u32(data.sampleRate);
    w.u32(data.hopMs);
    w.u8(static_cast<std::uint8_t>(data.channels.size()));

    for (const auto& c : data.channels)
    {
        std::uint64_t hopCount = 0;
        if (!validatedHopCount(c, hopCount) || c.coverage.size() > kMaxRangesPerChannel)
            return false;
        if (c.kwDbq.size() != hopCount || c.peakDbq.size() != hopCount)
            return false;
        if (data.vadPresent && c.vadPosterior.size() != hopCount)
            return false;
        if (c.channelId < 1u || c.channelId > kMaxFeatChannels)
            return false;

        w.u8(c.channelId);
        w.u32(static_cast<std::uint32_t>(c.coverage.size()));
        for (const auto& r : c.coverage)
        {
            w.u64(r.begin);
            w.u64(r.end);
        }
        for (const auto v : c.kwDbq)
            w.i16(v);
        for (const auto v : c.peakDbq)
            w.i16(v);
        if (data.vadPresent)
        {
            for (const auto v : c.vadPosterior)
                w.u8(v);
        }
    }

    out = std::move(w.data());
    return true;
}

// 编码 sidecar 引用节(未压缩)。
bool encodeReferenceSection(const SidecarRef& ref, std::uint32_t sampleRate, std::uint32_t hopMs,
                            std::uint8_t channelCount, std::vector<std::uint8_t>& out)
{
    if (!isValidSessionGuid(ref.sessionGuid))
        return false;

    ByteWriter w;
    w.u32(kFeatTag);
    w.u16(kFeatCodecVer);
    w.u16(0u); // flags:embedded=0(无 channel 数据,bit1 vadPresent 亦无意义)
    w.u32(sampleRate);
    w.u32(hopMs);
    w.u8(channelCount);
    w.bytes(ref.sessionGuid.data(), 36u);
    w.bytes(ref.sha256.data(), ref.sha256.size());
    w.u64(ref.sidecarBytes);

    out = std::move(w.data());
    return true;
}

} // namespace

std::vector<std::uint8_t> encodeFeatures(const FeaturesData& data)
{
    std::vector<std::uint8_t> section;
    if (!encodeEmbeddedSection(data, section))
        return {};
    return gzipCompress(section.data(), section.size());
}

std::vector<std::uint8_t> encodeReference(const SidecarRef& ref, std::uint32_t sampleRate, std::uint32_t hopMs,
                                          std::uint8_t channelCount)
{
    std::vector<std::uint8_t> section;
    if (!encodeReferenceSection(ref, sampleRate, hopMs, channelCount, section))
        return {};
    return gzipCompress(section.data(), section.size());
}

FeatDecode decodeFeatures(const std::uint8_t* gzData, std::size_t size)
{
    FeatDecode out;

    std::vector<std::uint8_t> raw;
    if (!gzipDecompress(gzData, size, raw, kMaxDecompressedFeatBytes))
        return out; // ok=false

    ByteReader r(raw.data(), raw.size());
    const std::uint32_t tag = r.u32();
    const std::uint16_t codecVer = r.u16();
    const std::uint16_t flags = r.u16();
    const std::uint32_t sampleRate = r.u32();
    const std::uint32_t hopMs = r.u32();
    const std::uint8_t channelCount = r.u8();
    if (!r.ok() || tag != kFeatTag)
        return out;

    if (codecVer > kFeatCodecVer)
    {
        // 更高 codecVer:不解码(ok=false),特征按空处理;接线层据此**原样保留**原始 chunk 字节
        // 并提示升级,绝不可重编码空特征(对齐 T19 StateCodec 三态:Ok/Newer/Corrupt)。
        out.codecVerNewer = true;
        return out;
    }
    if (codecVer < 1u || channelCount > kMaxFeatChannels)
        return out;
    if (sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate)
        return out;
    if (hopMs < kMinHopMs || hopMs > kMaxHopMs)
        return out;

    const bool embedded = (flags & kFeatFlagEmbedded) != 0u;
    const bool vadPresent = (flags & kFeatFlagVadPresent) != 0u;

    if (embedded)
    {
        FeaturesData data;
        data.sampleRate = sampleRate;
        data.hopMs = hopMs;
        data.vadPresent = vadPresent;
        data.channels.reserve(channelCount);

        for (std::uint8_t ci = 0; ci < channelCount; ++ci)
        {
            ChannelFeatures c;
            c.channelId = r.u8();
            if (!r.ok() || c.channelId < 1u || c.channelId > kMaxFeatChannels)
                return out;
            const std::uint32_t rangeCount = r.u32();
            if (!r.ok() || rangeCount > kMaxRangesPerChannel)
                return out;
            if (rangeCount > r.remaining() / 16u) // 每范围 {u64,u64} = 16 字节
                return out;

            std::uint64_t hopCount = 0;
            c.coverage.reserve(rangeCount);
            for (std::uint32_t i = 0; i < rangeCount; ++i)
            {
                const std::uint64_t b = r.u64();
                const std::uint64_t e = r.u64();
                if (!r.ok() || e < b)
                    return out;
                c.coverage.push_back(HopRange{b, e});
                const std::uint64_t span = e - b;
                if (span > kMaxHopsPerChannel || hopCount > kMaxHopsPerChannel - span)
                    return out;
                hopCount += span;
            }

            // 每 hop:kw(2)+peak(2)+[vad(1)] = 4 或 5 字节;先防分配/索引越界。
            const std::size_t bytesPerHop = vadPresent ? 5u : 4u;
            if (hopCount > r.remaining() / bytesPerHop)
                return out;

            const std::size_t hc = static_cast<std::size_t>(hopCount);
            c.kwDbq.resize(hc);
            c.peakDbq.resize(hc);
            for (std::size_t i = 0; i < hc; ++i)
                c.kwDbq[i] = r.i16();
            for (std::size_t i = 0; i < hc; ++i)
                c.peakDbq[i] = r.i16();
            if (vadPresent)
            {
                c.vadPosterior.resize(hc);
                for (std::size_t i = 0; i < hc; ++i)
                    c.vadPosterior[i] = r.u8();
            }
            if (!r.ok())
                return out;
            data.channels.push_back(std::move(c));
        }

        out.ok = true;
        out.embedded = true;
        out.features = std::move(data);
        return out;
    }

    // embedded=0:sessionGuid[36] + sha256[32] + sidecarBytes[8]
    if (r.remaining() < 36u + 32u + 8u)
        return out;
    std::array<char, 36> guidBuf{};
    std::memcpy(guidBuf.data(), r.ptr(), 36u);
    r.skip(36u);
    if (!isValidSessionGuid(std::string_view(guidBuf.data(), 36u)))
        return out; // 非法 guid(路径穿越/非 UUID)→ 拒解
    out.ref.sessionGuid.assign(guidBuf.data(), 36u);
    std::memcpy(out.ref.sha256.data(), r.ptr(), 32u);
    r.skip(32u);
    out.ref.sidecarBytes = r.u64();
    if (!r.ok())
        return out;

    out.ok = true;
    out.embedded = false;
    return out;
}

} // namespace scvb::state
