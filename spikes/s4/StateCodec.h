// SPDX-License-Identifier: GPL-3.0-or-later
// S4 spike:state 编解码(save/load 往返 + abi 拒载 + 内嵌/sidecar 切换)。
//   gzip 用 juce::GZIPCompressorOutputStream / GZIPDecompressorInputStream(生产同款,ADR-011)。

#pragma once

#include "StateSchema.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <string>
#include <vector>

namespace scvb::s4
{

enum class LoadStatus
{
    Ok, // 正常加载(可能 sidecar 缺失 -> featuresMissing=true)
    RejectedNewer, // abi > kCurrentAbi:拒载,保留原 blob
    Corrupt // magic/长度非法:拒载,默认态
};

struct LoadResult
{
    LoadStatus status = LoadStatus::Corrupt;
    FullState state; // Ok 时有效
    std::vector<std::uint8_t> preservedOriginal; // RejectedNewer 时保留原 blob
    bool featuresMissing = false; // sidecar 缺失/校验失败(Ok 但特征空)
    std::string message;
};

struct EncodeOptions
{
    bool forceSidecar = false; // 测试用:跳过 8MB 判定,强制走 sidecar
    std::uint64_t sidecarThresholdBytes = kSidecarThresholdBytes;
    juce::File sidecarBaseDir{}; // 空 -> 默认临时目录
};

struct EncodeResult
{
    std::vector<std::uint8_t> bytes; // state chunk(给宿主)
    bool embedded = true;
    std::string sessionGuid;
    std::uint64_t rawFeatBytes = 0; // FEAT 压缩前字节
    std::uint64_t gzFeatBytes = 0; // FEAT 压缩后字节(内嵌=chunk 载荷;sidecar=文件字节)
    std::uint64_t totalBytes = 0; // 整个 state chunk 字节
    std::string message;
};

// 序列化:模型 -> state chunk(特征默认内嵌,超阈值/强制时转 sidecar)。
EncodeResult encode(const FullState& state, const EncodeOptions& opts);

// 反序列化:state chunk -> 模型(校验 magic/长度/abi;特征内嵌解压或按 GUID 读 sidecar)。
LoadResult decode(const std::uint8_t* data, std::size_t size, const juce::File& sidecarBaseDir);

// sidecar 目录定位(供编解码与测试共用):<base>/sessions/<guid>。
juce::File sidecarDirectoryFor(const juce::File& baseDir, const std::string& guid);

// 最小状态机,复现 01 §7.2 的 setState/getState 语义:
//   读到高版本 -> 拒载 + 保留原 blob,此后 save() 原样回写(不毁高版本数据)。
class ProcessorState
{
public:
    void load(const std::uint8_t* data, std::size_t size, const juce::File& sidecarBaseDir);
    std::vector<std::uint8_t> save(const EncodeOptions& opts) const;
    LoadStatus status() const { return last_.status; }
    const LoadResult& lastResult() const { return last_; }

private:
    LoadResult last_;
};

} // namespace scvb::s4
