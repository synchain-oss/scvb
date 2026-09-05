// SPDX-License-Identifier: GPL-3.0-or-later
// check-source-encoding.mjs —— 「送进 C 标准输出的字面量必须是 ASCII」的机检(零依赖,Node >= 18,ESM)。
//
// [SL-325] 病灶来自同一条缺陷两次落地:
//   · #204 在 `tests/host/test_host_harness.cpp` 加了两处中文 `fprintf` 提示;
//   · #206 在 `tests/ipc/test_ipc_contract.cpp` 加了同形态的一处。
//   两次都在**中文 Windows(CP936)**上触发 MSVC `C4819`,于是 gates gate 5「/W4 零 warning」
//   对本机所有人判红,而 **CI runner 上零命中** —— 这条对 CI 隐形、只打本机。#206 实测确认:
//   把运行期文案改成 ASCII ⇒ 0 条;加 BOM 无效;`/utf-8` 压不掉;中文**注释**不触发。
//   止血是 #207 给那个 TU 挂 `/wd4819`(SL-325 已撤),根治就是这个脚本。
//
// [SL-325] **触发条件实测**(17 个探针,全部注进 `test_host_harness.cpp` 真编一遍,
//   数 `warning C4819` 的条数;这是为了确认本判据的面**盖得住**真缺陷,不是照着猜写的):
//     形态                                                          C4819
//     `const char* x = "单段中文";`                                    0
//     `const char* x = "拼接前半" "拼接后半";`(不进 printf)             0
//     历史原文四段拼接赋给 `const char*`(不进 printf)                    0
//     `fprintf(stderr, "单段中文");`                                   0
//     `fprintf(stderr, "单段中文 %d", 1);`                             0
//     `fprintf(stderr, "%s", "单段中文");`                             0
//     `fprintf(stderr, "part-a" "中文");`(首段 ASCII)                  0
//     `fprintf(stderr, "ascii-" "中文-" "%d", 1);`(首段 ASCII)         0
//     `fprintf(stderr, "中文-" "ascii");`(首段中文、无实参、两段)         0
//     `fprintf(stderr, "中" "%d", 1);`(首段中文 + 实参)                 1
//     三段长中文拼接、无格式符无实参                                       1
//     同上 + `%d` + 实参                                               1
//     #204 原文两处(四段拼接 + `%lu` + 实参)                            2
//   **观察到的必要条件**(每一个报警形态都同时满足这三条,反例各有一格):
//     ① 非 ASCII 字面量落在 **printf 族调用的实参位**上(族外一律 0 条);
//     ② 是**相邻字面量拼接**(单段一律 0 条);
//     ③ 拼接的**首段**含非 ASCII(首段 ASCII 的两格都是 0 条)。
//   **充分条件没有钉死**:`"中文-" "ascii"` 三条都满足却不报,而它加上一个变参、或再多两段
//   长中文就报。所以别把上面三条当成「符合就一定报」——那句话没量出来。
//   本判据只用**必要条件的第 ①条**,因此它的面**严格宽于**已知的每一个报警形态:
//   凡是会报 C4819 的写法,它都拦得住;拦下的一些写法今天并不报 C4819(那些也该是 ASCII,
//   理由见下面第三条)。
//   顺带解掉一个旧疑点:警告的行列指向**整条调用语句的末尾**(实测 `(53,57)` 落在
//   `static_cast<unsigned long>(err));` 那一行的行尾),不指向那个不可表示的字符 ——
//   #207 头注里「报出来的 `:174` / `:186` 两行本身是纯 ASCII、行号与字符对不上」就是这个。
//
// 判据面(**故意窄**,窄的理由在下面「边界」里):`src/**` 与 `tests/**` 的 C/C++ 源码里,
//   C 标准输出族的调用实参中,任何字符串字面量不得含非 ASCII 字符(码点 > 127)。
//   族 = printf / fprintf / vprintf / vfprintf / fputs / puts / perror(可带 `std::` 前缀)。
//
// 为什么是这一族、而不是「所有字符串字面量」:
//   · **按字面全仓做不了**:实测 `src/**` + `tests/**` 里 80 个源文件在双引号字面量里有中文,
//     光 `test_host_harness.cpp` 就 152 处 —— 绝大多数是 Catch2 的 `TEST_CASE` 名。
//     它们编了几个月都干净,不是缺陷;把它们判红只会让人关掉这个门禁。
//   · **这一族正是两次栽的形态**,且今天基线 **0 命中** —— 加上去就是干净的。
//   · 它另有一条独立于 C4819 的正当性:这类文案直接写进 gates / CI 日志,而消费日志那一端的
//     控制台代码页不确定(本机 CP936、runner UTF-8、别人重定向到文件再拿别的工具读)。
//     ASCII 是这条链路上唯一不会烂的编码。
//
// 边界(照实说,别让人以为它更严):
//   · `std::cout` / `std::cerr` 的 `<<` 链**不在面内**。`tests/tools/` 下的三个 CLI
//     (scvb_bench / scvb_diag / scvb_nulltest)有 14 处中文 `<<` 输出,那是给人看的中文界面、
//     且从未触发 C4819。把它们纳入等于顺手改掉三个工具的界面语言,不是本卡的事。
//     哪天要收,先立卡改文案,再把 `cout|cerr|clog` 加进 FAMILY —— 判据本身不用动。
//   · 宽字面量(`L"…"`)落在族内**照样判红**:族里没有 `fwprintf`,所以宽字面量出现在
//     `fprintf` 实参上本来就是错的。族外的宽字面量(如 `SegmentBackendWin32.cpp` 那条
//     `OutputDebugString` 用的 `L"… 失败 …"`)不在面内。
//   · 它查得出「字面量里有非 ASCII」,查不出「非 ASCII 从变量里流进来」——
//     `fprintf(stderr, "%s", someChineseString)` 它看不见,也不该看见。
//   · **注释先剥**:中文注释不触发 C4819(#206 实测),也不该被这条判据碰。剥离时**逐字符
//     换空格、保留换行**,行号因此与原文逐行对齐 —— 不能「删掉注释再匹配」,那样报出来的
//     行号是错的,而排障的人就是照着行号去找那一处。

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = fileURLToPath(new URL('..', import.meta.url));
const ROOTS = ['src', 'tests'];
const EXTS = ['.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.mm'];
// 反斜杠一律走 fromCharCode:本仓工具链里字面的双反斜杠会被折成一个,写进正则或字符比较后
// **静默失效**(CLAUDE 记过的教训)。这个文件里不留任何字面反斜杠。
const BS = String.fromCharCode(92);
const FAMILY = /\b(?:std::)?(printf|fprintf|vprintf|vfprintf|fputs|puts|perror)\s*\(/g;

// ── 注释剥离:换空格、保留换行,长度与原文逐字符对齐(行号才对得上)──────────────
export function blankComments(src) {
  const out = src.split('');
  const n = src.length;
  let i = 0;
  const blank = (k) => {
    if (out[k] !== '\n') out[k] = ' ';
  };
  while (i < n) {
    const c = src[i];
    const d = i + 1 < n ? src[i + 1] : '';
    if (c === '/' && d === '/') {
      while (i < n && src[i] !== '\n') {
        blank(i);
        i++;
      }
      continue;
    }
    if (c === '/' && d === '*') {
      blank(i);
      blank(i + 1);
      i += 2;
      while (i < n && !(src[i] === '*' && src[i + 1] === '/')) {
        blank(i);
        i++;
      }
      if (i < n) {
        blank(i);
        blank(i + 1);
        i += 2;
      }
      continue;
    }
    if (c === '"' || c === "'") {
      // 原始字面量 `R"delim( … )delim"`:里面的引号与反斜杠都不转义,普通跳法会跑飞。
      if (c === '"' && i > 0 && src[i - 1] === 'R') {
        const open = src.indexOf('(', i + 1);
        if (open > 0) {
          const delim = src.slice(i + 1, open);
          const close = src.indexOf(')' + delim + '"', open);
          i = close < 0 ? n : close + delim.length + 2;
          continue;
        }
      }
      const quote = c;
      i++;
      while (i < n) {
        if (src[i] === BS) {
          i += 2;
          continue;
        }
        if (src[i] === quote) {
          i++;
          break;
        }
        if (src[i] === '\n') break; // 未闭合:别把后面整份都当字面量吞掉
        i++;
      }
      continue;
    }
    i++;
  }
  return out.join('');
}

// ── 判据本体:剥注释 → 找族内调用 → 走到配对右括号 → 实参里的字面量逐个查非 ASCII ──
export function findViolations(src, label) {
  const code = blankComments(src);
  const hits = [];
  const lineOf = (idx) => {
    let line = 1;
    for (let k = 0; k < idx; k++) if (code[k] === '\n') line++;
    return line;
  };
  FAMILY.lastIndex = 0;
  let m;
  while ((m = FAMILY.exec(code)) !== null) {
    const fn = m[1];
    let i = m.index + m[0].length; // 落在左括号之后
    let depth = 1;
    const n = code.length;
    while (i < n && depth > 0) {
      const c = code[i];
      if (c === '(') {
        depth++;
        i++;
        continue;
      }
      if (c === ')') {
        depth--;
        i++;
        continue;
      }
      if (c === "'") {
        i++;
        while (i < n) {
          if (code[i] === BS) {
            i += 2;
            continue;
          }
          if (code[i] === "'" || code[i] === '\n') {
            i++;
            break;
          }
          i++;
        }
        continue;
      }
      if (c === '"') {
        const start = i;
        let bad = null;
        i++;
        while (i < n) {
          if (code[i] === BS) {
            i += 2;
            continue;
          }
          if (code[i] === '"') {
            i++;
            break;
          }
          if (code[i] === '\n') break;
          if (bad === null && code.codePointAt(i) > 127) bad = code[i];
          i++;
        }
        if (bad !== null) {
          hits.push({
            file: label,
            line: lineOf(start),
            fn,
            char: bad,
            text: code.slice(start, Math.min(i, start + 60)),
          });
        }
        continue;
      }
      i++;
    }
  }
  return hits;
}

function walk(dir, acc) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) {
      walk(p, acc);
      continue;
    }
    if (EXTS.some((e) => name.endsWith(e))) acc.push(p);
  }
  return acc;
}

// ── --self-test:夹具一律**拼装**,不从仓里读 ─────────────────────────────────────
// 本脚本自己不在扫描面里(只扫 C/C++),但夹具里那条中文 `fprintf` 若落进真源码就该红,
// 所以中文用 fromCharCode 拼 —— 文件里不留一个「长得像违规、却因为在自己身上而豁免」的样本。
function selfTest() {
  const cjk = String.fromCharCode(0x5931, 0x8d25); // 「失败」
  const nl = String.fromCharCode(92, 110); // 源码里的两字符转义 n
  const fails = [];
  const expect = (name, src, want) => {
    const got = findViolations(src, name).length;
    if (got !== want) fails.push(`${name}:期望 ${want} 条,实得 ${got} 条`);
  };

  expect('ascii-fprintf', `fprintf(stderr, "guard failed, exiting");`, 0);
  expect('cjk-fprintf', `fprintf(stderr, "${cjk}");`, 1);
  // 注释里的同一句必须**不**算 —— 中文注释不触发 C4819,剥离没做对这一格就会红。
  expect('cjk-in-line-comment', `// fprintf(stderr, "${cjk}");`, 0);
  expect('cjk-in-block-comment', `/* fprintf(stderr, "${cjk}"); */`, 0);
  // 族外:Catch2 用例名占了全仓中文字面量的大头,判据必须放它们过去。
  expect('cjk-test-case-name', `TEST_CASE("HOST L-6b:${cjk}", "[host]");`, 0);
  expect('cjk-cout', `std::cerr << "${cjk}" << std::endl;`, 0);
  // **两次事故的真实形态**:多段相邻窄字面量拼接、跨行。只钉单行会漏掉它。
  expect('cjk-adjacent-multiline', `fprintf(stderr,\n  "[SL-000] ok${nl}"\n  "${cjk}${nl}",\n  1);`, 1);
  // 括号嵌套:实参里有函数调用时不能提前收工。
  expect('cjk-after-nested-call', `fprintf(stderr, "%d ${cjk}", static_cast<int>(f(1, 2)));`, 1);
  // 行号要落在**字面量那一行**,不是调用起始行(排障靠它)。
  const multi = `int x = 1;\nfprintf(stderr,\n  "${cjk}");`;
  const got = findViolations(multi, 'lineno');
  if (got.length !== 1 || got[0].line !== 3) {
    fails.push(`lineno:期望第 3 行 1 条,实得 ${JSON.stringify(got.map((h) => h.line))}`);
  }

  if (fails.length > 0) {
    console.log('  [FAIL] check-source-encoding --self-test:');
    for (const f of fails) console.log('    ' + f);
    process.exit(1);
  }
  console.log(
    '  check-source-encoding --self-test:9 格全过(ASCII 不误报 / 中文抓得到 / 注释与族外放行 / 跨行拼接 / 行号对位)',
  );
  process.exit(0);
}

if (process.argv.includes('--self-test')) selfTest();

const files = [];
for (const r of ROOTS) walk(join(REPO_ROOT, r), files);
if (files.length === 0) {
  console.log('  [FAIL] src/ 与 tests/ 下一个 C/C++ 源文件都没扫到 —— 空集合会让本判据恒真');
  process.exit(1);
}

let calls = 0;
const violations = [];
for (const f of files) {
  const src = readFileSync(f, 'utf8');
  const code = blankComments(src);
  FAMILY.lastIndex = 0;
  while (FAMILY.exec(code) !== null) calls++;
  violations.push(...findViolations(src, relative(REPO_ROOT, f).split(sep).join('/')));
}

if (calls === 0) {
  // 判据面为空 = 判据恒真。今天全仓 17 处族内调用;掉到 0 只可能是 FAMILY 被改坏了,
  // 而「恒绿」连删除式都照不出来(0 == 0),所以这里判负,不安静通过。
  console.log('  [FAIL] 全仓一处 printf 族调用都没扫到 —— FAMILY 多半被改坏了,判据面为空即恒真');
  process.exit(1);
}

if (violations.length > 0) {
  console.log(
    `  [FAIL] ${violations.length} 处 printf 族调用的字面量含非 ASCII(CP936 上触发 MSVC C4819,gate 5 判红,CI 隐形):`,
  );
  for (const v of violations) {
    console.log(`    ${v.file}:${v.line}  ${v.fn}(… ${v.text} …)  首个非 ASCII 字符:${v.char}`);
  }
  console.log('    修法:运行期文案改 ASCII,要说的中文留在注释里(见 tests/support/exclusive_guard.h)。');
  process.exit(1);
}

console.log(`  printf 族字面量全 ASCII:扫了 ${files.length} 个源文件、${calls} 处族内调用,0 处违规`);
