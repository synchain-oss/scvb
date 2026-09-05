// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// [SL-324] **同机只允许一份 SCVB 测试进程** —— 四套二进制共用同一把命名互斥,
// 在整轮开始前判定,拿不到就整轮不跑。
//
// ## 为什么四套共用一把,而不是各自一把
//
// 固定组号在四套之间**大面积重叠**,八个组没有一个是独占的(SL-324 三通道枚举):
//
//     g1  scvb_tests(test_ipc_lifecycle) / host(MonoMultiRig 经越界回落) / ipc(--group=1 ×37 + resetRegistry ×6)
//     g2  scvb_tests(output/input_session) / monitor(kGroupA) / ipc
//     g3  ipc(kGroup、--group=3、segmentRegistryName(3)) / host(本卡把 MonoMultiRig 迁到这里)
//     g4  monitor(kGroupA) / host
//     g5  monitor(kGroupB) / host(kFromGroup)
//     g6  monitor(kGroup) / host(kOtherGroup、kToGroup) / ipc(kProbeGroup)
//     g7  monitor(kGroup,还 spawn viz-publisher 真建 g7.viz) / host(kTestGroup) / ipc(VIZ-2)
//     g8  host(kNoWriterGroup) / ipc(--group=8 claimer)
//
// 段名 `SynchainSCVB.v1.g{G}.…` 是**全机唯一**的,所以「每套一把锁」挡得住同套并发、
// **挡不住跨套件**:#204 给 host、#206 给 ipc 各加一把之后,ipc 的 VIZ-2 仍会被 host 或
// monitor 打红(实测过:VIZ-2 红时机器上正跑着 host 套件)。一把共用的锁一次性消掉全部重叠。
//
// 代价是**串行化所有测试进程**。这不新增等待:`scripts/with-ipc-lock.ps1`(SL-311)本来就在
// 做同一件事,区别只在于它依赖人记得走包装,而这把锁**不依赖任何人记得**。
//
// ## 为什么判定必须在整轮开始前
//
// Catch2 的 `FAIL` 用 `ResultDisposition::Normal` —— **只中止当前用例,不中止整轮**
// (#204 复审的结论)。守卫若挂在某个 fixture 上,第二个进程仍会跑完整轮、照常建段,
// **它仍在主动加害对方**。只有在 `testRunStarting` 里判、拿不到就 `std::exit`,
// 才能保证**一个段都不建**。
//
// ## 三条有意的边界
//
//   · **0 等待、不排队**:排队会把冲突藏起来,而且与 gates 的 `Local\SCVB-ipc-tests`
//     形成锁序风险(那把是 gates 自己串行 gate 3e 与 6/7/8 用的,与这把语义不同);
//   · **不碰 IPC 段**:`Registry::open()` 是 `createOrOpen` + `allowOverwrite=true`,
//     拿它当探针会创建、甚至覆盖式重初始化别人正在用的 registry —— 比它要治的问题更坏;
//   · **不看槽位**:「槽位活跃且 pid 非我」会被崩溃进程留下的**陈旧槽位**误报。
//
// 不手动释放:内核对象随进程退出自动回收,测试里放反而多一条出错路径。
//
// ## 用法
//
// 每个测试二进制在**任意一个** .cpp 里 `#include "support/exclusive_guard.h"` 即可
// (需要 `${CMAKE_SOURCE_DIR}/tests` 在 include 路径上)。取锁走函数内 static,
// **每进程至多一次** —— 即便某个二进制不小心在两个 TU 里都包含了本头,
// 也不会出现「自己把自己判成占用者」。
//
// 运行期文案一律 **ASCII**:中文字面量在本机 CP936 上会触发 MSVC C4819,而 ADR-011
// 要求 /W4 零 warning(SL-323 实测,归因见 SL-325)。中文只写在注释里。

// windows.h 的 min/max 宏会污染 `std::max` / `std::numeric_limits<T>::min()`;先禁再包含
//(与 `src/output/OutputProcessor.h`、`src/core/ipc/RegistryProbe.cpp` 同款)。
// **本头必须自带这道防护**:它会被包含进各测试 TU,而那些 TU 里的项目头(如
// `src/output/BridgeArgs.h` 的 `std::max`)可能排在本头之后 —— 实测不加就是
// `error C2062: 意外的类型「unknown-type」`,报在别人的文件上,极难联想到这里。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdio>
#include <cstdlib>

namespace scvb::testsupport
{

// 取锁结果:owned = 本进程独占;handle == nullptr 表示**建不出来**(与「已被占用」是两回事)。
struct ExclusiveAcquire
{
    HANDLE handle = nullptr;
    DWORD lastError = 0;
    bool owned = false;
};

// 每进程至多取一次(函数内 static)。句柄故意不关:活到进程退出,由内核回收。
inline const ExclusiveAcquire& acquireTestsExclusiveOnce()
{
    static const ExclusiveAcquire result = [] {
        ExclusiveAcquire r;
        r.handle = ::CreateMutexW(nullptr, TRUE, L"Local\\SCVB-tests-proc");
        // `GetLastError()` 必须**紧挨着**取:中间插任何一个 Win32 调用都可能把它冲掉。
        r.lastError = ::GetLastError();
        r.owned = (r.handle != nullptr && r.lastError != ERROR_ALREADY_EXISTS);
        return r;
    }();
    return result;
}

struct TestsExclusiveListener : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;

    // `TestRunInfo::name` 是 `Catch::StringRef` —— **没有 `c_str()`,也不保证 NUL 结尾**,
    // 所以用 `%.*s` + `size()/data()`,不要 `%s`(那是编译错误,而且即便编过也可能读越界)。
    void testRunStarting(Catch::TestRunInfo const& info) override
    {
        const ExclusiveAcquire& a = acquireTestsExclusiveOnce();
        if (a.owned)
        {
            return;
        }

        // **两种失败要分开说**:处方完全不同,合成一句会把人送错方向。
        if (a.handle == nullptr)
        {
            // 建不出来是**另一件事**:DACL 拒绝 / 句柄耗尽 / 同名非互斥对象占位。
            // 判负是对的(CLAUDE.md §2:建不出来判负,绝不静默继续),但这时**没有**
            // 「另一份在跑」,排队或走包装脚本都救不了。
            std::fprintf(stderr,
                         "[SL-324] %.*s: CreateMutexW(Local"
                         "\\SCVB-tests-proc) failed, GetLastError=%lu."
                         " This is NOT 'another test process is running' -- likely DACL denial,"
                         " handle exhaustion, or the name taken by a non-mutex object."
                         " SCVB test binaries use fixed IPC group ids whose segment names are"
                         " machine-wide, so they must not run without proven exclusivity."
                         "\n",
                         static_cast<int>(info.name.size()), info.name.data(), static_cast<unsigned long>(a.lastError));
        }
        else
        {
            std::fprintf(stderr,
                         "[SL-324] %.*s: another SCVB test process is already running on this"
                         " machine. All four test binaries (scvb_tests, scvb_monitor_tests,"
                         " scvb_host_tests, scvb_ipc_tests) share fixed IPC group ids g1-g8 whose"
                         " segment names are machine-wide, so two of them clobber each other's"
                         " segments -- including across different binaries."
                         "\n  Run it through the wrapper instead:"
                         " pwsh scripts/with-ipc-lock.ps1 -Command 'ctest --test-dir <builddir>"
                         " -C Release'"
                         "\n  (That wrapper takes gates' Local"
                         "\\SCVB-ipc-tests, NOT the mutex refused here -- with both runs behind"
                         " it they are serialised, so this -proc mutex is then free.)"
                         "\n  If nobody is running: look for a leftover scvb_*tests.exe --"
                         " a zombie keeps holding this mutex"
                         " (see the orphan scans around gate 6 in gates.ps1)."
                         "\n",
                         static_cast<int>(info.name.size()), info.name.data());
        }
        std::fflush(stderr);
        // 整轮不跑:再往下走就会建段,那正是要避免的加害。
        std::exit(2);
    }
};

} // namespace scvb::testsupport

CATCH_REGISTER_LISTENER(scvb::testsupport::TestsExclusiveListener);
