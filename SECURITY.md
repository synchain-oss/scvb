# Security Policy

## Supported versions

| Version              | Supported |
| -------------------- | --------- |
| latest minor (x.Y.*) | ✅        |
| older                | ❌        |

只维护最新一个 minor 版本线(`x.Y.*`)。更老的版本不再回补安全修复,请先升级到最新 minor 再复现。

## Reporting a vulnerability

**Do not open a public issue.** 请走 GitHub Private Vulnerability Reporting:仓库页面 → Security → Report a vulnerability。

备用渠道(GitHub 通道不可用时):**contact@synchain.ca**。

报告里请尽量带上:SCVB 版本、DAW 与版本、Windows 版本、复现步骤,以及能触发问题的最小工程文件或 state 字节。**请不要在报告里附任何凭据**。

## Response targets

- 首次响应:3 个工作日内
- 修复或缓解方案:高危 14 天、中危 30 天
- 披露:修复发布后 7 天公开 advisory(GHSA)

## Scope

- ✅ 本仓库代码中的内存安全问题:越界读写、未初始化读、释放后使用、整数溢出导致的错误分配或索引。
- ✅ **state 反序列化**(`setStateInformation`)导致的崩溃、任意写或资源耗尽 —— 包括内嵌压缩特征流的解压路径。
- ✅ **IPC 共享内存段**的越权访问、跨会话串段、恶意或损坏的 header 字段导致的崩溃(段名前缀 `SynchainSCVB.v1.`)。
- ✅ **JS↔C++ 桥**(WebView2)的消息处理:来自 web 层的载荷导致 C++ 侧崩溃或越界;插件 UI 加载非预期来源的资源。
- ✅ 构建脚本 / CI 的供应链问题(依赖拉取、workflow 权限、secret 泄漏面)。
- ❌ JUCE / VST3 SDK / WebView2 Runtime 的上游问题 —— 请报给上游。
- ❌ 用户自行修改路由导致的音频异常(那是使用问题,见 `docs/USER_GUIDE.md` 与 `docs/DAW_COMPATIBILITY.md`)。
- ❌ 需要攻击者已经能在用户机器上任意执行代码才能达成的场景。

**特别提示**:`setStateInformation` 处理的是**用户工程文件里的不可信字节**,state 内可能内嵌 8MB 以内的压缩特征流。state 反序列化必须做长度 / 范围校验,任何 `abi` 或长度字段都不得直接用于分配或索引 —— 这条同时写在 [CLAUDE.md](./CLAUDE.md) §7 里,是代码评审的硬判据。
