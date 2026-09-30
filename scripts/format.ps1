<#
.SYNOPSIS  一键修复格式:clang-format -i(C++ 源)+ prettier --write(web 等)。
.DESCRIPTION 与 gates.ps1 gate 2/3 对应的修复命令;版本纪律:clang-format 18.1.8(06 §3.3 / §5.1)。
.EXAMPLE    pwsh scripts/format.ps1
#>
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

Write-Host '== clang-format -i (18.1.8) =='
$files = git ls-files '*.h' '*.hpp' '*.cpp' '*.cc' | Where-Object { $_ -notmatch '^third_party/' }
if (-not $files) {
  Write-Host '未发现 C++ 源文件。'
}
else {
  $files | ForEach-Object { clang-format -i --style=file $_ }
  Write-Host ("已格式化 {0} 个 C++ 源文件。" -f @($files).Count)
}

Write-Host '== prettier --write =='
# 版本与 scripts/gates.ps1 Gate 3、.github/workflows/format.yml 的 prettier 步**三处同步**:
# 这是 gate 3 的配对写入器。写入器浮动而检查器钉死,一旦 npm 上出了 >3.9.6 的 3.x,
# 「跑 format.ps1 重排 → 跑 gates 判红 → 再跑 format.ps1 还是红」会形成没有出路的死循环,
# 与 clang-format 侧「18.1.8 单一版本贯穿检查与修复」的纪律一致。
npx --yes prettier@3.9.6 --write .
Write-Host '格式化完成。'
