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
npx --yes prettier@3 --write .
Write-Host '格式化完成。'
