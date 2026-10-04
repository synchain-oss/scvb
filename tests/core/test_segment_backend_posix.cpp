// SPDX-License-Identifier: GPL-3.0-or-later
// test_segment_backend_posix —— POSIX 共享内存后端(B 线 M05)的用例落点。只在 Apple 上编进
// scvb_tests(tests/CMakeLists.txt 里那个 if(APPLE) 追加块)。M06a 只放一条最小用例把接缝跑通,
// 后端本体与生命周期用例由 M05 在这里填实。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

#include "ipc/SegmentLayout.h"

namespace
{
// macOS 对 shm_open 名字的长度上限(xnu 的 PSHMNAMLEN = 31,含开头的 '/',不含结尾 NUL)。
// 这个宏在内核私有头里,用户态拿不到,只能写常量;超出即 ENAMETOOLONG。
constexpr std::size_t kPosixShmNameMax = 31;

// 逻辑段名(不带 OS 前缀)→ POSIX 名:加 '/' 前缀。冻结的逻辑前缀只允许 ASCII。
std::string posixName(const std::wstring& logical)
{
    std::string s = "/";
    for (const wchar_t c : logical)
    {
        REQUIRE(static_cast<unsigned long>(c) < 0x80ul);
        s.push_back(static_cast<char>(c));
    }
    return s;
}
} // namespace

TEST_CASE("POSIX shm names: every logical segment name fits PSHMNAMLEN with the '/' prefix", "[posix][names]")
{
    std::size_t longest = 0;
    const auto consider = [&](const std::wstring& logical) { longest = std::max(longest, posixName(logical).size()); };

    for (scvb::u32 g = 1; g <= scvb::kMaxGroups; ++g)
    {
        consider(scvb::segmentLogicalName(g, scvb::SegmentKind::kRegistry));
        consider(scvb::segmentLogicalName(g, scvb::SegmentKind::kCtrl));
        consider(scvb::segmentLogicalName(g, scvb::SegmentKind::kViz));
        for (scvb::u32 ch = 1; ch <= scvb::kMaxChannels; ++ch)
        {
            consider(scvb::segmentLogicalName(g, scvb::SegmentKind::kAudio, ch));
            consider(scvb::segmentLogicalName(g, scvb::SegmentKind::kFeat, ch));
        }
    }

    // SegmentLayout.h 段名注释写的上界 "/SynchainSCVB.v1.g8.audio.ch15"(30 字符)确实是最长的那一档
    // (两位数声道的 audio / feat 名都与它等长,不唯一,所以比长度而不比名字)。
    const std::string documentedLongest = posixName(scvb::segmentLogicalName(8, scvb::SegmentKind::kAudio, 15));
    INFO("documented longest: " << documentedLongest << " (" << documentedLongest.size() << " chars)");
    CHECK(documentedLongest.size() == longest);
    REQUIRE(longest <= kPosixShmNameMax);
}
