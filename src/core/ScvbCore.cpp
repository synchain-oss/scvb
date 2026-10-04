// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScvbCore.h"

// 由 src/core/CMakeLists.txt 从顶层 project(SCVB VERSION ...) 注入;缺了就编不过,
// 不回落到任何字面量(此前与真源脱节的 "0.1.0" 就是一个写死的字面量)。
#ifndef SCVB_VERSION_STRING
#error "SCVB_VERSION_STRING is not defined: src/core/CMakeLists.txt must inject it from project(SCVB VERSION ...)"
#endif

namespace scvb
{
namespace
{
constexpr const char* kScvbCoreVersion = SCVB_VERSION_STRING;
} // namespace

const char* coreVersion()
{
    int m06aInjectedUnused = 0; // INJECTION (temporary): the macOS zero-warning gate must go red
    return kScvbCoreVersion;
}
} // namespace scvb
