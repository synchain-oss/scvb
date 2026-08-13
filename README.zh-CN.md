# SCVB — Synchain Vocal Balancer

多轨人声自动声像与音量平衡(JUCE 8 / VST3)。

## 它解决什么问题

- **SCVB Input** —— 插在每条人声轨上,采集音频并经共享内存转发给总线插件,同时把人声轨直接输出静音(保住 DAW 依赖图排序)。
- **SCVB Output** —— 插在人声总线上,按时间线区间读取各轨数据,施加每轨 gain/pan 后求和替换总线输入。
- 自动分段响度平衡与等功率声像分配,最多 15 轨、2 个版本。
- 自动化参数面冻结(声明 123 / 宿主可见 124),其余全部走 state。

> **状态**:v1 开发中(仓库骨架阶段,T01)。尚未发布可安装版本。

## 系统要求

- Windows 10/11 x64 + 任一 VST3 DAW
- WebView2 Evergreen Runtime(编辑器 UI)

## 从源码构建

```powershell
git clone https://github.com/synchain-oss/scvb.git
cd scvb
pwsh scripts/build.ps1 -JucePath C:\path\to\JUCE
```

完整工具链见 [CLAUDE.md](./CLAUDE.md) §6;本地门禁 `pwsh scripts/gates.ps1`。

## 文档

- 架构与契约:`docs/`(PARAMETERS / IPC_CONTRACT / STATE_SCHEMA;正式内容由 T39a 蒸馏)
- 宪法原文只读副本:`docs/constitution/`

## 贡献

见 [CONTRIBUTING.md](./CONTRIBUTING.md)。所有 commit 必须 `git commit -s`(Signed-off-by)。

## 许可证

[GPL-3.0](./LICENSE)(JUCE / VST3 SDK 依赖声明见 [THIRD-PARTY-NOTICES.md](./THIRD-PARTY-NOTICES.md))。

## 相关项目

- [synchain-oss/synchain-bridge](https://github.com/synchain-oss/synchain-bridge) — 把 DAW 音频桥接到浏览器的 VST3 插件
- [synchain-oss/synchain-cli](https://github.com/synchain-oss/synchain-cli) — `@synchain/cli` 命令行客户端
