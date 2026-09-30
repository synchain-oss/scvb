// SPDX-License-Identifier: GPL-3.0-or-later
// test_version —— T18 版本层:VersionStore(versions[2] 曲线真身 + 版本名)纯核心单测。
// 覆盖:copyVersion 深拷贝/零参数写入/零 gesture 的结构性保证、PRINT 拒绝、version_active 钳制、
// name 边界(空值回落/超长截断)、曲线不可变契约。无 JUCE(ADR-011)。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>

#include "engine/AuthorityMode.h"
#include "engine/CurveEvaluator.h"
#include "engine/VersionStore.h"

using Catch::Approx;

namespace
{

using scvb::engine::AuthorityMode;
using scvb::engine::CopyVersionResult;
using scvb::engine::SetNameResult;
using scvb::engine::VersionStore;

// 常量曲线:单段覆盖 [0,1000),pan/vol 恒定。
scvb::CurveEvaluator constCurve(double pan, double volDb)
{
    scvb::CurveEvaluator ev;
    ev.build({scvb::CurveSegment{0.0, 1000.0, pan, volDb}}, scvb::TransitionConfig{});
    return ev;
}

} // namespace

// ============================================================================
// 版本名 [J05]
// ============================================================================

TEST_CASE("VERSION-NAME-1 默认名 V1/V2", "[version][name]")
{
    VersionStore store;
    REQUIRE(store.versionName(1) == "V1");
    REQUIRE(store.versionName(2) == "V2");
}

TEST_CASE("VERSION-NAME-2 合法名往返一致", "[version][name]")
{
    VersionStore store;
    REQUIRE(store.setVersionName(1, "Lead") == SetNameResult::Ok);
    REQUIRE(store.versionName(1) == "Lead");
}

TEST_CASE("VERSION-NAME-3 空值回落默认 V{n}", "[version][name]")
{
    VersionStore store;
    REQUIRE(store.setVersionName(1, "Lead") == SetNameResult::Ok);
    REQUIRE(store.setVersionName(1, "") == SetNameResult::EmptyFellBack);
    REQUIRE(store.versionName(1) == "V1"); // 空值 → 默认 V1
}

TEST_CASE("VERSION-NAME-4 超长截断到 16 字符(UTF-8 不劈半)", "[version][name]")
{
    VersionStore store;
    // 20 个 ASCII 字符 → 截断到前 16。
    REQUIRE(store.setVersionName(2, "abcdefghijklmnopqrst") == SetNameResult::Truncated);
    REQUIRE(store.versionName(2) == "abcdefghijklmnop");
    REQUIRE(store.versionName(2).size() == 16u);

    // 多字节字符不被劈开:20 个中文(3 字节/字)截断到前 16 个码点,尾字完整。
    REQUIRE(store.setVersionName(1, "一二三四五六七八九十一二三四五六七八九十") == SetNameResult::Truncated);
    REQUIRE(store.versionName(1) == "一二三四五六七八九十一二三四五六");
}

TEST_CASE("VERSION-NAME-5 非法版本号拒绝", "[version][name]")
{
    VersionStore store;
    REQUIRE(store.setVersionName(0, "X") == SetNameResult::InvalidIndex);
    REQUIRE(store.setVersionName(3, "X") == SetNameResult::InvalidIndex);
    REQUIRE(store.versionName(1) == "V1"); // 未被污染
}

// ============================================================================
// version_active 值域 1..2(越界钳制 + warning,不静默取模)
// ============================================================================

TEST_CASE("VERSION-ACTIVE-1 默认 1", "[version][active]")
{
    VersionStore store;
    REQUIRE(store.versionActive() == 1);
    REQUIRE(store.warningCount() == 0);
}

TEST_CASE("VERSION-ACTIVE-2 合法切换", "[version][active]")
{
    VersionStore store;
    REQUIRE(store.setVersionActive(2) == 2);
    REQUIRE(store.versionActive() == 2);
    REQUIRE(store.warningCount() == 0);
}

TEST_CASE("VERSION-ACTIVE-3 越界钳制 + warning(不取模)", "[version][active]")
{
    VersionStore store;
    REQUIRE(store.setVersionActive(0) == 1); // 下越界 → 钳到 1(若取模会得 0/1?)
    REQUIRE(store.warningCount() == 1);
    REQUIRE(store.setVersionActive(3) == 2); // 上越界 → 钳到 2(若取模会得 1)
    REQUIRE(store.warningCount() == 2);
    REQUIRE(store.setVersionActive(-100) == 1);
    REQUIRE(store.setVersionActive(100) == 2);
    REQUIRE(store.warningCount() == 4);
    REQUIRE(store.versionActive() == 2);
}

// ============================================================================
// 曲线不可变契约:setCurve 深拷贝,源对象后续 mutate 不影响 store
// ============================================================================

TEST_CASE("VERSION-CURVE-1 setCurve 深拷贝(注入后源对象可变不改 store)", "[version][immutable]")
{
    VersionStore store;
    scvb::CurveEvaluator src = constCurve(30.0, -3.0);
    store.setCurve(1, 0, &src);

    // 注入后 mutate 源对象(模拟违反契约的调用方对「原对象」build):
    src.build({scvb::CurveSegment{0.0, 1000.0, 99.0, 9.0}}, scvb::TransitionConfig{});

    // store 持有的副本不受影响(深拷贝,对象独立)。
    REQUIRE(store.curve(1, 0) != nullptr);
    REQUIRE(store.curve(1, 0)->panAt(0.0) == Approx(30.0).margin(1e-9));
    REQUIRE(store.curve(1, 0)->volAt(0.0) == Approx(-3.0).margin(1e-9));
}

TEST_CASE("VERSION-CURVE-2 setCurve nullptr 清空该轨", "[version][immutable]")
{
    VersionStore store;
    scvb::CurveEvaluator src = constCurve(30.0, -3.0);
    store.setCurve(1, 0, &src);
    REQUIRE(store.curve(1, 0) != nullptr);

    store.setCurve(1, 0, nullptr);
    REQUIRE(store.curve(1, 0) == nullptr);
}

// ============================================================================
// 版本复制 §5.3:纯 state 深拷贝
// ============================================================================

TEST_CASE("VERSION-COPY-1 深拷贝:值相等、对象独立", "[version][copy]")
{
    VersionStore store;
    scvb::CurveEvaluator src = constCurve(30.0, -3.0);
    store.setCurve(1, 0, &src);

    REQUIRE(store.copyVersion(1, 2, AuthorityMode::Follow) == CopyVersionResult::Ok);

    REQUIRE(store.curve(2, 0) != nullptr);
    REQUIRE(store.curve(2, 0)->panAt(0.0) == Approx(30.0).margin(1e-9));
    REQUIRE(store.curve(2, 0)->volAt(0.0) == Approx(-3.0).margin(1e-9));
    // 深拷贝:目标持有新对象(非与 src 共享同一对象)。
    REQUIRE(store.curve(2, 0) != store.curve(1, 0));
}

TEST_CASE("VERSION-COPY-2 复制后目标名不变", "[version][copy]")
{
    VersionStore store;
    store.setVersionName(2, "我的版本");
    scvb::CurveEvaluator src = constCurve(30.0, -3.0);
    store.setCurve(1, 0, &src);

    REQUIRE(store.copyVersion(1, 2, AuthorityMode::Follow) == CopyVersionResult::Ok);
    REQUIRE(store.versionName(2) == "我的版本"); // name 不随复制覆盖
}

TEST_CASE("VERSION-COPY-3 meta 记录 copied_from/copied_at", "[version][copy]")
{
    VersionStore store;
    scvb::CurveEvaluator src = constCurve(30.0, -3.0);
    store.setCurve(1, 0, &src);

    REQUIRE(store.meta(2).copiedFrom == 0); // 复制前未复制
    REQUIRE(store.meta(2).copiedAtMs == 0);

    REQUIRE(store.copyVersion(1, 2, AuthorityMode::Follow) == CopyVersionResult::Ok);
    REQUIRE(store.meta(2).copiedFrom == 1);
    REQUIRE(store.meta(2).copiedAtMs > 0);
}

TEST_CASE("VERSION-COPY-4 前置校验:PRINT 拒绝 / 越界 / 同版本", "[version][copy]")
{
    VersionStore store;
    REQUIRE(store.copyVersion(1, 2, AuthorityMode::Print) == CopyVersionResult::RejectedPrint);
    REQUIRE(store.copyVersion(0, 2, AuthorityMode::Follow) == CopyVersionResult::InvalidIndex);
    REQUIRE(store.copyVersion(1, 3, AuthorityMode::Follow) == CopyVersionResult::InvalidIndex);
    REQUIRE(store.copyVersion(2, 2, AuthorityMode::Follow) == CopyVersionResult::SameVersion);
}

TEST_CASE("VERSION-COPY-5 复制后改 src 不影响 dst(独立深拷贝)", "[version][copy]")
{
    VersionStore store;
    scvb::CurveEvaluator srcA = constCurve(30.0, -3.0);
    store.setCurve(1, 0, &srcA);
    REQUIRE(store.copyVersion(1, 2, AuthorityMode::Follow) == CopyVersionResult::Ok);

    // 复制后重注入 src 版本(新曲线 -99/-9)。
    scvb::CurveEvaluator srcB = constCurve(-99.0, -9.0);
    store.setCurve(1, 0, &srcB);

    REQUIRE(store.curve(1, 0)->panAt(0.0) == Approx(-99.0).margin(1e-9)); // src 已变
    REQUIRE(store.curve(2, 0)->panAt(0.0) == Approx(30.0).margin(1e-9)); // dst 独立不变
}

TEST_CASE("VERSION-COPY-6 复制空 src 轨得 null", "[version][copy]")
{
    VersionStore store;
    // src 版本轨 5 无曲线(默认 null)。
    REQUIRE(store.copyVersion(1, 2, AuthorityMode::Follow) == CopyVersionResult::Ok);
    REQUIRE(store.curve(2, 5) == nullptr);
}

// ============================================================================
// 撤销支撑:snapshotCurves/restoreCurves 浅拷贝(不可变契约下浅拷贝即完整快照)
// ============================================================================

TEST_CASE("VERSION-SNAPSHOT-1 快照/恢复往返", "[version][snapshot]")
{
    VersionStore store;
    scvb::CurveEvaluator src = constCurve(30.0, -3.0);
    store.setCurve(1, 0, &src);

    const auto snap = store.snapshotCurves(1);
    REQUIRE(snap[0] != nullptr);

    // 覆盖后再恢复。
    store.setCurve(1, 0, nullptr);
    REQUIRE(store.curve(1, 0) == nullptr);

    store.restoreCurves(1, snap);
    REQUIRE(store.curve(1, 0) != nullptr);
    REQUIRE(store.curve(1, 0)->panAt(0.0) == Approx(30.0).margin(1e-9));
}
