#include <catch2/catch_test_macros.hpp>

#include <string>

#include "ScvbCore.h"

// T01 冒烟:证明 scvb_core 可链接、可调用(ADR-011:scvb_core 全离线可测)。
TEST_CASE("scvb_core links and exposes version", "[smoke]")
{
    REQUIRE(scvb::coreVersion() != nullptr);
    REQUIRE(std::string(scvb::coreVersion()) == "0.1.0");
}
