// SPDX-License-Identifier: GPL-3.0-or-later
#include <DesignBox.h>

#include <array>

#include <catch2/catch_test_macros.hpp>

// DesignBox.h 由 scripts/gen-design-box.py 从 web/shared/design-box.js 生成(唯一真源)。
// 本测试把生成物与 05 §1.2 / design-box.js 的定值逐条对拍:任何人手工改坏 DesignBox.h
// 或改坏生成器输出,这里立即变红(配合 scripts/gen-design-box.py --check 的「源-产物」对拍)。
TEST_CASE("DesignBox.h matches design-box.js single source")
{
    CHECK(scvb::design::kOutputDesignW == 1180);
    CHECK(scvb::design::kOutputDesignH == 780);
    CHECK(scvb::design::kInputDesignW == 460);
    CHECK(scvb::design::kInputDesignH == 560);

    CHECK(scvb::design::kOutputPresetCount == 7);
    CHECK(scvb::design::kInputPresetCount == 10);

    CHECK(scvb::design::kOutputPresets == std::array<double, 7>{0.5, 0.65, 0.8, 1, 1.25, 1.5, 2});
    CHECK(scvb::design::kInputPresets == std::array<double, 10>{0.33, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3});
}
