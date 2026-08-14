// SPDX-License-Identifier: GPL-3.0-or-later
// S4 spike:state 编解码实现(容器 TLV + CRVS/CFGS/PRMS 紧凑二进制 + FEAT gzip + sidecar)。

#include "StateCodec.h"

#include "ByteIo.h"

#include <juce_cryptography/juce_cryptography.h>

#include <cstring>
#include <limits>

namespace scvb::s4
{
namespace
{

// ---------------------------------------------------------------------------
// gzip(生产同款 juce::GZIPCompressorOutputStream,zlib 默认等级 6 / zlib 容器格式)。
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> gzipCompress(const void* data, std::size_t size)
{
    juce::MemoryOutputStream mos;
    {
        juce::GZIPCompressorOutputStream gz(mos);
        gz.write(data, size);
        gz.flush();
    }
    const auto* p = static_cast<const std::uint8_t*>(mos.getData());
    return std::vector<std::uint8_t>(p, p + mos.getDataSize());
}

bool gzipDecompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out)
{
    out.clear();
    juce::MemoryInputStream mis(data, size, false);
    juce::GZIPDecompressorInputStream gz(mis);
    juce::MemoryOutputStream mos;
    mos.writeFromInputStream(gz, -1);
    const auto* p = static_cast<const std::uint8_t*>(mos.getData());
    out.assign(p, p + mos.getDataSize());
    return true;
}

// ---------------------------------------------------------------------------
// 节编码(全部 little-endian,确定性)。
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> encodeCrvs(const std::vector<VersionCurves>& versions)
{
    ByteWriter w;
    w.u16(kCrvsMinorVersion);
    w.u32(static_cast<std::uint32_t>(versions.size()));
    for (const auto& v : versions)
    {
        w.u16(static_cast<std::uint16_t>(v.name.size()));
        w.bytes(v.name.data(), v.name.size());
        w.u32(static_cast<std::uint32_t>(v.tracks.size()));
        for (const auto& t : v.tracks)
        {
            w.u32(static_cast<std::uint32_t>(t.segments.size()));
            for (const auto& seg : t.segments)
            {
                w.i64(seg.t0);
                w.i64(seg.t1);
                w.f32(seg.pan);
                w.f32(seg.volDb);
                w.u32(seg.flags);
            }
            w.u32(static_cast<std::uint32_t>(t.excludedRanges.size()));
            for (const auto& r : t.excludedRanges)
            {
                w.i64(r.t0);
                w.i64(r.t1);
            }
        }
        w.u32(static_cast<std::uint32_t>(v.panCurve.points.size()));
        for (const auto& p : v.panCurve.points)
        {
            w.f32(p.angle);
            w.f32(p.gainDb);
            w.u32(p.shape);
            w.f32(p.q);
            w.u32(p.side);
        }
    }
    return w.data();
}

bool decodeCrvs(const std::vector<std::uint8_t>& raw, std::vector<VersionCurves>& versions)
{
    ByteReader r(raw.data(), raw.size());
    const std::uint16_t minor = r.u16();
    if (!r.ok() || minor > kCrvsMinorVersion)
        return false;
    const std::uint32_t versionCount = r.u32();
    if (!r.ok() || versionCount > 64u)
        return false;
    versions.clear();
    versions.reserve(versionCount);
    for (std::uint32_t vi = 0; vi < versionCount; ++vi)
    {
        VersionCurves v;
        const std::uint16_t nameLen = r.u16();
        if (!r.ok() || nameLen > 1024u)
            return false;
        v.name.assign(reinterpret_cast<const char*>(r.ptr()), nameLen);
        r.skip(nameLen);
        const std::uint32_t trackCount = r.u32();
        if (!r.ok() || trackCount > 64u)
            return false;
        for (std::uint32_t ti = 0; ti < trackCount; ++ti)
        {
            TrackCurves t;
            const std::uint32_t segCount = r.u32();
            if (!r.ok() || segCount > 1000000u)
                return false;
            for (std::uint32_t i = 0; i < segCount; ++i)
            {
                CurveSegment s;
                s.t0 = r.i64();
                s.t1 = r.i64();
                s.pan = r.f32();
                s.volDb = r.f32();
                s.flags = r.u32();
                t.segments.push_back(s);
            }
            const std::uint32_t exclCount = r.u32();
            if (!r.ok() || exclCount > 1000000u)
                return false;
            for (std::uint32_t i = 0; i < exclCount; ++i)
            {
                SampleRange e;
                e.t0 = r.i64();
                e.t1 = r.i64();
                t.excludedRanges.push_back(e);
            }
            v.tracks.push_back(std::move(t));
        }
        const std::uint32_t pointCount = r.u32();
        if (!r.ok() || pointCount > 4096u)
            return false;
        for (std::uint32_t i = 0; i < pointCount; ++i)
        {
            PanCurvePoint p;
            p.angle = r.f32();
            p.gainDb = r.f32();
            p.shape = r.u32();
            p.q = r.f32();
            p.side = r.u32();
            v.panCurve.points.push_back(p);
        }
        versions.push_back(std::move(v));
    }
    return r.ok();
}

std::vector<std::uint8_t> encodeCfgs(const FullState& s)
{
    ByteWriter w;
    w.u32(s.groupId);
    w.u8(s.global.captureEnabled ? 1u : 0u);
    w.u8(s.global.outputEnabled ? 1u : 0u);
    w.u32(s.global.versionActive);
    w.u32(s.global.rangeMode);
    w.f32(s.global.rangeStartS);
    w.f32(s.global.rangeEndS);
    w.f32(s.analysis.vadThresholdDb);
    w.f32(s.analysis.vadHysteresisDb);
    w.u32(s.analysis.vadHangoverMs);
    w.u32(s.analysis.vadPaddingPreMs);
    w.u32(s.analysis.vadPaddingPostMs);
    w.u32(s.analysis.segmentationMode);
    w.u32(s.analysis.segmentationSensitivity);
    w.u32(s.analysis.segmentationMinSegmentMs);
    w.u32(s.analysis.transitionRampMs);
    w.u32(static_cast<std::uint32_t>(s.channels.size()));
    for (const auto& c : s.channels)
    {
        w.u8(c.enabled ? 1u : 0u);
        w.u16(static_cast<std::uint16_t>(c.label.size()));
        w.bytes(c.label.data(), c.label.size());
        w.u32(c.sourceChannels);
        w.u8(c.participateInAutoPan ? 1u : 0u);
        w.u32(c.priority);
        w.u8(c.leadLock ? 1u : 0u);
        w.u8(c.leadVolExempt ? 1u : 0u);
        w.u32(c.pairId);
    }
    return w.data();
}

bool decodeCfgs(const std::vector<std::uint8_t>& raw, FullState& s)
{
    ByteReader r(raw.data(), raw.size());
    s.groupId = r.u32();
    s.global.captureEnabled = r.u8() != 0u;
    s.global.outputEnabled = r.u8() != 0u;
    s.global.versionActive = r.u32();
    s.global.rangeMode = r.u32();
    s.global.rangeStartS = r.f32();
    s.global.rangeEndS = r.f32();
    s.analysis.vadThresholdDb = r.f32();
    s.analysis.vadHysteresisDb = r.f32();
    s.analysis.vadHangoverMs = r.u32();
    s.analysis.vadPaddingPreMs = r.u32();
    s.analysis.vadPaddingPostMs = r.u32();
    s.analysis.segmentationMode = r.u32();
    s.analysis.segmentationSensitivity = r.u32();
    s.analysis.segmentationMinSegmentMs = r.u32();
    s.analysis.transitionRampMs = r.u32();
    const std::uint32_t channelCount = r.u32();
    if (!r.ok() || channelCount > 64u)
        return false;
    s.channels.clear();
    for (std::uint32_t i = 0; i < channelCount; ++i)
    {
        ChannelConfig c;
        c.enabled = r.u8() != 0u;
        const std::uint16_t labelLen = r.u16();
        if (!r.ok() || labelLen > 1024u)
            return false;
        c.label.assign(reinterpret_cast<const char*>(r.ptr()), labelLen);
        r.skip(labelLen);
        c.sourceChannels = r.u32();
        c.participateInAutoPan = r.u8() != 0u;
        c.priority = r.u32();
        c.leadLock = r.u8() != 0u;
        c.leadVolExempt = r.u8() != 0u;
        c.pairId = r.u32();
        s.channels.push_back(std::move(c));
    }
    return r.ok();
}

std::vector<std::uint8_t> encodePrms(const FullState& s)
{
    ByteWriter w;
    w.u32(kParamCount);
    for (const float v : s.params)
        w.f32(v);
    w.f32(s.ui.scale);
    w.u32(s.ui.language);
    w.u32(s.ui.activeTab);
    w.u8(s.ui.guideSeen ? 1u : 0u);
    w.u8(s.ui.tourSeen ? 1u : 0u);
    return w.data();
}

bool decodePrms(const std::vector<std::uint8_t>& raw, FullState& s)
{
    ByteReader r(raw.data(), raw.size());
    const std::uint32_t paramCount = r.u32();
    if (!r.ok() || paramCount != kParamCount)
        return false;
    for (std::uint32_t i = 0; i < paramCount; ++i)
        s.params[i] = r.f32();
    s.ui.scale = r.f32();
    s.ui.language = r.u32();
    s.ui.activeTab = r.u32();
    s.ui.guideSeen = r.u8() != 0u;
    s.ui.tourSeen = r.u8() != 0u;
    return r.ok();
}

// ---------------------------------------------------------------------------
// FEAT 节(04 §5.2 布局,压缩前)。
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> encodeFeatSection(const FullState& s, bool embedded, const std::string& guid,
                                            const std::array<std::uint8_t, 32>& sha256, std::uint64_t sidecarBytes)
{
    ByteWriter w;
    w.u32(kFourccFeat);
    w.u16(kFeatCodecVer);
    w.u16(embedded ? kFeatFlagEmbedded : 0u);
    w.u32(s.sampleRate);
    w.u32(s.hopMs);
    w.u8(static_cast<std::uint8_t>(s.features.size()));
    if (embedded)
    {
        for (const auto& f : s.features)
        {
            w.u8(f.channelId);
            w.u32(static_cast<std::uint32_t>(f.coverage.size()));
            for (const auto& r : f.coverage)
            {
                w.u64(r.beginHop);
                w.u64(r.endHop);
            }
            for (const auto v : f.kwDbq)
                w.i16(v);
            for (const auto v : f.peakDbq)
                w.i16(v);
            for (const auto v : f.vadPosterior)
                w.u8(v);
        }
    }
    else
    {
        w.bytes(guid.data(), guid.size());
        w.bytes(sha256.data(), sha256.size());
        w.u64(sidecarBytes);
    }
    return w.data();
}

struct FeatMeta
{
    bool embedded = true;
    std::string guid;
    std::array<std::uint8_t, 32> sha256{};
    std::uint64_t sidecarBytes = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t hopMs = 0;
};

bool decodeFeatSection(const std::vector<std::uint8_t>& raw, FeatMeta& meta, std::vector<ChannelFeatures>& features)
{
    ByteReader r(raw.data(), raw.size());
    const std::uint32_t tag = r.u32();
    if (!r.ok() || tag != kFourccFeat)
        return false;
    const std::uint16_t codecVer = r.u16();
    const std::uint16_t flags = r.u16();
    meta.sampleRate = r.u32();
    meta.hopMs = r.u32();
    const std::uint8_t channelCount = r.u8();
    if (!r.ok())
        return false;
    if (codecVer > kFeatCodecVer)
    {
        // codecVer 高于当前:该节按空处理(04 §5.2 加载),不硬失败。
        features.clear();
        meta.embedded = true;
        return true;
    }
    meta.embedded = (flags & kFeatFlagEmbedded) != 0u;
    features.clear();
    if (meta.embedded)
    {
        for (std::uint8_t c = 0; c < channelCount; ++c)
        {
            ChannelFeatures f;
            f.channelId = r.u8();
            const std::uint32_t rangeCount = r.u32();
            if (!r.ok())
                return false;
            std::uint64_t hopCount = 0;
            for (std::uint32_t i = 0; i < rangeCount; ++i)
            {
                const std::uint64_t b = r.u64();
                const std::uint64_t e = r.u64();
                if (!r.ok() || e < b)
                    return false;
                f.coverage.push_back(Range{b, e});
                hopCount += (e - b);
            }
            // 5 B/hop(kw i16 + peak i16 + vad u8):hopCount 不可能超过剩余字节 / 5(压缩炸弹防护)。
            if (hopCount > r.remaining() / 5u)
                return false;
            f.kwDbq.resize(hopCount);
            f.peakDbq.resize(hopCount);
            f.vadPosterior.resize(hopCount);
            for (std::uint64_t i = 0; i < hopCount; ++i)
                f.kwDbq[i] = r.i16();
            for (std::uint64_t i = 0; i < hopCount; ++i)
                f.peakDbq[i] = r.i16();
            for (std::uint64_t i = 0; i < hopCount; ++i)
                f.vadPosterior[i] = r.u8();
            if (!r.ok())
                return false;
            features.push_back(std::move(f));
        }
    }
    else
    {
        std::array<char, 36> guidBuf{};
        if (r.remaining() < 36u + 32u + 8u)
            return false;
        std::memcpy(guidBuf.data(), r.ptr(), 36u);
        r.skip(36u);
        std::memcpy(meta.sha256.data(), r.ptr(), 32u);
        r.skip(32u);
        meta.sidecarBytes = r.u64();
        meta.guid = std::string(guidBuf.data(), 36u);
    }
    return r.ok();
}

// ---------------------------------------------------------------------------
// sidecar 定位与读写。
// ---------------------------------------------------------------------------
juce::File defaultSidecarBase()
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("scvb-s4-spike");
}

juce::File sidecarDirFor(const juce::File& baseDir, const std::string& guid)
{
    const juce::File base = baseDir.getFullPathName().isEmpty() ? defaultSidecarBase() : baseDir;
    return base.getChildFile("sessions").getChildFile(guid);
}

juce::SHA256 sha256Of(const void* data, std::size_t size)
{
    return juce::SHA256(juce::MemoryBlock(data, size));
}

bool writeSidecarFiles(const juce::File& dir, const std::vector<std::uint8_t>& gzBytes, std::uint32_t channelCount)
{
    if (dir.createDirectory().failed())
        return false;
    auto tmp = dir.getChildFile("features.bin.gz.tmp");
    if (!tmp.replaceWithData(gzBytes.data(), gzBytes.size()))
        return false;
    auto featuresFile = dir.getChildFile("features.bin.gz");
    featuresFile.deleteFile();
    if (!tmp.moveFileTo(featuresFile))
        return false;

    const auto digest = sha256Of(gzBytes.data(), gzBytes.size());
    const std::string manifest =
        "schemaVersion=1 codecVer=" + std::to_string(kFeatCodecVer) + " bytes=" + std::to_string(gzBytes.size()) +
        " sha256=" + digest.toHexString().toStdString() + " channelCount=" + std::to_string(channelCount);
    auto manifestFile = dir.getChildFile("manifest.json");
    manifestFile.replaceWithText(manifest);

    auto lock = dir.getChildFile("owner.lock");
    lock.replaceWithText("pid=0 heartbeatIso8601=spike");
    return true;
}

// ---------------------------------------------------------------------------
// 容器(03 §6.1)。
// ---------------------------------------------------------------------------
void writeChunk(ByteWriter& w, std::uint32_t fourcc, const std::vector<std::uint8_t>& payload)
{
    w.u32(fourcc);
    w.u32(static_cast<std::uint32_t>(payload.size()));
    w.bytes(payload.data(), payload.size());
    w.align4();
}

} // namespace

juce::File sidecarDirectoryFor(const juce::File& baseDir, const std::string& guid)
{
    return sidecarDirFor(baseDir, guid);
}

// ---------------------------------------------------------------------------
EncodeResult encode(const FullState& state, const EncodeOptions& opts)
{
    EncodeResult res;

    // 1. 四个节。
    const std::vector<std::uint8_t> prms = encodePrms(state);
    const std::vector<std::uint8_t> cfgs = encodeCfgs(state);
    const std::vector<std::uint8_t> crvs = encodeCrvs(state.versions);
    const std::vector<std::uint8_t> featRaw = encodeFeatSection(state, true, {}, {}, 0);
    const std::vector<std::uint8_t> featGz = gzipCompress(featRaw.data(), featRaw.size());
    res.rawFeatBytes = featRaw.size();
    res.gzFeatBytes = featGz.size();

    // 2. 内嵌 / sidecar 判定(ADR-007 阈值;hysteresis 留 T21)。
    res.embedded = !opts.forceSidecar && featGz.size() <= opts.sidecarThresholdBytes;

    std::string guid = state.sessionGuid;
    std::vector<std::uint8_t> featSection;
    if (res.embedded)
    {
        featSection = featGz; // 容器 FEAT chunk = gzip(FeatSection)
    }
    else
    {
        if (guid.empty())
            guid = juce::Uuid().toDashedString().toStdString(); // 生产会持久化进 state(04 §5.5)
        res.sessionGuid = guid;
        const auto digest = sha256Of(featGz.data(), featGz.size());
        std::array<std::uint8_t, 32> sha{};
        std::memcpy(sha.data(), digest.getRawData().getData(), 32u);
        const juce::File dir = sidecarDirFor(opts.sidecarBaseDir, guid);
        if (!writeSidecarFiles(dir, featGz, static_cast<std::uint32_t>(state.features.size())))
        {
            // sidecar 写失败 -> 特征丢弃 + 警告(01 §7.3);state 仍保存(嵌入空特征引用)。
            FullState emptyFeat = state;
            emptyFeat.features.clear();
            const std::vector<std::uint8_t> emptyRaw = encodeFeatSection(emptyFeat, true, {}, {}, 0);
            featSection = gzipCompress(emptyRaw.data(), emptyRaw.size());
            res.embedded = true;
            res.message = "sidecar write failed, features dropped";
        }
        else
        {
            const std::vector<std::uint8_t> refSection = encodeFeatSection(state, false, guid, sha, featGz.size());
            featSection = gzipCompress(refSection.data(), refSection.size());
        }
    }

    // 3. 容器。
    ByteWriter w;
    const std::uint32_t chunkCount = 4u + static_cast<std::uint32_t>(state.unknownChunks.size());
    w.u32(kContainerMagic);
    w.u32(state.abi);
    w.u32(kContainerFlagsDefault);
    w.u32(chunkCount);
    writeChunk(w, kFourccPrms, prms);
    writeChunk(w, kFourccCfgs, cfgs);
    writeChunk(w, kFourccCrvs, crvs);
    writeChunk(w, kFourccFeat, featSection);
    for (const auto& uc : state.unknownChunks)
        writeChunk(w, uc.fourcc, uc.payload);

    res.bytes = w.data();
    res.totalBytes = res.bytes.size();
    return res;
}

LoadResult decode(const std::uint8_t* data, std::size_t size, const juce::File& sidecarBaseDir)
{
    LoadResult res;
    if (data == nullptr)
    {
        res.status = LoadStatus::Corrupt;
        res.message = "null input";
        return res;
    }

    ByteReader r(data, size);
    const std::uint32_t magic = r.u32();
    const std::uint32_t abi = r.u32();
    (void)r.u32(); // flags(本 spike 不读,保留字段)
    const std::uint32_t chunkCount = r.u32();
    if (!r.ok() || magic != kContainerMagic)
    {
        res.status = LoadStatus::Corrupt;
        res.message = "magic mismatch or truncated header";
        return res;
    }
    if (abi > kCurrentAbi)
    {
        res.status = LoadStatus::RejectedNewer;
        res.message = "saved by a newer SCVB version (abi " + std::to_string(abi) + " > " +
                      std::to_string(kCurrentAbi) + "), please upgrade";
        res.preservedOriginal.assign(data, data + size);
        return res;
    }
    if (abi < kCurrentAbi)
    {
        // 无历史迁移函数(spike 无 kMigrators);abi=0 视为损坏。
        res.status = LoadStatus::Corrupt;
        res.message = "no migrator for abi " + std::to_string(abi);
        return res;
    }

    FullState s;
    s.abi = abi;
    if (chunkCount > 64u)
    {
        res.status = LoadStatus::Corrupt;
        res.message = "chunkCount out of range";
        return res;
    }

    bool featFound = false;
    std::vector<std::uint8_t> featRaw;
    for (std::uint32_t i = 0; i < chunkCount; ++i)
    {
        const std::uint32_t fourcc = r.u32();
        const std::uint32_t sizeBytes = r.u32();
        if (!r.ok() || sizeBytes > r.remaining())
        {
            res.status = LoadStatus::Corrupt;
            res.message = "chunk payload exceeds container";
            return res;
        }
        const std::uint8_t* payload = r.ptr();
        std::vector<std::uint8_t> payloadCopy(payload, payload + sizeBytes);
        r.skip(sizeBytes);
        const std::size_t pad = (4u - (sizeBytes % 4u)) % 4u;
        if (pad > r.remaining())
        {
            res.status = LoadStatus::Corrupt;
            res.message = "chunk padding exceeds container";
            return res;
        }
        r.skip(pad);

        switch (fourcc)
        {
        case kFourccPrms:
            if (!decodePrms(payloadCopy, s))
            {
                res.status = LoadStatus::Corrupt;
                res.message = "PRMS parse failed";
                return res;
            }
            break;
        case kFourccCfgs:
            if (!decodeCfgs(payloadCopy, s))
            {
                res.status = LoadStatus::Corrupt;
                res.message = "CFGS parse failed";
                return res;
            }
            break;
        case kFourccCrvs:
            if (!decodeCrvs(payloadCopy, s.versions))
            {
                res.status = LoadStatus::Corrupt;
                res.message = "CRVS parse failed";
                return res;
            }
            break;
        case kFourccFeat:
            featFound = true;
            featRaw = std::move(payloadCopy);
            break;
        default:
            s.unknownChunks.push_back(UnknownChunk{fourcc, std::move(payloadCopy)});
            break;
        }
    }

    // 特征:gunzip FEAT chunk -> FeatSection;embedded=0 时按 GUID 读 sidecar。
    if (featFound)
    {
        std::vector<std::uint8_t> featUncompressed;
        gzipDecompress(featRaw.data(), featRaw.size(), featUncompressed);
        FeatMeta meta;
        if (!decodeFeatSection(featUncompressed, meta, s.features))
        {
            res.status = LoadStatus::Corrupt;
            res.message = "FEAT parse failed";
            return res;
        }
        s.sampleRate = meta.sampleRate;
        s.hopMs = meta.hopMs;
        if (!meta.embedded)
        {
            const juce::File dir = sidecarDirFor(sidecarBaseDir, meta.guid);
            auto featuresFile = dir.getChildFile("features.bin.gz");
            juce::MemoryBlock mb;
            if (!featuresFile.existsAsFile() || !featuresFile.loadFileAsData(mb))
            {
                res.featuresMissing = true;
                res.message = "sidecar missing, features cleared (curves/config intact)";
                s.features.clear();
            }
            else
            {
                const auto digest = sha256Of(mb.getData(), mb.getSize());
                if (digest.getRawData().getSize() != 32u ||
                    std::memcmp(digest.getRawData().getData(), meta.sha256.data(), 32u) != 0)
                {
                    res.featuresMissing = true;
                    res.message = "sidecar sha256 mismatch, features cleared (curves/config intact)";
                    s.features.clear();
                }
                else
                {
                    std::vector<std::uint8_t> sideUncompressed;
                    gzipDecompress(static_cast<const std::uint8_t*>(mb.getData()), mb.getSize(), sideUncompressed);
                    FeatMeta sideMeta;
                    if (!decodeFeatSection(sideUncompressed, sideMeta, s.features))
                    {
                        res.featuresMissing = true;
                        res.message = "sidecar parse failed, features cleared (curves/config intact)";
                        s.features.clear();
                    }
                }
            }
        }
    }

    res.status = LoadStatus::Ok;
    res.state = std::move(s);
    return res;
}

// ---------------------------------------------------------------------------
void ProcessorState::load(const std::uint8_t* data, std::size_t size, const juce::File& sidecarBaseDir)
{
    last_ = decode(data, size, sidecarBaseDir);
}

std::vector<std::uint8_t> ProcessorState::save(const EncodeOptions& opts) const
{
    if (last_.status == LoadStatus::RejectedNewer)
        return last_.preservedOriginal; // 原样回写,不毁高版本数据(01 §7.2)
    return encode(last_.state, opts).bytes;
}

} // namespace scvb::s4
