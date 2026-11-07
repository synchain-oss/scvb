// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 测试二进制的入口用:把环境变量 SCVB_IPC_LOCK_DIR 转成 SegmentBackendPosix 的测试覆盖。
// 生产代码不读这个环境变量(见 ipc/SegmentBackendPosix.h「测试钩子」);只有测试进程在自己的入口调这里,
// 父子进程继承环境的隔离方式因此不变。非 POSIX 后端的平台上什么都不做。

#include <cstdlib>

#if defined(SCVB_HAS_POSIX_SHM)
#include "ipc/SegmentBackendPosix.h"
#endif

namespace scvb::testsupport
{

inline void applyLockDirOverrideFromEnv()
{
#if defined(SCVB_HAS_POSIX_SHM)
    const char* v = std::getenv(scvb::SegmentBackendPosix::kLockDirEnvVar);
    if (v != nullptr && v[0] != '\0')
    {
        scvb::SegmentBackendPosix::setLockDirOverrideForTests(v);
    }
#endif
}

} // namespace scvb::testsupport
