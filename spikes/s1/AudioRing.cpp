// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioRing.h"

#include <cstdlib>

namespace scvb
{

u32 ringFramesFromEnv()
{
    const char* v = std::getenv("SCVB_RING_FRAMES");
    if (v == nullptr || *v == '\0')
    {
        return kDefaultRingFrames;
    }
    const unsigned long parsed = std::strtoul(v, nullptr, 10);
    if (parsed == 0 || parsed > 0x80000000u)
    {
        return kDefaultRingFrames;
    }
    const u32 value = static_cast<u32>(parsed);
    // 必须是 2^k(寻址靠 mask = ring_frames-1)。
    if ((value & (value - 1)) != 0)
    {
        return kDefaultRingFrames;
    }
    return value;
}

AudioRing::~AudioRing()
{
    // SegmentView 析构时 Unmap/CloseHandle。
}

InitResult AudioRing::createForInput(u32 sampleRate, u32 ringFrames, u32 channels)
{
    if (channels != 1 && channels != 2)
    {
        return InitResult::kFailed;
    }
    const std::size_t size = sizeof(AudioRingHeader) + static_cast<std::size_t>(ringFrames) * channels * sizeof(float);

    const auto r = SegmentBackendWin32::map(segmentAudioName(group_, channel_), size, view_);
    if (r != InitResult::kOk)
    {
        return r;
    }
    auto* header = static_cast<AudioRingHeader*>(view_.base);
    const bool created = view_.created;

    // 几何字段写回(magic 发布前落盘):sample_rate/ring_frames/channels/写头/epoch。
    const auto writeGeometry = [&]() {
        header->sample_rate = sampleRate;
        header->ring_frames = ringFrames;
        header->channels = channels;
        header->write_head_samples.store(0, std::memory_order_relaxed);
        header->epoch.store(0, std::memory_order_relaxed);
    };

    // magic 后行不变式:几何字段先写、magic 最后 release 发布(created 与覆盖式重初始化两路径都经 initData)。
    const auto ir = SegmentBackendWin32::initHeader(view_, &header->magic, &header->abi, nullptr,
                                                    sizeof(AudioRingHeader), [&]() { writeGeometry(); });
    if (ir != InitResult::kOk)
    {
        view_.reset();
        return ir;
    }

    if (!created)
    {
        // attach 到已有有效段(自身重初始化 / 接管残段):几何字段重写后 epoch+1 作为新代发布标记(读方弃旧代)。
        writeGeometry();
        header->epoch.fetch_add(1, std::memory_order_release);
    }

    header_ = header;
    data_ = reinterpret_cast<float*>(static_cast<char*>(view_.base) + sizeof(AudioRingHeader));
    ringFrames_ = ringFrames;
    channels_ = channels;
    lastEpoch_ = -1;
    validFrom_ = 0;
    return InitResult::kOk;
}

InitResult AudioRing::openForOutput()
{
    const auto r = SegmentBackendWin32::openExisting(segmentAudioName(group_, channel_), view_);
    if (r != InitResult::kOk)
    {
        return r;
    }
    auto* header = static_cast<AudioRingHeader*>(view_.base);
    // 非阻塞单次校验:magic 未就绪(创建者尚未发布)即返回失败,由 [M] 25Hz 下一 tick 重试,
    // 不在消息线程里 Sleep 自旋(避免串行叠加拖慢 UI/心跳,review 建议 4)。
    if (header->magic.load(std::memory_order_acquire) != kScvbMagic)
    {
        view_.reset();
        return InitResult::kFailed;
    }
    if (header->abi.load(std::memory_order_acquire) != kScvbAbi)
    {
        view_.reset();
        return InitResult::kAbiMismatch;
    }
    header_ = header;
    ringFrames_ = header->ring_frames;
    channels_ = header->channels;
    data_ = reinterpret_cast<float*>(static_cast<char*>(view_.base) + sizeof(AudioRingHeader));
    lastEpoch_ = -1;
    validFrom_ = 0;
    return InitResult::kOk;
}

void AudioRing::writeBlock(i64 t0, const float* interleavedSrc, int frames)
{
    if (!isOpen() || t0 < 0 || frames <= 0 || interleavedSrc == nullptr)
    {
        return;
    }
    const u32 nch = channels_;
    const u32 m = mask();
    for (int i = 0; i < frames; ++i)
    {
        const std::size_t base = (static_cast<std::size_t>(t0 + i) & m) * nch;
        for (u32 c = 0; c < nch; ++c)
        {
            data_[base + c] = interleavedSrc[static_cast<std::size_t>(i) * nch + c];
        }
    }
    // 数据先写,写头后发布(01 §5.1 步骤 4)。
    header_->write_head_samples.store(static_cast<u64>(t0 + frames), std::memory_order_release);
}

void AudioRing::bumpEpoch()
{
    if (isOpen())
    {
        header_->epoch.fetch_add(1, std::memory_order_release);
    }
}

AudioRing::ReadStatus AudioRing::readBlock(i64 t0, int frames, float* interleavedDst)
{
    if (!isOpen() || t0 < 0 || frames <= 0 || interleavedDst == nullptr)
    {
        return ReadStatus::kGap;
    }
    const u32 nch = channels_;
    const u32 m = mask();

    const u64 e1 = header_->epoch.load(std::memory_order_acquire);
    const u64 w = header_->write_head_samples.load(std::memory_order_acquire);
    if (e1 != static_cast<u64>(lastEpoch_))
    {
        lastEpoch_ = static_cast<i64>(e1);
        validFrom_ = t0; // 换代后本代有效数据从当前块起点起算(01 §5.2)
    }

    const u64 t0u = static_cast<u64>(t0);
    const bool covered =
        (t0 >= validFrom_) && (w >= t0u + static_cast<u64>(frames)) && (w - t0u <= static_cast<u64>(ringFrames_));
    if (!covered)
    {
        return ReadStatus::kGap;
    }

    for (int i = 0; i < frames; ++i)
    {
        const std::size_t base = (static_cast<std::size_t>(t0 + i) & m) * nch;
        for (u32 c = 0; c < nch; ++c)
        {
            interleavedDst[static_cast<std::size_t>(i) * nch + c] = data_[base + c];
        }
    }

    // 读后 write_head 复查(R1):读中被写方套圈 → 弃块 + gapCount(调用方)+1。
    const u64 e2 = header_->epoch.load(std::memory_order_acquire);
    const u64 w2 = header_->write_head_samples.load(std::memory_order_acquire);
    if (e2 != e1 || w2 > t0u + static_cast<u64>(ringFrames_))
    {
        return ReadStatus::kGap;
    }
    return ReadStatus::kOk;
}

} // namespace scvb
