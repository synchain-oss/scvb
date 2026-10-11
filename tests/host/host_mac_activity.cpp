// SPDX-License-Identifier: GPL-3.0-or-later
// host_mac_activity —— [B 线 M12b] 只在 macOS 上编进 scvb_host_tests(tests/CMakeLists.txt 的 if(APPLE) 追加块)。
//
// 整轮测试期间向系统声明一次「用户发起的活动」(juce::ScopedLowPowerModeDisabler =
// NSProcessInfo beginActivityWithOptions: NSActivityUserInitiatedAllowingIdleSystemSleep),让本进程不被
// App Nap 降频。
//
// 为什么要它:harness 用 ScopedJuceInitialiser_GUI 起了 NSApplication,却没有任何窗口 —— 在 runner 上它是一个
// 「看不见的 app」。M12b 首跑实测(macos-15 arm64 CI,同一个测试进程里):25Hz 的 juce::Timer 实得间隔
// p50 92 ms / p90 179 ms,runDispatchLoopUntil(1) 实得 p50 10 ms(Windows CI 同一格是 40.5 ms / 1.0 ms)。
// 用例大多是「泵消息直到某个 Timer 驱动的条件成立」,于是整套慢 2.4 倍(中位数),超出 900 s 上界。
// 宿主里的插件不在这种环境下:DAW 是前台 app,而且放音期间系统本来就不让它 nap。所以这里是把测试进程
// 拉回「像在宿主里」的调度条件,不改任何用例的判据与期望;Windows 上不编这个文件。
//
// 运行期文案一律 ASCII;中文只在注释里。

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <juce_events/juce_events.h>

#include <memory>

namespace
{

struct HostMacActivityListener : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override
    {
        disabler_ = std::make_unique<juce::ScopedLowPowerModeDisabler>();
    }

    void testRunEnded(Catch::TestRunStats const&) override { disabler_.reset(); }

private:
    std::unique_ptr<juce::ScopedLowPowerModeDisabler> disabler_;
};

} // namespace

CATCH_REGISTER_LISTENER(HostMacActivityListener)
