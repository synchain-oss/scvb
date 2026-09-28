// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <regex>
#include <string>

#include "ScvbCore.h"

namespace
{
// 顶层 CMakeLists.txt 里 project(SCVB ... VERSION x.y.z) 的 x.y.z;取不到返回空串。
// 与 scripts/lib/release-version.ps1 的 Get-ScvbCMakeVersion 同一判据:只认 project(SCVB ...)
// 那一行(第 1 行的 cmake_minimum_required(VERSION 3.22) 也含 VERSION),行首锚定,被 # 注释掉的不算。
std::string cmakeProjectVersion()
{
    std::ifstream in(std::string(SCVB_SOURCE_DIR) + "/CMakeLists.txt");
    if (!in)
        return {};
    const std::regex re(R"(^[ \t]*project[ \t]*\([ \t]*SCVB\b[^)]*?\bVERSION[ \t]+([0-9]+\.[0-9]+\.[0-9]+))");
    std::string line;
    while (std::getline(in, line))
    {
        std::smatch m;
        if (std::regex_search(line, m, re))
            return m[1].str();
    }
    return {};
}
} // namespace

// T01 冒烟:证明 scvb_core 可链接、可调用(ADR-011:scvb_core 全离线可测)。
// 版本串必须等于顶层 CMakeLists.txt 的 project(SCVB VERSION ...) —— 期望值从那一行现读,
// 不写字面量:此前这里写死 "0.1.0",CMake 升到 0.9.0 之后核心库仍报 0.1.0,而用例照样绿。
TEST_CASE("scvb_core links and exposes version", "[smoke]")
{
    REQUIRE(scvb::coreVersion() != nullptr);
    const std::string expected = cmakeProjectVersion();
    INFO("CMakeLists.txt = " << SCVB_SOURCE_DIR << "/CMakeLists.txt");
    // 先断「取到了」:取不到时下面那条会变成拿空串比,红的原因就不是版本不一致了。
    REQUIRE_FALSE(expected.empty());
    CHECK(std::string(scvb::coreVersion()) == expected);
}
