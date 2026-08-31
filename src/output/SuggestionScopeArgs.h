// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "export/SuggestionExport.h"

// exportSuggestions(scope) 的入参归一(契约 §1.36)。
//
// 单拎成纯函数的理由与同目录 `AnalyzeScopeMath.h` 一脉相承:它埋在 `OutputEditor` 的私有
// 成员里就够不着 —— 那个类要 WebView 才构造得起来,离线 harness 编不进那个 TU,于是「入参
// 归一」这一层只能靠源码正则去看,改回去照样绿。抽成纯函数,`scvb_params_tests` 可以直接调。
//
// **口径以「契约 §1.36 拒绝态行 + mock 实现」为准**(#163 复审两条【红旗】的教训):
// 这一层最容易犯的错是「凭直觉给参数加守卫」,而契约与 mock 早把语义定死了 —— 加出来的
// 守卫不是更严谨,是**制造真桥与预览页的行为分叉**,而现有门禁比不出行为差异。
//
// §1.36 拒绝态逐字:`badArg`(**versions 不在两值内 / `endS ≤ startS`**)、`cancelled`、
// `noData`(**scope 内零段**)、`ioError`。**badArg 的触发面只有那两条**:
//   · `tracksMask` 掩完为 0 **不是 badArg** —— 它落在「scope 内零段」⇒ `noData`。
//     mock(`channelsOfMask(0) → []` ⇒ `rows === 0` ⇒ `noData`)与冒烟
//     (`smoke-t41-suggestions.mjs` 的「零轨 → noData」)都钉死了这一档。
//     曾照 §1.23 [J87] 的「掩完为 0 ⇒ badArg」抄过来 —— **错了**:`recaptureArm` 是写函数,
//     空 mask 会改运行时态所以必须拒;导出是只读的,「零轨 → 零行 → noData」本身就是准确
//     且用户看得懂的反馈,没有升格成 badArg 的理由。
//
// 时间窗口径**与 mock 逐条对齐**(mock:`isFiniteNumber(x) ? x : ±Infinity`):
//   · 两头都给且 `end > start` ⇒ 逐字照用;
//   · 两头都给但 `end <= start` ⇒ `badArg`(§1.36 明写,mock 亦然);
//   · **只给一头 ⇒ 半开窗**,不是「整个不限」。`{startS:30}` 的字面意思就是「从 30 秒起」,
//     退化成全时间线等于把用户明确给了的那一头**丢掉**,而这条路没有任何回显能让他发现。
//     `Scope` 用 `startSec >= 0 && endSec > startSec` 表示「窗生效」(见 `inWindow`),
//     表达不了 ±∞,故:缺右端取 `double` 上限(`t0Sec < max` 恒真,与 `+∞` 等效);
//     缺左端取 `0.0`(段时间恒 ≥ 0,真实段上 `t1Sec > 0` 与 `> -∞` 等效)。
//   · 非有限值(NaN / ±Inf)**按「没给」处理**,与 mock 的 `isFiniteNumber` 同口径。
//     顺带:`Infinity` 其实过不了桥 —— `JSON.stringify(Infinity)` 是 `"null"`,桥面判
//     「是不是数」会判假;这道守卫是为别的调用方与语义完整性留的。
//
// 「给没给」由调用方判(桥面 `juce::var` 的 `hasProperty` + 是不是数),本函数只收已解出的
// 值 + 一个 has 标志 —— 与 `analyzeScopeRange` 同款签名风格,免得把 JUCE 类型拖进纯层。

namespace scvb::output
{

struct SuggestionScopeParse
{
    scvb::suggest::Scope scope{};
    bool badArg = false;
};

inline SuggestionScopeParse parseSuggestionScope(bool hasVersions, const std::string& versions, bool hasMask,
                                                 int maskRaw, bool hasStart, double startS, bool hasEnd, double endS,
                                                 int activeVersion)
{
    SuggestionScopeParse r;
    r.scope.activeVersion = activeVersion;

    if (hasVersions)
    {
        if (versions == "all")
        {
            r.scope.allVersions = true;
        }
        else if (versions == "active")
        {
            r.scope.allVersions = false;
        }
        else
        {
            r.badArg = true; // §1.36 拒绝态第一条:versions 不在两值内
            return r;
        }
    }

    if (hasMask)
    {
        // bit15 是 §9.2 的保留位,恒 0。掩完为 0 **不拒** —— 让它自然走到零行集 ⇒ noData。
        r.scope.tracksMask = static_cast<std::uint16_t>(static_cast<unsigned>(maskRaw) & 0x7FFFu);
    }

    // 非有限值按「没给」处理(与 mock 的 isFiniteNumber 同口径)
    const bool okStart = hasStart && std::isfinite(startS);
    const bool okEnd = hasEnd && std::isfinite(endS);

    if (okStart && okEnd)
    {
        if (!(endS > startS))
        {
            r.badArg = true; // §1.36 拒绝态第二条:endS ≤ startS
            return r;
        }
        r.scope.startSec = startS;
        r.scope.endSec = endS;
    }
    else if (okStart)
    {
        r.scope.startSec = startS;
        r.scope.endSec = std::numeric_limits<double>::max(); // ≡ +∞(mock 口径)
    }
    else if (okEnd)
    {
        if (endS > 0.0)
        {
            // ≡ −∞:段时间恒 ≥ 0,真实段上 `t1Sec > 0` 与 `> -∞` 等效。
            r.scope.startSec = 0.0;
            r.scope.endSec = endS;
        }
        else
        {
            // 右端 ≤ 0 ⇒ 窗内不可能有段(段时间恒 ≥ 0)。**不是 badArg**(§1.36 没这一条),
            // 但也**不能**退回「不限」——那会把「只要 0 秒之前的段」变成「导出全部」。
            // ⚠ `Scope` 表达不了「空窗」:`inWindow` 在 `!(endSec > startSec)` 时**关掉筛选**
            //   (返回 true),所以 `0/0` 反而是「全导」—— 与本意正相反。
            // 故取一个真实段够不到的远端区间:`t1Sec > startSec` 对任何真实段恒假 ⇒ 零行 ⇒
            // noData,与 mock 的 `t0S < endS` 恒假同结果。
            r.scope.startSec = std::numeric_limits<double>::max() * 0.5;
            r.scope.endSec = std::numeric_limits<double>::max();
        }
    }
    // 两头都没给(或都非有限)⇒ Scope 默认的 -1/-1 = 不限,不用写。

    return r;
}

} // namespace scvb::output
