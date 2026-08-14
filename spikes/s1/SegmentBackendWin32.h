// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S1 spike —— Windows 命名共享内存段后端(CreateFileMapping/MapViewOfFile)+ 01 §4.0
// 初始化协议(含覆盖式重初始化)。仅消息线程调用;音频线程只拿映射后的裸指针做原子读写。

#include <atomic>
#include <cstddef>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "SegmentLayout.h"

namespace scvb
{

// 段名构造(ipc v1.5 [J66]:全部段名带组号 g{G},G=1..8)。
std::wstring segmentRegistryName(u32 group);
std::wstring segmentAudioName(u32 group, u32 channel); // channel ∈ [1, 15]
std::wstring segmentCtrlName(u32 group);

// 段映射视图(RAII:析构/移动时 UnmapViewOfFile + CloseHandle)。
struct SegmentView
{
    void* base = nullptr;
    std::size_t size = 0;
    HANDLE mapping = nullptr;
    bool created = false;

    SegmentView() = default;
    SegmentView(SegmentView&& other) noexcept;
    SegmentView& operator=(SegmentView&& other) noexcept;
    SegmentView(const SegmentView&) = delete;
    SegmentView& operator=(const SegmentView&) = delete;
    ~SegmentView();

    void reset();
};

// 初始化结果。
enum class InitResult
{
    kOk, // 创建成功或 attach 成功
    kAbiMismatch, // magic 相符但 abi 不符(J40:拒连不崩溃)
    kFailed // 映射失败
};

class SegmentBackendWin32
{
public:
    // 创建或打开命名共享内存段,写入 view。返回 kOk / kFailed。
    // created=true 时内核已零初始化整段;created=false 时需经 initHeader 判定是否可用。
    static InitResult map(const std::wstring& name, std::size_t size, SegmentView& view);

    // 仅打开已存在的命名段(OpenFileMappingW,不创建)——Output 侧 attach audio.chN 用。
    // 段不存在 → kFailed。映射整段(view.size 置 0,由调用方按 header 字段推算)。
    static InitResult openExisting(const std::wstring& name, SegmentView& view);

    // 01 §4.0 初始化协议(registry/audio 通用;header 前两字段为 magic/abi,可选 generation):
    //   created=true:写 abi=1、generation=1(若有)、release-store magic;
    //   created=false:自旋读 magic(50ms × 10)。magic ok 且 abi ok → kOk;
    //     abi 不符 → kAbiMismatch;超时仍 magic==0 → 覆盖式重初始化(清 slot 区、
    //     写 abi、generation.fetch_add(1) 若有、CAS magic 0→'SCVB';失败即他人已完成,直接 attach)。
    //   headerMagic/headerAbi 指向视图内的 magic/abi 字段;headerGen 可空;dataOffset 为
    //   「清 slot 区」的起始偏移(通常 = 头部大小,覆盖式重初始化时从该偏移起清零)。
    static InitResult initHeader(SegmentView& view, std::atomic<u32>* headerMagic, std::atomic<u32>* headerAbi,
                                 std::atomic<u32>* headerGen, std::size_t dataOffset);
};

} // namespace scvb
