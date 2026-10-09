#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/package-macos.sh —— SCVB macOS 发行打包的唯一真源(CI 冒烟与发版共用这一份;[B 线 M10])。
# 与 scripts/package.ps1 并列:Windows 包走 .ps1,macOS 包走本脚本。六条硬要求(06 §3.8,照 Bridge 仓同名脚本)
# 在这里的落点:
#   1. zip 名由版本号算出:SCVB-v<ver>-macos-arm64.zip。版本缺省读根 CMakeLists.txt 的 project(SCVB VERSION),
#      与 Windows 同一个真源;不写字面量。
#   2. 按目录枚举 bundle,断言**恰好** 3 个 .vst3 + 3 个 .component,名字恰好是 SCVB Input / Output / Monitor,
#      层级保住 <name>.<ext>/Contents/...;每个 bundle 里的**每个** Mach-O 都只有 arm64 一个架构。
#   3. .sha256 是独立文件,格式与 package.ps1 逐字一致(`<小写 hex><两个空格><zip 名>` + LF);
#      另出 package-summary-macos.md(与 Windows 的 package-summary.md 同一个表头,文件名带平台,两边能放进同一个 Release)。
#   4. 生成 INSTALL.txt(中英):校验、解压、升级先删旧 bundle、ditto 安装、xattr、AU 重新登记与宿主重扫、可选 auval、
#      系统与宿主要求、九条使用规则的前 3 条、精确到 tag 的源码声明。
#   5. zip 根目录带 LICENSE.txt / THIRD-PARTY-NOTICES.md / LICENSES/ / third_party/notices/,与 Windows 包同一批文件。
#   6. 打包后断言:zip 条目清单(根目录白名单、6 个 bundle、合规文件、NOTICES 点名的每个路径)、Contents/MacOS 可执行位、
#      解压回来逐文件比对字节、源码声明行;.sha256 用 INSTALL.txt 里教用户的那条 `shasum -a 256 -c` 自验。
# **合规判据只有一份**:本脚本调 `pwsh scripts/package.ps1 -Preflight -PreflightOut <清单>`,许可证全文覆盖、
# NOTICES 点名的声明文件、要打进 zip 的 LICENSES/ 与 third_party/notices/ 文件、九条规则前 3 条的原文
# (取自用户手册的生成区,与 Windows INSTALL.txt 同一个解析器)都从那份清单读,这里不重写第二套。所以本脚本需要 pwsh。
# mac 专属:
#   - 拷贝与压缩一律用 ditto(cp -r / zip -r 会丢符号链接与可执行位,用户解压后拿到加载不了的死壳);
#     压缩带 --norsrc --noextattr --noacl,不产生 __MACOSX/ 条目。
#   - 确定性:暂存区里每个文件与目录的时间戳统一取 SOURCE_DATE_EPOCH / HEAD 提交时间(都取不到才用 1980-01-01),
#     合规文件与 INSTALL.txt 的权限统一 0644,所以同一份输入重跑,zip 的 sha256 不变(CI 冒烟连跑三次核这一条)。
# 只用 macOS 自带的 bash 3.2 与 BSD 工具能跑通的写法:不用关联数组 / mapfile,不展开可能为空的数组,
# sed 只用 -E 与 POSIX 字符类,`find | head` 一律不用(pipefail 下 SIGPIPE)。
# 绝不在 workflow 里内联打包命令 —— 打包逻辑只在此处。
#
# 用法:
#   bash scripts/package-macos.sh [--version X.Y.Z[-pre]] [--build-dir build] [--out-dir dist]
#                                 [--source-commit <sha>] [--allow-missing-license-texts] [--dry-run]

set -euo pipefail

die() {
    echo "package-macos: $*" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
usage: package-macos.sh [options]
  --version <X.Y.Z[-pre]>        version without the leading v; omitted = read project(SCVB VERSION) from CMakeLists.txt
  --build-dir <path>             directory holding the 3 .vst3 + 3 .component bundles (searched recursively); default build
  --out-dir <path>               where the zip, .sha256 and package-summary-macos.md go; default dist
  --source-commit <sha>          recorded in INSTALL.txt / summary; default = git HEAD of this repo
  --allow-missing-license-texts  pipeline-test tags only (v0.0.0-test): passed through to package.ps1 -Preflight
  --dry-run                      resolve the version, run the compliance preflight, print the plan; build nothing
Relative paths are resolved against the current directory (same as scripts/package.ps1).
USAGE
}

VERSION=""
VERSION_GIVEN=0
BUILD_DIR="build"
OUT_DIR="dist"
SOURCE_COMMIT=""
ALLOW_MISSING=0
DRY_RUN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --version)       [ $# -ge 2 ] || die "--version needs a value";       VERSION="$2"; VERSION_GIVEN=1; shift 2 ;;
        --build-dir)     [ $# -ge 2 ] || die "--build-dir needs a value";     BUILD_DIR="$2"; shift 2 ;;
        --out-dir)       [ $# -ge 2 ] || die "--out-dir needs a value";       OUT_DIR="$2"; shift 2 ;;
        --source-commit) [ $# -ge 2 ] || die "--source-commit needs a value"; SOURCE_COMMIT="$2"; shift 2 ;;
        --allow-missing-license-texts) ALLOW_MISSING=1; shift ;;
        --dry-run)       DRY_RUN=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               usage >&2; die "unknown argument: $1" ;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"

abs_path() {
    case "$1" in
        /*) printf '%s' "$1" ;;
        *)  printf '%s/%s' "$PWD" "$1" ;;
    esac
}
BUILD_DIR="$(abs_path "$BUILD_DIR")"
OUT_DIR="$(abs_path "$OUT_DIR")"

# 三个插件(Input / Output 必装,Monitor 可选)× 两种格式。名字与 package.ps1 的 $ExpectedBundles 同一组。
ROLES="Input Output Monitor"
ROLES_BRACE="Input,Output,Monitor"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/scvb-package-macos.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# ── 1) 版本 ────────────────────────────────────────────────────────────────────────────────────
# 参数优先;**没传** --version 才从 CMakeLists.txt 回落。传了 --version "" 属于调用方没算出版本号,
# 静默回落会产出版本对不上的资产(Bridge 仓踩过:演练 tag 产出了正式版号的 zip),所以直接 die。
# 回落用单条 BSD / GNU 两边都通的 sed:先丢注释行;行首锚定(被 # 注释掉的 project(...) 不算);
# 只认 project(SCVB ... VERSION x.y.z) —— 与 scripts/lib/release-version.ps1 的 Get-ScvbCMakeVersion 同口径,
# 取到的值下面还要与 package.ps1 读到的 cmakeVersion 对拍,两个解析器不一致当场红。
# `/re/{s//\2/p;q;}`:命中即打印并退出,不接 `| head`(pipefail 下 SIGPIPE);`q;}` 的分号是 BSD sed 的硬要求。
VERSION_FROM="--version"
if [ "$VERSION_GIVEN" -eq 1 ] && [ -z "$VERSION" ]; then
    die "--version was given an empty string: the caller failed to compute the version (not falling back to CMakeLists.txt)"
fi
if [ -z "$VERSION" ]; then
    version_re='^[[:space:]]*project[[:space:]]*\([[:space:]]*SCVB[[:space:]]([^)]*[[:space:]])?VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+)([^0-9].*)?$'
    VERSION="$(sed -nE '/^[[:space:]]*#/d; /'"$version_re"'/{s//\2/p;q;}' "$REPO_ROOT/CMakeLists.txt")"
    [ -n "$VERSION" ] || die "cannot read project(SCVB VERSION x.y.z) from $REPO_ROOT/CMakeLists.txt"
    VERSION_FROM="CMakeLists.txt"
fi
VERSION="${VERSION#v}"

if [ -z "$SOURCE_COMMIT" ]; then
    SOURCE_COMMIT="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || true)"
fi

# ── 2) 合规 preflight(唯一真源 = scripts/package.ps1)──────────────────────────────────────────
command -v pwsh >/dev/null 2>&1 \
    || die "pwsh not found: the compliance criteria and the INSTALL.txt rule texts live in scripts/package.ps1 -Preflight"
MANIFEST="$WORK/preflight.tsv"
set -- -NoProfile -File "$REPO_ROOT/scripts/package.ps1" -Preflight -Version "$VERSION" -PreflightOut "$MANIFEST"
if [ -n "$SOURCE_COMMIT" ]; then set -- "$@" -SourceCommit "$SOURCE_COMMIT"; fi
if [ "$ALLOW_MISSING" -eq 1 ]; then set -- "$@" -AllowMissingLicenseTexts; fi
pwsh "$@" || die "scripts/package.ps1 -Preflight failed (see its output above)"
[ -s "$MANIFEST" ] || die "package.ps1 -Preflight wrote no manifest"

# 清单:每行 `键<TAB>值`,同名键按出现顺序累积。按第一个 TAB 切,值里的内容原样保留。
mf_all() {
    awk -v k="$1" '{ i = index($0, "\t"); if (i > 0 && substr($0, 1, i - 1) == k) print substr($0, i + 1) }' "$MANIFEST"
}
mf_one() {
    local n v
    n="$(mf_all "$1" | wc -l | tr -d '[:space:]')"
    [ "$n" = "1" ] || die "preflight manifest: expected exactly one '$1' line, found $n"
    v="$(mf_all "$1")"
    printf '%s' "$v"
}
[ "$(mf_one format)" = "scvb-package-preflight/1" ] \
    || die "preflight manifest format is '$(mf_one format)', this script only understands scvb-package-preflight/1"
MF_VERSION="$(mf_one version)"
[ "$MF_VERSION" = "$VERSION" ] || die "package.ps1 reports version '$MF_VERSION', this script asked for '$VERSION'"
CMAKE_VERSION="$(mf_one cmakeVersion)"
if [ "$VERSION_FROM" = "CMakeLists.txt" ] && [ "$VERSION" != "$CMAKE_VERSION" ]; then
    die "the sed fallback read version '$VERSION' from CMakeLists.txt but package.ps1 reads '$CMAKE_VERSION': the two parsers disagree"
fi
TAG="$(mf_one tag)"
SOURCE_LINE="$(mf_one sourceLine)"
SOURCE_URL="$(mf_one sourceUrl)"
GUIDE_EN="$(mf_one guideEn)"
GUIDE_ZH="$(mf_one guideZh)"
ISSUES_URL="$(mf_one issuesUrl)"
mf_all ruleEn > "$WORK/rules-en.txt"
mf_all ruleZh > "$WORK/rules-zh.txt"
mf_all license > "$WORK/license.txt"
mf_all notice > "$WORK/notice.txt"
mf_all cited > "$WORK/cited.txt"
mf_all missingLicenseText > "$WORK/missing.txt"
for f in rules-en rules-zh; do
    n="$(wc -l < "$WORK/$f.txt" | tr -d '[:space:]')"
    [ "$n" = "3" ] || die "preflight manifest: expected 3 lines of $f, found $n"
done
for f in license notice cited; do
    [ -s "$WORK/$f.txt" ] || die "preflight manifest lists no '$f' entries"
done
# 清单里的路径要拼进暂存区:只放行仓库内的相对路径。
while IFS= read -r rel <&3; do
    case "$rel" in
        /*|*..*|'') die "preflight manifest has an unexpected path: '$rel'" ;;
    esac
    [ -f "$REPO_ROOT/$rel" ] || die "preflight manifest lists '$rel', which is not a file in the repo"
done 3< <(cat "$WORK/license.txt" "$WORK/notice.txt")

ZIP_NAME="SCVB-v$VERSION-macos-arm64.zip"
SHA_NAME="$ZIP_NAME.sha256"
SUMMARY_NAME="package-summary-macos.md"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "[DryRun] RepoRoot : $REPO_ROOT"
    echo "[DryRun] Version  : $VERSION (from $VERSION_FROM; CMakeLists.txt project(SCVB VERSION) = $CMAKE_VERSION)"
    echo "[DryRun] Tag      : $TAG"
    echo "[DryRun] BuildDir : $BUILD_DIR"
    echo "[DryRun] OutDir   : $OUT_DIR"
    echo "[DryRun] zip      : $OUT_DIR/$ZIP_NAME"
    echo "[DryRun] sha256   : $OUT_DIR/$SHA_NAME"
    echo "[DryRun] summary  : $OUT_DIR/$SUMMARY_NAME"
    echo "[DryRun] compliance: $(wc -l < "$WORK/license.txt" | tr -d '[:space:]') LICENSES/ files, $(wc -l < "$WORK/notice.txt" | tr -d '[:space:]') third_party/notices/ files, $(wc -l < "$WORK/cited.txt" | tr -d '[:space:]') cited notice paths (checked by package.ps1 -Preflight)"
    echo "[DryRun] OK - nothing was built or written."
    exit 0
fi

# ── 3) 工具与输入 ──────────────────────────────────────────────────────────────────────────────
# PlistBuddy 不在 PATH 上(/usr/libexec);SCVB_PLISTBUDDY 只给本机脚本自测换桩用,CI 与发版不设。
PLISTBUDDY="${SCVB_PLISTBUDDY:-/usr/libexec/PlistBuddy}"
for tool in ditto lipo file otool unzip shasum "$PLISTBUDDY"; do
    command -v "$tool" >/dev/null 2>&1 || die "required macOS tool not found: $tool"
done
[ -d "$BUILD_DIR" ] || die "build dir not found: $BUILD_DIR"

# 输出目录里同名的旧产物先删:这一次中途红了,不能留下上一次的 zip 冒充这一次的结果。
# 新产物先在 $WORK 里做完全部断言,最后才挪进 OUT_DIR。
rm -f "$OUT_DIR/$ZIP_NAME" "$OUT_DIR/$SHA_NAME" "$OUT_DIR/$SUMMARY_NAME"

# ── 4) 枚举 bundle(硬要求 2)────────────────────────────────────────────────────────────────────
# 跳过 FetchContent 的 _deps/;命中一个 bundle 就不再往里找(-prune),bundle 里不该再套 bundle。
find "$BUILD_DIR" -name _deps -prune -o -type d \( -name '*.vst3' -o -name '*.component' \) -print -prune \
    > "$WORK/found.txt"
n_vst3="$(grep -c '\.vst3$' "$WORK/found.txt" || true)"
n_comp="$(grep -c '\.component$' "$WORK/found.txt" || true)"
if [ "$n_vst3" != "3" ]; then
    sed 's/^/  found: /' "$WORK/found.txt" >&2
    die "expected exactly 3 .vst3 bundles (SCVB Input / Output / Monitor) under '$BUILD_DIR', found $n_vst3"
fi
if [ "$n_comp" != "3" ]; then
    sed 's/^/  found: /' "$WORK/found.txt" >&2
    die "expected exactly 3 .component bundles (SCVB Input / Output / Monitor) under '$BUILD_DIR', found $n_comp"
fi
while IFS= read -r p <&3; do basename "$p"; done 3< "$WORK/found.txt" | LC_ALL=C sort > "$WORK/names.txt"
for r in $ROLES; do printf 'SCVB %s.vst3\nSCVB %s.component\n' "$r" "$r"; done | LC_ALL=C sort > "$WORK/expected-names.txt"
if ! cmp -s "$WORK/names.txt" "$WORK/expected-names.txt"; then
    sed 's/^/  found: /' "$WORK/names.txt" >&2
    sed 's/^/  expected: /' "$WORK/expected-names.txt" >&2
    die "bundle names under '$BUILD_DIR' are not exactly the 6 expected ones"
fi

# 名字 → 路径(名字已核过唯一)。
bundle_path() {
    awk -v n="$1" '{ k = split($0, a, "/"); if (a[k] == n) print }' "$WORK/found.txt"
}

# 每个 bundle:Contents/Info.plist、Contents/MacOS/<名字> 可执行;bundle 里**每个** Mach-O 都只有 arm64
# (带 x86_64 的胖二进制或纯 x86_64 都红 —— 只支持 Apple Silicon 是 INSTALL.txt 写给用户的承诺)。
: > "$WORK/minos.txt"
while IFS= read -r b <&3; do
    name="$(basename "$b")"
    stem="${name%.*}"
    [ -f "$b/Contents/Info.plist" ] || die "'$b' has no Contents/Info.plist"
    exe="$b/Contents/MacOS/$stem"
    [ -f "$exe" ] && [ -x "$exe" ] || die "'$exe' is missing or not executable"
    case "$(file -b "$exe")" in
        *Mach-O*) ;;
        *) die "'$exe' is not a Mach-O file: $(file -b "$exe")" ;;
    esac
    nmacho=0
    while IFS= read -r -d '' f; do
        case "$(file -b "$f")" in
            *Mach-O*) ;;
            *) continue ;;
        esac
        archs="$(lipo -archs "$f")"
        [ "$archs" = "arm64" ] || die "'$f' has architectures '$archs'; this package is arm64-only"
        nmacho=$((nmacho + 1))
    done < <(find "$b" -type f -print0)
    [ "$nmacho" -ge 1 ] || die "'$b' contains no Mach-O file"
    otool -l "$exe" > "$WORK/otool.txt"
    minos="$(awk '/LC_BUILD_VERSION/ { f = 1 } f && $1 == "minos" { print $2; exit }' "$WORK/otool.txt")"
    printf '%s\t%s\n' "$name" "${minos:-?}" >> "$WORK/minos.txt"
    echo "bundle ok: $name ($nmacho Mach-O file(s), arm64, minos ${minos:-?})"
done 3< "$WORK/found.txt"

# AU 三元组从 bundle 自己的 Info.plist 读(INSTALL.txt 里的 auval 命令按它生成,写的就是包里那一份);
# 每个 .component 恰好声明一个 AudioComponent。冻结值的核对在 CI 的 auval 门(build-macos.yml 11c)。
: > "$WORK/au.txt"
for r in $ROLES; do
    plist="$(bundle_path "SCVB $r.component")/Contents/Info.plist"
    pb() { "$PLISTBUDDY" -c "Print :AudioComponents:$1" "$plist" 2>/dev/null || true; }
    t="$(pb 0:type)"; s="$(pb 0:subtype)"; m="$(pb 0:manufacturer)"
    [ -n "$t" ] && [ -n "$s" ] && [ -n "$m" ] || die "cannot read AudioComponents[0] type/subtype/manufacturer from $plist"
    [ -z "$(pb 1:type)" ] || die "$plist declares more than one AudioComponent"
    printf '%s %s %s\n' "$t" "$s" "$m" >> "$WORK/au.txt"
done

# ── 5) 暂存区(硬要求 5)──────────────────────────────────────────────────────────────────────────
STAGE="$WORK/stage"
mkdir "$STAGE"
while IFS= read -r b <&3; do
    ditto --norsrc --noextattr --noacl "$b" "$STAGE/$(basename "$b")"
done 3< "$WORK/found.txt"
cp "$REPO_ROOT/LICENSE" "$STAGE/LICENSE.txt"
cp "$REPO_ROOT/THIRD-PARTY-NOTICES.md" "$STAGE/THIRD-PARTY-NOTICES.md"
while IFS= read -r rel <&3; do
    mkdir -p "$STAGE/$(dirname "$rel")"
    cp "$REPO_ROOT/$rel" "$STAGE/$rel"
done 3< <(cat "$WORK/license.txt" "$WORK/notice.txt")

# ── 6) INSTALL.txt(硬要求 4)─────────────────────────────────────────────────────────────────────
# 命令行(缩进 7 格的行)只用 ASCII,中英两段的命令逐字相同 —— 用户直接复制进终端(macOS 默认 zsh,bash 同样成立)。
# 九条规则的前 3 条来自 package.ps1 的清单(用户手册生成区的原文),不在这里抄。
# 下面是不加引号的 heredoc:要原样留给用户的 `$` 写成 `\$`;`\ ` 在 heredoc 里本来就原样保留。
RULES_EN_BLOCK="$(awk '{ printf "  %d. %s\n", NR, $0 }' "$WORK/rules-en.txt")"
RULES_ZH_BLOCK="$(awk '{ printf "  %d. %s\n", NR, $0 }' "$WORK/rules-zh.txt")"
AUVAL_BLOCK="$(awk '{ printf "       auval -v %s %s %s\n", $1, $2, $3 }' "$WORK/au.txt")"
COMMIT_LINE=""
if [ -n "$SOURCE_COMMIT" ]; then COMMIT_LINE="Built from commit: $SOURCE_COMMIT"; fi
# 解压目录与 zip 同名(去掉 .zip):每个版本一个新目录。ditto -x -k 往已有目录里是合并、不删多余文件,
# 固定叫 SCVB 的话,上一个版本解压留下的文件会被第 4 步一起复制进插件目录。
UNPACK_DIR="${ZIP_NAME%.zip}"
VST3_DIR="~/Library/Audio/Plug-Ins/VST3"
AU_DIR="~/Library/Audio/Plug-Ins/Components"

cat > "$STAGE/INSTALL.txt" <<EOF
SCVB (Synchain Vocal Balancer) $VERSION - macOS, Apple Silicon (arm64) - VST3 + Audio Unit - beta

$SOURCE_LINE
$COMMIT_LINE

== English ==

This macOS build is a beta: it is not code-signed or notarized, and it has not been tested in
Logic Pro (beta / not tested in Logic). Please report problems via the Issues link at the end.

What is in this zip
  SCVB Input.vst3    SCVB Input.component     required - goes on every vocal track
  SCVB Output.vst3   SCVB Output.component    required - goes on the vocal bus
  SCVB Monitor.vst3  SCVB Monitor.component   optional - read-only window for watching a whole group;
                                              install it only if you want it
  LICENSE.txt, THIRD-PARTY-NOTICES.md, LICENSES/   licence texts
  third_party/notices/   original copyright and licence notices of the third-party code built into the plugins
  INSTALL.txt        this file
Each plugin comes in two formats: .vst3 (VST3) and .component (Audio Unit). Install the format your
host uses; installing both is fine. Input and Output are a pair and share one version number: install
both from the same zip.

Requirements
  - A Mac with Apple Silicon (M1 or later). Intel Macs are not supported.
  - The host must run natively on Apple Silicon. Hosts running under Rosetta are not supported: the
    plugins contain arm64 code only, so a host running under Rosetta does not list them. In Finder,
    select the host app, choose File > Get Info and untick "Open using Rosetta".
  - macOS 11 Big Sur or later, with the system Safari / WebKit 15.4 or later (the plugin window uses
    the system WebKit). With a system Safari older than 26.4 the plugin window's zoom stays at 100%.
  - A VST3 or Audio Unit host. GarageBand is not supported.
  - Logic Pro: beta / not tested in Logic. Logic loads the Audio Unit (.component) only.

Install (in Terminal)
  1. Check the download. Browsers usually save it in ~/Downloads:
       cd ~/Downloads
     If you downloaded $SHA_NAME from the GitHub Release as well, keep it next
     to the zip and run:
       shasum -a 256 -c $SHA_NAME
     It must print "$ZIP_NAME: OK". Without the .sha256 file, run
       shasum -a 256 $ZIP_NAME
     and compare the result with the SHA-256 on the download page or in the GitHub Release notes,
     character for character. If it does not match, delete the zip and do not install it.
  2. Extract the zip into a new folder named after it and go into it (the next steps run from there;
     a fresh folder per version, so files left over from an older version are never copied along):
       ditto -x -k $ZIP_NAME $UNPACK_DIR && cd $UNPACK_DIR
  3. Updating? Delete the old bundles first, so old and new files never mix (harmless on a first
     install):
       rm -rf $VST3_DIR/SCVB\ {$ROLES_BRACE}.vst3
       rm -rf $AU_DIR/SCVB\ {$ROLES_BRACE}.component
  4. Copy the six bundles with ditto (it keeps the executable bits; do not use cp -r):
       for b in SCVB\ *.vst3; do ditto "\$b" $VST3_DIR/"\$b"; done
       for b in SCVB\ *.component; do ditto "\$b" $AU_DIR/"\$b"; done
     Monitor is optional: if you do not want it, delete SCVB Monitor.vst3 and SCVB Monitor.component
     from the extracted folder before this step.
  5. Clear the quarantine flag. SCVB is not code-signed or notarized; skip this and your host refuses
     to load it:
       xattr -dr com.apple.quarantine $VST3_DIR/SCVB\ *.vst3
       xattr -dr com.apple.quarantine $AU_DIR/SCVB\ *.component
  6. Make macOS register the Audio Units again, then rescan plugins in your host:
       killall -9 AudioComponentRegistrar
     "No matching processes" only means it was not running; you can ignore it. Do this after every
     update as well: the AU version number stays the same between betas, so a host may otherwise keep
     its cached entry.
     LUNA: Audio Units are enabled by default. Open the Plug-In Manager and run Scan New (or Rescan
     All); SCVB Input, Output and Monitor must show as "scanned", not "rejected". VST3 is not enabled
     by default in LUNA on macOS: to use the .vst3 bundles, enable VST3 in the Plug-In Manager first,
     then rescan.
  7. (Optional) Validate the Audio Units (Input, Output, Monitor). Each run must end with
     "AU VALIDATION SUCCEEDED":
$AUVAL_BLOCK
     If you did not install Monitor, skip the last line (Scvm): without it that check can only fail.

Installing for every user of this Mac: in steps 3-5 use /Library/Audio/Plug-Ins/VST3 and
/Library/Audio/Plug-Ins/Components instead of the ~/Library/... folders, and put sudo in front of
each rm, ditto and xattr command (administrator password required).

Read before first use - the first 3 of the nine usage rules (the plugin shows all nine on first
launch; breaking any of them gives silence, wrong panning or failed analysis):
$RULES_EN_BLOCK
All nine rules: $GUIDE_EN

Licence: SCVB is free software under the GNU GPL v3 or later (LICENSE.txt).
Third-party components and their licences: THIRD-PARTY-NOTICES.md, LICENSES/ and third_party/notices/.
Issues: $ISSUES_URL

== 中文 ==

这是 macOS 版 beta:没有代码签名、没有公证,也未在 Logic Pro 实测(beta / 未在 Logic 实测)。
遇到问题请到文末的问题反馈链接提出。

zip 里有什么
  SCVB Input.vst3    SCVB Input.component     必装 —— 插在每条人声轨上
  SCVB Output.vst3   SCVB Output.component    必装 —— 插在人声总线上
  SCVB Monitor.vst3  SCVB Monitor.component   可选 —— 只读的整组观察窗,需要才装
  LICENSE.txt、THIRD-PARTY-NOTICES.md、LICENSES/   许可证全文
  third_party/notices/   编进插件的第三方代码的上游版权与许可声明原文
  INSTALL.txt        本文件
每个插件都有两种格式:.vst3(VST3)与 .component(Audio Unit)。装宿主用的那一种即可,两种都装也没问题。
Input 与 Output 是一对,共用一个版本号,请从同一个 zip 里一起安装。

系统要求
  - Apple Silicon(M1 及以后)的 Mac。不支持 Intel Mac。
  - 宿主必须以 Apple Silicon 原生方式运行。不支持在 Rosetta 下运行的宿主:插件只含 arm64 代码,
    在 Rosetta 下运行的宿主不会列出它们。在「访达」里选中宿主程序 > 文件 > 显示简介,取消勾选「使用 Rosetta 打开」。
  - macOS 11 Big Sur 或更高版本,且系统 Safari / WebKit 为 15.4 或更高(插件界面用的是系统 WebKit)。
    系统 Safari 低于 26.4 时,插件界面的缩放固定在 100%。
  - 支持 VST3 或 Audio Unit 的宿主。不支持 GarageBand。
  - Logic Pro:beta / 未在 Logic 实测。Logic 只加载 Audio Unit(.component)。

安装(在「终端」里执行)
  1. 校验下载。浏览器一般把它存在 ~/Downloads:
       cd ~/Downloads
     如果也从 GitHub Release 下载了 ${SHA_NAME},把它和 zip 放在一起,然后运行:
       shasum -a 256 -c $SHA_NAME
     必须输出「${ZIP_NAME}: OK」。没有 .sha256 文件时运行
       shasum -a 256 $ZIP_NAME
     把结果与下载页或 GitHub Release 正文里的 SHA-256 逐字核对。对不上就删掉 zip,不要安装。
  2. 把 zip 解压到与它同名的新文件夹并进入它(后面几步都在这里执行;每个版本一个新文件夹,旧版本留下的文件
     不会被一起复制过去):
       ditto -x -k $ZIP_NAME $UNPACK_DIR && cd $UNPACK_DIR
  3. 升级时先删掉旧 bundle,免得新旧文件混在一起(首次安装时这两行也无害):
       rm -rf $VST3_DIR/SCVB\ {$ROLES_BRACE}.vst3
       rm -rf $AU_DIR/SCVB\ {$ROLES_BRACE}.component
  4. 用 ditto 复制六个 bundle(ditto 会保住可执行位;不要用 cp -r):
       for b in SCVB\ *.vst3; do ditto "\$b" $VST3_DIR/"\$b"; done
       for b in SCVB\ *.component; do ditto "\$b" $AU_DIR/"\$b"; done
     Monitor 是可选的:不需要的话,在这一步之前把解压出来的文件夹里的 SCVB Monitor.vst3 与
     SCVB Monitor.component 删掉。
  5. 去掉隔离属性。SCVB 没有代码签名、没有公证,不做这一步宿主会拒绝加载:
       xattr -dr com.apple.quarantine $VST3_DIR/SCVB\ *.vst3
       xattr -dr com.apple.quarantine $AU_DIR/SCVB\ *.component
  6. 让 macOS 重新登记 Audio Unit,再在宿主里重新扫描插件:
       killall -9 AudioComponentRegistrar
     提示 No matching processes 说明它本来就没在运行,可以忽略。每次升级后也要做这一步:各个 beta 之间
     AU 的版本号不变,不做的话宿主可能继续用缓存里的旧条目。
     LUNA:Audio Unit 默认启用。打开 Plug-In Manager,执行 Scan New(或 Rescan All);SCVB Input、
     Output、Monitor 的状态必须是 scanned,不能是 rejected。macOS 上的 LUNA 默认不启用 VST3:要用 .vst3,
     先在 Plug-In Manager 里启用 VST3,再重新扫描。
  7. (可选)验证三个 Audio Unit(依次是 Input、Output、Monitor),每条都应以 AU VALIDATION SUCCEEDED 结尾:
$AUVAL_BLOCK
     没装 Monitor 的话跳过最后一条(Scvm):没装它,那一条只会报错。

给这台 Mac 的所有用户安装:第 3-5 步里的 ~/Library/... 换成 /Library/Audio/Plug-Ins/VST3 与
/Library/Audio/Plug-Ins/Components,并在每条 rm、ditto、xattr 命令前加 sudo(需要输入管理员密码)。

首次使用前必读 —— 九条使用规则的前 3 条(插件首次启动会完整弹出九条;违反任何一条都会导致静音、
声像位置错误或分析失效):
$RULES_ZH_BLOCK
完整九条:${GUIDE_ZH}

许可证:SCVB 以 GNU GPL v3 或更高版本发布(LICENSE.txt);第三方组件及其许可证见
THIRD-PARTY-NOTICES.md、LICENSES/ 与 third_party/notices/。
问题反馈:${ISSUES_URL}
EOF

# 命令行(缩进恰好 7 格)必须是纯 ASCII:中文标点、弯引号混进命令,用户复制进终端就是一条报错。
NON_ASCII_CMDS="$(LC_ALL=C grep -n '^       [^ ]' "$STAGE/INSTALL.txt" | LC_ALL=C grep '[^ -~]' || true)"
[ -z "$NON_ASCII_CMDS" ] || die "INSTALL.txt has non-ASCII characters on command lines:
$NON_ASCII_CMDS"
grep -Fqx -- "$SOURCE_LINE" "$STAGE/INSTALL.txt" || die "INSTALL.txt lacks the exact-tag source line: $SOURCE_LINE"

# ── 7) 确定性:权限与时间戳 ──────────────────────────────────────────────────────────────────────
EPOCH="${SOURCE_DATE_EPOCH:-}"
case "$EPOCH" in ''|*[!0-9]*) EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct 2>/dev/null || true)" ;; esac
case "$EPOCH" in ''|*[!0-9]*) EPOCH=315532800 ;; esac
TOUCH_TS="$(date -u -r "$EPOCH" '+%Y%m%d%H%M.%S' 2>/dev/null || date -u -d "@$EPOCH" '+%Y%m%d%H%M.%S')"
# zip 里的 DOS 时间是「本地时间」:暂存与压缩都钉在 UTC,换台机器、换个时区,条目时间不变。
export TZ=UTC
chmod 0644 "$STAGE/LICENSE.txt" "$STAGE/THIRD-PARTY-NOTICES.md" "$STAGE/INSTALL.txt"
while IFS= read -r rel <&3; do chmod 0644 "$STAGE/$rel"; done 3< <(cat "$WORK/license.txt" "$WORK/notice.txt")
find "$STAGE" -mindepth 1 -type d ! -path '*.vst3*' ! -path '*.component*' -exec chmod 0755 {} +
find "$STAGE" -exec touch -h -t "$TOUCH_TS" {} +

# ── 8) 压缩:ditto -c -k,不加 --keepParent(暂存区内容平铺到 zip 根,与 package.ps1 一致)───────────────
ZIP_W="$WORK/$ZIP_NAME"
ditto -c -k --norsrc --noextattr --noacl "$STAGE" "$ZIP_W"

# ── 9) 打包后断言(硬要求 6)──────────────────────────────────────────────────────────────────────
unzip -Z1 "$ZIP_W" > "$WORK/entries.txt"
bad="$(grep -E '(^|/)__MACOSX(/|$)|(^|/)\.DS_Store$|^/|(^|/)\.\.(/|$)' "$WORK/entries.txt" || true)"
[ -z "$bad" ] || die "zip has entries that must not be shipped:
$bad"
# 根目录白名单:6 个 bundle、三个根文件、LICENSES/、third_party/(其下只许 notices/)。
awk -F/ '{ print $1 }' "$WORK/entries.txt" | LC_ALL=C sort -u > "$WORK/tops.txt"
{
    cat "$WORK/expected-names.txt"
    printf '%s\n' INSTALL.txt LICENSE.txt LICENSES THIRD-PARTY-NOTICES.md third_party
} | LC_ALL=C sort > "$WORK/expected-tops.txt"
if ! cmp -s "$WORK/tops.txt" "$WORK/expected-tops.txt"; then
    sed 's/^/  zip root: /' "$WORK/tops.txt" >&2
    die "zip root entries are not exactly the 6 bundles + INSTALL.txt, LICENSE.txt, THIRD-PARTY-NOTICES.md, LICENSES/, third_party/"
fi
bad="$(grep '^third_party/' "$WORK/entries.txt" | grep -v '^third_party/$' | grep -v '^third_party/notices/' || true)"
[ -z "$bad" ] || die "zip has entries under third_party/ outside notices/:
$bad"
# 合规文件与 NOTICES 点名的每个声明文件路径都在 zip 里(区分大小写:路径要在解压目录里原样可查)。
for r in LICENSE.txt THIRD-PARTY-NOTICES.md INSTALL.txt; do
    grep -Fqx -- "$r" "$WORK/entries.txt" || die "zip lacks $r"
done
while IFS= read -r p <&3; do
    grep -Fqx -- "$p" "$WORK/entries.txt" || die "zip lacks '$p' (listed by package.ps1 -Preflight)"
done 3< <(cat "$WORK/license.txt" "$WORK/notice.txt" "$WORK/cited.txt")
for r in $ROLES; do
    for ext in vst3 component; do
        grep -Fqx -- "SCVB $r.$ext/Contents/MacOS/SCVB $r" "$WORK/entries.txt" \
            || die "zip lacks the executable entry 'SCVB $r.$ext/Contents/MacOS/SCVB $r'"
    done
done
# 可执行位(zip 元数据):Contents/MacOS/ 下的文件条目恰好 6 个,全带 x 位。首字符放行 `-`(文件)与 `l`(符号链接)。
unzip -Z "$ZIP_W" > "$WORK/zipinfo.txt"
grep -E '/Contents/MacOS/[^/]+$' "$WORK/zipinfo.txt" > "$WORK/macho-entries.txt" || true
n="$(grep -c . "$WORK/macho-entries.txt" || true)"
[ "$n" = "6" ] || die "zip should hold exactly 6 Contents/MacOS/ files (3 VST3 + 3 AU), found $n"
bad="$(grep -vE '^[-l]rwx' "$WORK/macho-entries.txt" || true)"
[ -z "$bad" ] || die "zip lost the executable bit on:
$bad"
# 真解压一遍(ditto -x -k,与 INSTALL.txt 教用户的同一条命令),与暂存区逐文件比对:文件集合相同、每个文件字节相同、
# 符号链接指向相同;解出来的可执行文件仍可执行、仍只有 arm64。
UNPACK="$WORK/unpack"
mkdir "$UNPACK"
ditto -x -k "$ZIP_W" "$UNPACK"
(cd "$STAGE" && find . \( -type f -o -type l \) -print | LC_ALL=C sort) > "$WORK/stage-files.txt"
(cd "$UNPACK" && find . \( -type f -o -type l \) -print | LC_ALL=C sort) > "$WORK/unpack-files.txt"
if ! cmp -s "$WORK/stage-files.txt" "$WORK/unpack-files.txt"; then
    diff "$WORK/stage-files.txt" "$WORK/unpack-files.txt" >&2 || true
    die "the files unpacked from the zip differ from the staged files"
fi
while IFS= read -r rel <&3; do
    if [ -L "$STAGE/$rel" ]; then
        [ -L "$UNPACK/$rel" ] && [ "$(readlink "$STAGE/$rel")" = "$(readlink "$UNPACK/$rel")" ] \
            || die "symlink '$rel' did not survive the zip"
    else
        cmp -s "$STAGE/$rel" "$UNPACK/$rel" || die "'$rel' unpacked from the zip differs from the staged file"
    fi
done 3< "$WORK/stage-files.txt"
for r in $ROLES; do
    for ext in vst3 component; do
        f="$UNPACK/SCVB $r.$ext/Contents/MacOS/SCVB $r"
        [ -x "$f" ] || die "unpacked '$f' is not executable"
        [ "$(lipo -archs "$f")" = "arm64" ] || die "unpacked '$f' is not arm64-only"
    done
done
grep -Fqx -- "$SOURCE_LINE" "$UNPACK/INSTALL.txt" || die "INSTALL.txt in the zip lacks the source line"

# ── 10) .sha256 与 summary(硬要求 3)──────────────────────────────────────────────────────────────
HASH="$(shasum -a 256 "$ZIP_W" | awk '{ print $1 }')"
SIZE_BYTES="$(wc -c < "$ZIP_W" | tr -d '[:space:]')"
printf '%s  %s\n' "$HASH" "$ZIP_NAME" > "$WORK/$SHA_NAME"

N_ENTRIES="$(grep -c . "$WORK/unpack-files.txt" || true)"
N_NOTICES="$(grep -c . "$WORK/notice.txt" || true)"
CITED_LIST="$(paste -sd, "$WORK/cited.txt" | sed 's/,/, /g')"
MISSING_LIST="none"
if [ -s "$WORK/missing.txt" ]; then
    MISSING_LIST="$(paste -sd, "$WORK/missing.txt" | sed 's/,/, /g') (pipeline test only - must be empty for a real release)"
fi
BUNDLE_LIST="$(paste -sd, "$WORK/expected-names.txt" | sed 's/,/, /g')"
MINOS_LIST="$(cut -f2 "$WORK/minos.txt" | LC_ALL=C sort -u | paste -sd, - | sed 's/,/, /g')"
AU_LIST="$(paste -sd, "$WORK/au.txt" | sed 's/,/, /g')"
RELEASE_DATE="$(date -u '+%Y-%m-%d')"
{
    echo "# SCVB package summary (macOS)"
    echo ""
    echo "| key | value |"
    echo "| --- | --- |"
    echo "| version | $VERSION |"
    echo "| tag | $TAG |"
    echo "| platform | macOS, Apple Silicon (arm64 only), VST3 + AU |"
    echo "| zipFileName | $ZIP_NAME |"
    echo "| sizeBytes | $SIZE_BYTES |"
    echo "| sha256 | $HASH |"
    echo "| releaseDate | $RELEASE_DATE (UTC) |"
    echo "| sourceCommit | $SOURCE_COMMIT |"
    echo "| cmakeVersion | $CMAKE_VERSION |"
    echo "| bundles | $BUNDLE_LIST |"
    echo "| architectures | arm64 only (every Mach-O in every bundle checked with lipo -archs) |"
    echo "| minimumMacOS | $MINOS_LIST (LC_BUILD_VERSION minos of the bundle executables) |"
    echo "| auComponents | $AU_LIST |"
    echo "| missingLicenseTexts | $MISSING_LIST |"
    echo "| thirdPartyNotices | $N_NOTICES files under third_party/notices/ |"
    echo "| citedNoticePaths | $CITED_LIST (every file path THIRD-PARTY-NOTICES.md cites is in the zip) |"
    echo ""
    echo "Corresponding source: $SOURCE_URL"
    echo ""
    echo "## zip contents ($N_ENTRIES files)"
    echo ""
    echo "| entry | sha256 |"
    echo "| --- | --- |"
    while IFS= read -r rel <&3; do
        h="$(shasum -a 256 "$UNPACK/$rel" | awk '{ print $1 }')"
        printf '| `%s` | %s |\n' "${rel#./}" "$h"
    done 3< "$WORK/unpack-files.txt"
} > "$WORK/$SUMMARY_NAME"

mkdir -p "$OUT_DIR"
mv -f "$ZIP_W" "$OUT_DIR/$ZIP_NAME"
mv -f "$WORK/$SHA_NAME" "$OUT_DIR/$SHA_NAME"
mv -f "$WORK/$SUMMARY_NAME" "$OUT_DIR/$SUMMARY_NAME"
# 用 INSTALL.txt 教用户的那条命令自验(在输出目录里、对着挪过去的那一对文件)。
(cd "$OUT_DIR" && shasum -a 256 -c "$SHA_NAME") || die "shasum -a 256 -c $SHA_NAME failed in $OUT_DIR"

if [ -n "${GITHUB_OUTPUT:-}" ]; then
    {
        echo "zip=$OUT_DIR/$ZIP_NAME"
        echo "sha256=$HASH"
        echo "summary=$OUT_DIR/$SUMMARY_NAME"
    } >> "$GITHUB_OUTPUT"
fi

echo "package-macos: OK"
echo "  zip      $OUT_DIR/$ZIP_NAME ($SIZE_BYTES bytes, $N_ENTRIES files)"
echo "  sha256   $HASH"
echo "  summary  $OUT_DIR/$SUMMARY_NAME"
