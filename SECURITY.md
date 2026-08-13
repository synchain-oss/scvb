# Security Policy

## Supported versions

| Version              | Supported |
| -------------------- | --------- |
| latest minor (x.Y.*) | ✅        |
| older                | ❌        |

## Reporting a vulnerability

**Do not open a public issue.** Use GitHub Private Vulnerability Reporting:
Security → Report a vulnerability。

## Response targets

- 首次响应:3 个工作日内
- 修复或缓解方案:高危 14 天、中危 30 天
- 披露:修复发布后 7 天公开 advisory(GHSA)

## Scope

- ✅ 本仓库代码中的内存安全/越界/未初始化读、共享内存段的越权访问、状态反序列化(setStateInformation)导致的崩溃或任意写、构建脚本/CI 的供应链问题
- ❌ JUCE / VST3 SDK / WebView2 Runtime 上游问题(请报给上游)
- ❌ 用户自行修改路由导致的音频异常(那是使用问题,见 docs/USER_GUIDE.md)

**特别提示**:setStateInformation 处理的是用户工程文件里的不可信字节,state 内可能嵌压缩特征流。state 反序列化必须做长度/范围校验,任何 abi/长度字段都不能直接用于分配或索引。
