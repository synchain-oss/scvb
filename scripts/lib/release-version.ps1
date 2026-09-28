# SPDX-License-Identifier: GPL-3.0-or-later
# 发布版本号的两条判据(dot-source 用):scripts/package.ps1 与 scripts/check-release-tag.ps1 共用,
# 别在调用方再抄一份。自测在 `check-release-tag.ps1 -SelfTest`。

# 版本真源 = 顶层 CMakeLists.txt 的 project(SCVB ... VERSION x.y.z)。
# **只认 project(SCVB ...) 那一行**:第 1 行 cmake_minimum_required(VERSION 3.22) 也含 VERSION,
# 「取第一条含 VERSION 的行」会拿到 3.22 —— release.yml 骨架与 #71 之前的 build.ps1 都栽在这里。
# 行首锚定,所以被 # 注释掉的 project(...) 不算。
function Get-ScvbCMakeVersion([string]$CMakeText) {
  $m = [regex]::Match($CMakeText, '(?m)^[ \t]*project[ \t]*\([ \t]*SCVB\b[^)]*?\bVERSION[ \t]+([0-9]+\.[0-9]+\.[0-9]+)')
  if (-not $m.Success) { return $null }
  return $m.Groups[1].Value
}

# tag 与版本真源的对应关系。返回 hashtable:Ok / Version(去掉 v 的整串)/ Core(x.y.z)/
# Kind(final | rc | test)/ Prerelease / Message。
#   vX.Y.Z        正式版,X.Y.Z 必须等于 CMake 版本
#   vX.Y.Z-rc.N   预发布,X.Y.Z 同样必须等于 CMake 版本(rc 不改 CMakeLists)
#   v0.0.0-test[.N] 流水线演练专用:只放行 0.0.0,不比对 CMake —— 为的是不改版本号就能走通
#                 构建 → 打包 → 草稿 Release 全程;限死 0.0.0 是为了让它不可能冒充一个真版本。
function Test-ScvbReleaseTag([string]$Tag, [string]$CMakeVersion) {
  $r = @{ Ok = $false; Version = $null; Core = $null; Kind = $null; Prerelease = $false; Message = '' }
  $m = [regex]::Match($Tag, '^v([0-9]+\.[0-9]+\.[0-9]+)(?:-(rc\.[0-9]+|test(?:\.[0-9]+)?))?\z')
  if (-not $m.Success) {
    $r.Message = "tag '$Tag' 形态不对:只接受 vX.Y.Z、vX.Y.Z-rc.N、v0.0.0-test[.N]"
    return $r
  }
  $r.Core = $m.Groups[1].Value
  $r.Version = $Tag.Substring(1)
  $pre = $m.Groups[2].Value
  if (-not $pre) { $r.Kind = 'final' }
  elseif ($pre.StartsWith('rc.')) { $r.Kind = 'rc'; $r.Prerelease = $true }
  else { $r.Kind = 'test'; $r.Prerelease = $true }

  if ($r.Kind -eq 'test') {
    if ($r.Core -ne '0.0.0') { $r.Message = "演练 tag 只允许 v0.0.0-test[.N],'$Tag' 不行"; return $r }
    $r.Ok = $true
    $r.Message = "演练 tag:不比对 CMake 版本(当前 $CMakeVersion),产物名按 tag 取"
    return $r
  }
  if (-not $CMakeVersion) { $r.Message = 'CMakeLists.txt 里没找到 project(SCVB ... VERSION x.y.z)'; return $r }
  if ($r.Core -ne $CMakeVersion) {
    $r.Message = "tag '$Tag' 的 $($r.Core) 与 CMakeLists.txt 的 project(SCVB VERSION $CMakeVersion) 不一致"
    return $r
  }
  $r.Ok = $true
  $r.Message = "tag '$Tag' 与 CMake 版本 $CMakeVersion 一致($($r.Kind))"
  return $r
}
