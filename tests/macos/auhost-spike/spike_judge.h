// SPDX-License-Identifier: GPL-3.0-or-later
// spike_judge.h —— M01 判定表(G0)的唯一实现。只用标准库,不含任何平台头:
// 判定逻辑与「怎么在 mac 上量」分开,才能脱离 mac 单独做删除式 / 反向注入(--self-test 用合成输入
// 逐格断言判定结果),判定表本身有牙,而不是只在 runner 恰好给出某一种结果时被执行到一条分支。
//
// 输入:两次独立运行(--mode inproc / --mode oop)各自落盘的 key=value 文本(.kv)。
// 输出:verdict(A / A-prime / B / C / inconclusive)+ 退出码 + 逐条依据。
//
// 判定表(与 docs/spikes/S5-macos-auhost-oop.md 同一份;改一处必须改另一处):
//   A            OOP 下 shm 创建/附着全成功,与宿主双向互见,锁文件可建可 flock,HOME/NSHomeDirectory 未被容器化
//   A-prime      shm 与互见都成立,但锁文件路径被拒,或 HOME/NSHomeDirectory 被容器化
//   B            OOP 下 shm 被拒(EPERM/EACCES),但三个实例同 pid,且 B 能找到 A 注册的 ObjC 会合类
//   C            OOP 下 shm 被拒,且实例 pid 不同
//   inconclusive 进程内对照失败(退出码 1),或 OOP 实例化失败 / 实例其实没跑到宿主进程外 / 探针没跑完 /
//                落在表外组合(退出码 0)
// 退出码:负对照不成立(组件没注册上、进程内实例不在宿主进程里、进程内 shm 没成功、32 字符名没报
//         ENAMETOOLONG、进程内没跑完)= 1;
//         其余结论(含因 OOP 起不来而判 inconclusive)= 0。
#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace spike
{
using KV = std::map<std::string, std::string>;

// macOS 的 errno 数值(判定只认 mac 上量出来的码;这里写死,不用宿主平台的 <cerrno>,
// 否则在别的平台上做 --self-test 时 EACCES 之类可能是另一个数)。
inline constexpr int kMacEPERM = 1;
inline constexpr int kMacEACCES = 13;

inline constexpr const char* kInstances[] = {"a1", "a2", "b1"};

inline bool parseKV(const std::string& text, KV& out)
{
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty())
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            return false;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return true;
}

inline long getl(const KV& kv, const std::string& key, long def = -1)
{
    const auto it = kv.find(key);
    if (it == kv.end() || it->second.empty())
        return def;
    char* end = nullptr;
    const long v = std::strtol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str() || *end != '\0')
        return def;
    return v;
}

struct ModeEval
{
    bool present = false; // kv 在且 run.complete=1
    bool allFound = false; // 三个组件都在注册表里
    bool allInstantiated = false; // 三个实例都实例化成功
    bool allInitialized = false; // 三个实例 AudioUnitInitialize 都成功
    bool allDone = false; // 三个实例都经参数通道报告「探针跑完」
    bool shmOk = false; // 恰好一个创建者(open/trunc/map/peer 全 1),其余附着(open=2,map/peer=1)
    bool shmDenied = false; // 三个实例的 shm_open 都是 EPERM 或 EACCES
    bool longNameOk = false; // 三个实例的 32 字符名都报 ENAMETOOLONG
    bool samePid = false; // 三个实例 pid 相同且非 0
    bool inHostProcess = false; // 三个实例 pid 都等于宿主 pid(进程内模式必须成立)
    bool outOfHostProcess = false; // 三个实例 pid 都非 0 且都不等于宿主 pid(进程外模式必须成立)
    bool hostSeesPlugin = false; // 宿主打开插件建的段且读到 magic
    bool pluginSeesHost = false; // 三个实例都读到宿主建的段且 magic+token 对得上
    bool liveRender = false; // 渲染后宿主在段里看到每个实例的计数前进(同一块物理内存)
    bool lockOk = false; // 锁文件都建得出;恰好一个拿到 EX,其余 EWOULDBLOCK;全部持 SH
    bool homeContained = false; // HOME 或 NSHomeDirectory 不等于真实 home
    bool homeUnset = false; // 有实例 HOME 未设(只记录)
    bool objcOk = false; // B 找到并调用了 A 注册的会合类
};

inline ModeEval evaluate(const KV* kv)
{
    ModeEval e;
    if (kv == nullptr || getl(*kv, "run.complete") != 1)
        return e;
    e.present = true;

    auto inst = [&](const char* i, const char* key) { return getl(*kv, std::string(i) + "." + key); };

    e.allFound = e.allInstantiated = e.allInitialized = e.allDone = true;
    e.shmDenied = e.longNameOk = e.pluginSeesHost = e.liveRender = true;
    int creators = 0;
    int attachersOk = 0;
    int lockExOwners = 0;
    int lockExBlocked = 0;
    bool lockAll = true;
    long pid0 = -2;
    bool pidsEqual = true;
    const long hostPid = getl(*kv, "host.pid");
    e.inHostProcess = e.outOfHostProcess = hostPid > 0;
    for (const char* i : kInstances)
    {
        e.allFound = e.allFound && inst(i, "found") == 1;
        e.allInstantiated = e.allInstantiated && inst(i, "instantiated") == 1;
        e.allInitialized = e.allInitialized && inst(i, "initialized") == 1;
        e.allDone = e.allDone && inst(i, "out.done") == 1;

        const long open = inst(i, "out.shmOpen");
        const long trunc = inst(i, "out.shmTrunc");
        const long map = inst(i, "out.shmMap");
        const long peer = inst(i, "out.shmPeer");
        if (open == 1 && trunc == 1 && map == 1 && peer == 1)
            ++creators;
        else if (open == 2 && map == 1 && peer == 1)
            ++attachersOk;
        e.shmDenied = e.shmDenied && (open == 100 + kMacEPERM || open == 100 + kMacEACCES);
        e.longNameOk = e.longNameOk && inst(i, "out.shmLong") == 1;
        e.pluginSeesHost = e.pluginSeesHost && inst(i, "out.hostSeg") == 1;

        const long lockEx = inst(i, "out.lockEx");
        lockAll = lockAll && inst(i, "out.lockOpen") == 1 && inst(i, "out.lockSh") == 1;
        if (lockEx == 1)
            ++lockExOwners;
        else if (lockEx == 2)
            ++lockExBlocked;

        const long homeEnv = inst(i, "out.homeEnv");
        const long nsHome = inst(i, "out.nsHome");
        if (homeEnv == 2 || nsHome == 2)
            e.homeContained = true;
        if (homeEnv == 3)
            e.homeUnset = true;

        const long pid = inst(i, "out.pid");
        e.inHostProcess = e.inHostProcess && pid == hostPid;
        e.outOfHostProcess = e.outOfHostProcess && pid > 0 && pid != hostPid;
        if (pid <= 0)
            pidsEqual = false;
        else if (pid0 == -2)
            pid0 = pid;
        else if (pid != pid0)
            pidsEqual = false;

        // 渲染计数:按宿主分配的 nonce 在宿主读到的槽位里找本实例。
        const long nonce = inst(i, "nonce");
        bool seen = false;
        for (int k = 0; k < 16; ++k)
        {
            const std::string s = "host.slot" + std::to_string(k) + ".";
            if (nonce > 0 && getl(*kv, s + "nonce") == nonce)
            {
                seen = getl(*kv, s + "renders_after", 0) > getl(*kv, s + "renders_before", 0);
                break;
            }
        }
        e.liveRender = e.liveRender && seen;
    }
    e.shmOk = creators == 1 && attachersOk == 2;
    e.samePid = pidsEqual && pid0 > 0;
    e.hostSeesPlugin = getl(*kv, "host.plugin_seg_open") == 1 && getl(*kv, "host.plugin_seg_magic") == 1;
    e.lockOk = lockAll && lockExOwners == 1 && lockExBlocked == 2;
    e.objcOk = getl(*kv, "b1.out.objc") == 1;
    return e;
}

struct Verdict
{
    std::string verdict;
    int exitCode = 1;
    std::vector<std::pair<std::string, bool>> controls; // 负对照逐条
    std::vector<std::string> notes;
    ModeEval inproc;
    ModeEval oop;
};

inline Verdict judge(const KV* inproc, const KV* oop)
{
    Verdict v;
    v.inproc = evaluate(inproc);
    v.oop = evaluate(oop);
    const ModeEval& i = v.inproc;
    const ModeEval& o = v.oop;

    v.controls = {
        {"inproc.result_present", i.present},
        {"inproc.components_registered", i.allFound},
        {"inproc.instantiated_initialized_done", i.allInstantiated && i.allInitialized && i.allDone},
        {"inproc.ran_in_host_process", i.inHostProcess},
        {"inproc.shm_create_attach_ok", i.shmOk},
        {"inproc.long_name_enametoolong", i.longNameOk},
    };
    bool controlsOk = true;
    for (const auto& c : v.controls)
        controlsOk = controlsOk && c.second;
    if (!controlsOk)
    {
        v.verdict = "inconclusive";
        v.exitCode = 1;
        v.notes.push_back("negative control failed in the in-process run (see controls); the OOP result is not "
                          "interpretable without it");
        if (i.present && !i.allFound)
            v.notes.push_back("component not registered: subtype/manufacturer mismatch or AU registrar not rescanned");
        return v;
    }
    if (!i.samePid)
        v.notes.push_back("in-process instances reported different pids (unexpected)");
    if (!i.lockOk)
        v.notes.push_back("in-process lock-file probe did not show the expected EX/EWOULDBLOCK/SH pattern");

    v.exitCode = 0;
    if (!o.present)
    {
        v.verdict = "inconclusive";
        v.notes.push_back("OOP run produced no complete result (host crashed, timed out or was not run)");
        return v;
    }
    if (!o.allFound || !o.allInstantiated || !o.allInitialized)
    {
        v.verdict = "inconclusive";
        v.notes.push_back("OOP instantiation/initialization failed (e.g. no GUI session on the runner)");
        return v;
    }
    if (!o.allDone)
    {
        v.verdict = "inconclusive";
        v.notes.push_back("OOP instances never reported probes done through the parameter channel");
        return v;
    }
    // 判据失效的那一种:进程外标志被静默忽略、插件其实跑在宿主进程里 —— 那样量到的是进程内结果,
    // 绝不能当成进程外的 A 报出去。
    if (!o.outOfHostProcess)
    {
        v.verdict = "inconclusive";
        v.notes.push_back("OOP: instances did not run outside the host process (pid equals the host pid or is "
                          "unknown) - the out-of-process flag did not take effect");
        return v;
    }
    if (o.present && !o.longNameOk)
        v.notes.push_back("OOP: 32-char name did not report ENAMETOOLONG in every instance (recorded only)");
    if (o.homeUnset)
        v.notes.push_back("OOP: HOME is unset in at least one instance (not treated as containerized)");

    if (o.shmOk && o.hostSeesPlugin && o.pluginSeesHost)
    {
        if (!o.liveRender)
            v.notes.push_back("OOP: render counters did not advance in the host's view (recorded only)");
        if (o.lockOk && !o.homeContained)
        {
            v.verdict = "A";
            if (!o.samePid)
                v.notes.push_back("OOP: A and B instances live in different processes; shm still crosses "
                                  "processes, so scheme A does not depend on a shared process");
        }
        else
        {
            v.verdict = "A-prime";
            if (!o.lockOk)
                v.notes.push_back("OOP: lock file under the real home could not be created/flocked as expected");
            if (o.homeContained)
                v.notes.push_back("OOP: HOME or NSHomeDirectory differs from the real home (containerized)");
        }
        return v;
    }
    if (o.shmDenied)
    {
        if (o.samePid && o.objcOk)
            v.verdict = "B";
        else if (!o.samePid)
            v.verdict = "C";
        else
        {
            v.verdict = "inconclusive";
            v.notes.push_back("OOP: shm denied, same pid, but the ObjC rendezvous class was not visible to B "
                              "(outside the decision table)");
        }
        return v;
    }
    v.verdict = "inconclusive";
    v.notes.push_back("OOP: partial shm result (neither fully working with mutual host visibility nor uniformly "
                      "denied) - outside the decision table");
    return v;
}

// ---- 自测:合成输入逐格断言判定表 ----------------------------------------------------------------
inline KV syntheticGoodMode(bool oop)
{
    KV kv;
    kv["run.complete"] = "1";
    kv["mode"] = oop ? "oop" : "inproc";
    kv["host.plugin_seg_open"] = "1";
    kv["host.plugin_seg_magic"] = "1";
    kv["host.pid"] = oop ? "4000" : "1000"; // 进程内:实例与宿主同 pid;进程外:实例在另一个进程
    const char* pid = oop ? "4242" : "1000";
    int n = 0;
    for (const char* i : kInstances)
    {
        const std::string p = std::string(i) + ".";
        ++n;
        kv[p + "found"] = "1";
        kv[p + "instantiated"] = "1";
        kv[p + "initialized"] = "1";
        kv[p + "nonce"] = std::to_string(n);
        kv[p + "out.done"] = "1";
        kv[p + "out.pid"] = pid;
        kv[p + "out.shmOpen"] = n == 1 ? "1" : "2";
        kv[p + "out.shmTrunc"] = n == 1 ? "1" : "4";
        kv[p + "out.shmMap"] = "1";
        kv[p + "out.shmPeer"] = "1";
        kv[p + "out.shmLong"] = "1";
        kv[p + "out.hostSeg"] = "1";
        kv[p + "out.lockOpen"] = "1";
        kv[p + "out.lockEx"] = n == 1 ? "1" : "2";
        kv[p + "out.lockSh"] = "1";
        kv[p + "out.homeEnv"] = "1";
        kv[p + "out.nsHome"] = "1";
        kv[p + "out.objc"] = n == 3 ? "1" : (n == 1 ? "1" : "2");
        const std::string s = "host.slot" + std::to_string(n - 1) + ".";
        kv[s + "nonce"] = std::to_string(n);
        kv[s + "renders_before"] = "0";
        kv[s + "renders_after"] = "16";
    }
    return kv;
}

inline void setAll(KV& kv, const std::string& key, const std::string& value)
{
    for (const char* i : kInstances)
        kv[std::string(i) + "." + key] = value;
}

inline int selfTest()
{
    struct Case
    {
        const char* name;
        KV inproc;
        KV oop;
        bool hasOop;
        const char* wantVerdict;
        int wantExit;
    };
    std::vector<Case> cases;
    const KV gi = syntheticGoodMode(false);
    const KV go = syntheticGoodMode(true);
    const std::string eperm = std::to_string(100 + kMacEPERM);
    const std::string eacces = std::to_string(100 + kMacEACCES);

    cases.push_back({"all good -> A", gi, go, true, "A", 0});
    {
        KV o = go;
        o["b1.out.pid"] = "4343";
        cases.push_back({"shm ok, A/B different pid -> A (noted)", gi, o, true, "A", 0});
    }
    {
        KV o = go;
        setAll(o, "out.lockOpen", eperm);
        setAll(o, "out.lockEx", "4");
        setAll(o, "out.lockSh", "4");
        cases.push_back({"lock path denied -> A-prime", gi, o, true, "A-prime", 0});
    }
    {
        KV o = go;
        o["a2.out.nsHome"] = "2";
        cases.push_back({"NSHomeDirectory containerized -> A-prime", gi, o, true, "A-prime", 0});
    }
    {
        KV o = go;
        o["a1.out.homeEnv"] = "2";
        cases.push_back({"HOME containerized -> A-prime", gi, o, true, "A-prime", 0});
    }
    {
        KV o = go;
        setAll(o, "out.shmOpen", eperm);
        setAll(o, "out.shmTrunc", "0");
        setAll(o, "out.shmMap", "4");
        setAll(o, "out.shmPeer", "4");
        cases.push_back({"shm EPERM, same pid, objc visible -> B", gi, o, true, "B", 0});
        KV o2 = o;
        o2["b1.out.objc"] = "6";
        cases.push_back({"shm EPERM, same pid, objc not visible -> inconclusive", gi, o2, true, "inconclusive", 0});
        KV o3 = o;
        o3["b1.out.pid"] = "5555";
        cases.push_back({"shm EPERM, different pid -> C", gi, o3, true, "C", 0});
        KV o4 = o3;
        setAll(o4, "out.shmOpen", eacces);
        cases.push_back({"shm EACCES, different pid -> C", gi, o4, true, "C", 0});
    }
    {
        KV o = go;
        o["host.plugin_seg_open"] = eperm;
        cases.push_back({"shm ok in service but host cannot see it -> inconclusive", gi, o, true, "inconclusive", 0});
        KV o2 = go;
        o2["a2.out.hostSeg"] = "5";
        cases.push_back({"plugin cannot see host segment -> inconclusive", gi, o2, true, "inconclusive", 0});
    }
    {
        KV o = go;
        setAll(o, "instantiated", "0");
        cases.push_back({"OOP instantiation failed -> inconclusive, exit 0", gi, o, true, "inconclusive", 0});
        KV o2 = go;
        o2["b1.out.done"] = "0";
        cases.push_back({"OOP probes never done -> inconclusive, exit 0", gi, o2, true, "inconclusive", 0});
        cases.push_back({"OOP result missing -> inconclusive, exit 0", gi, KV{}, false, "inconclusive", 0});
        KV o3 = go;
        o3["host.pid"] = "4242";
        cases.push_back({"OOP flag ignored (instances in the host process) -> inconclusive, exit 0", gi, o3, true,
                         "inconclusive", 0});
        KV o4 = go;
        o4.erase("host.pid");
        cases.push_back({"OOP host pid unknown -> inconclusive, exit 0", gi, o4, true, "inconclusive", 0});
    }
    {
        KV in = gi;
        in["a1.found"] = "0";
        cases.push_back({"inproc component not registered (wrong subtype) -> inconclusive, exit 1", in, go, true,
                         "inconclusive", 1});
        KV in2 = gi;
        in2["b1.out.shmLong"] = "3";
        cases.push_back({"inproc 32-char name succeeded -> inconclusive, exit 1", in2, go, true, "inconclusive", 1});
        KV in3 = gi;
        setAll(in3, "out.shmOpen", eperm);
        cases.push_back({"inproc shm failed -> inconclusive, exit 1", in3, go, true, "inconclusive", 1});
        KV in4 = gi;
        in4.erase("run.complete");
        cases.push_back({"inproc result incomplete -> inconclusive, exit 1", in4, go, true, "inconclusive", 1});
        KV in5 = gi;
        in5["a2.out.done"] = "0";
        cases.push_back({"inproc probes not done -> inconclusive, exit 1", in5, go, true, "inconclusive", 1});
        KV in6 = gi;
        in6["b1.out.pid"] = "1001";
        cases.push_back(
            {"inproc instance outside the host process -> inconclusive, exit 1", in6, go, true, "inconclusive", 1});
    }
    {
        // 没有创建者(上一轮的残段没清掉,三个实例全是附着):各探针都「成功」,但不能被读成 A。
        KV o = go;
        o["a1.out.shmOpen"] = "2";
        o["a1.out.shmTrunc"] = "4";
        cases.push_back({"no creator (stale segment) -> inconclusive", gi, o, true, "inconclusive", 0});
    }

    int failed = 0;
    for (const Case& c : cases)
    {
        const Verdict v = judge(&c.inproc, c.hasOop ? &c.oop : nullptr);
        const bool ok = v.verdict == c.wantVerdict && v.exitCode == c.wantExit;
        std::printf("[%s] %s => verdict=%s exit=%d (want %s/%d)\n", ok ? "PASS" : "FAIL", c.name, v.verdict.c_str(),
                    v.exitCode, c.wantVerdict, c.wantExit);
        if (!ok)
            ++failed;
    }
    std::printf("judge self-test: %d cases, %d failed\n", static_cast<int>(cases.size()), failed);
    return failed == 0 ? 0 : 1;
}
} // namespace spike
