# REAPER 真机矩阵

用 [reaper-harness](https://github.com/synchain-oss/reaper-harness) 驱动本机的 REAPER 7,测成品插件。结果写进 [DAW_COMPATIBILITY.md](../../docs/DAW_COMPATIBILITY.md) §2.2。

这套矩阵**只能在本机跑**,不进 CI:它要一个真的 REAPER 和桌面会话。

## 钉版

harness 钉在 `b5f9c484fa5f3ed602419bd756fb719e40f14f67`。单独 clone 一份切到这个提交,不要放进本仓。

REAPER 用 7.82,上面的结果都是这个版本测的。

## 怎么跑

`-CfgDir` 是 harness 的 `doctor.ps1 -Init` 建的私有配置目录,不会碰默认的 REAPER 配置。`-PluginDir` 放这次要测的三个 `.vst3`。

```powershell
$harness = "<harness 的 clone>"
$n = "<本仓>/tests/reaper/nested"
$args = @{ CfgDir = "<私有配置目录>"; PluginDir = "<放 .vst3 的目录>"; ResultDir = "<结果目录>" }

# 先跑 setup:设好每个实例、做两层分析,把工程存进 states/(不进仓库)
pwsh $harness/run.ps1 -Config $n/setup.json @args -NoNegative -KeepWork

# 再跑矩阵
pwsh $harness/run.ps1 -Config $n/nested.json @args -NoNegative -KeepWork
```

- 全程约 50 分钟。这段时间不要在本机编译或跑别的重活,实时渲染的结果对时序敏感。
- 退出码 2 是没有结论:通常是 REAPER 的授权提醒挡住了某一行。用 `-Only` 重跑那几行,比较对象也要带上,例如 `-Only '^(n11-device|w4-master-online|r-ref-pos)$'`。
- 机器上已经开着 REAPER 时,harness 不跑,退出码 3。

## 改矩阵

`setup.json` 和 `nested.json` 由 `nested/gen_nested.py` 生成,改生成器,不要改 JSON:

```powershell
python tests/reaper/nested/gen_nested.py
npx prettier@3.9.6 --write tests/reaper/nested
```

会话脚本在 `nested/session/`,REAPER 侧的扩展在 `nested/ext/`。
