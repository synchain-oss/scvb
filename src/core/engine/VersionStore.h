// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "engine/AuthorityMode.h"
#include "engine/CurveEvaluator.h"

// T18 版本层:versions[2] 曲线真身 + 版本名(03 §5 / ADR-005 / [J05] / [J59])。纯 C++17,无 JUCE(ADR-011)。
//
// 曲线不可变契约(PR #43 终审,硬前置):
//   - 版本持有的曲线一经 setCurve 注入即不可变 —— 任何「改版本曲线内容」都必须新建 CurveEvaluator
//     对象再注入,禁止对已注入对象调用 build()。
//   - 曲线生命周期须 ≥ OutputAuthority/音频线程寿命。本存储以 std::shared_ptr<const CurveEvaluator>
//     持有曲线:setCurve/copyVersion 一律「新建对象 + 替换 shared_ptr」,绝不原地改写已发布对象;
//     旧对象由仍引用它的快照(std::shared_ptr)保活,音频线程绝不读到悬垂或被原地改写的曲线。
namespace scvb::engine
{

// [J59] 版本数 4→2;[J59] 15 轨;[J05] 版本名 ≤16 字符。
inline constexpr int kNumVersions = 2;
inline constexpr int kNumTracks = 15;
inline constexpr int kMaxVersionNameChars = 16;

// 版本复制结果(03 §5.3 前置校验)。
enum class CopyVersionResult
{
    Ok = 0,
    RejectedPrint = 1, // authorityMode == PRINT(打印中禁止,§5.2/§5.3)
    InvalidIndex = 2, // src/dst 越界(1..kNumVersions)
    SameVersion = 3, // src == dst
};

// 版本名设置结果([J05])。
enum class SetNameResult
{
    Ok = 0,
    InvalidIndex = 1,
    EmptyFellBack = 2, // 空值回落默认 V{n}
    Truncated = 3, // 超长(>16 字符)截断到 16 字符
};

// 版本元数据(03 §5.3:versions[dst].meta = {copied_from, copied_at})。
struct VersionMeta
{
    int copiedFrom = 0; // 1-based 来源版本号;0 = 未复制
    std::int64_t copiedAtMs = 0; // 复制时刻(epoch 毫秒);0 = 未复制
};

// 单个版本:名称 + 每轨曲线真身 + 元数据。曲线按轨持有不可变 shared_ptr。
struct Version
{
    std::string name; // 默认 "V1"/"V2",见 VersionStore 构造
    std::array<std::shared_ptr<const CurveEvaluator>, kNumTracks> curves{};
    VersionMeta meta{};
};

// 归一化版本名:空值 → 默认 V{n};超长(>16 字符)→ 截断到 16 字符。返回结果,out 为生效名。
SetNameResult normalizeVersionName(int version, const std::string& input, std::string& out);

class VersionStore
{
public:
    VersionStore();

    // ---- 版本名 [J05] ----
    const std::string& versionName(int version) const; // version 1-based
    SetNameResult setVersionName(int version, const std::string& name);

    // ---- 曲线真身存取(不可变契约,见文件头)----
    // setCurve:深拷贝 src 进新建对象再替换;src 可为 nullptr(清空该轨 → 恒 0)。
    void setCurve(int version, int track, const CurveEvaluator* curve);
    // curve:只读裸指针(仅用于采样值;对象寿命由本存储或引用它的快照保证)。
    const CurveEvaluator* curve(int version, int track) const;

    // ---- 版本复制 §5.3(纯 state 深拷贝,零 gesture、零参数写入)----
    CopyVersionResult copyVersion(int src, int dst, AuthorityMode mode); // 校验 + 执行
    CopyVersionResult validateCopy(int src, int dst, AuthorityMode mode) const;
    void applyCopy(int src, int dst); // 不校验;深拷贝 src → dst(新建对象,保持 dst 名,更新 meta)

    // 撤销支撑:浅拷贝 dst 的 shared_ptr 快照(对象不可变,浅拷贝即完整语义快照)。
    std::array<std::shared_ptr<const CurveEvaluator>, kNumTracks> snapshotCurves(int version) const;
    void restoreCurves(int version, const std::array<std::shared_ptr<const CurveEvaluator>, kNumTracks>& curves);

    // ---- meta ----
    const VersionMeta& meta(int version) const;
    void setMeta(int version, const VersionMeta& meta);

    // ---- version_active(非自动化 state,§5.2):值域 1..kNumVersions,越界钳制 + warning 计数(不静默取模)----
    int versionActive() const;
    int setVersionActive(int version); // 返回钳制后的生效值
    int warningCount() const;

private:
    std::array<Version, kNumVersions> m_versions;
    int m_versionActive = 1;
    int m_warningCount = 0;
};

} // namespace scvb::engine
