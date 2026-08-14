// SPDX-License-Identifier: GPL-3.0-or-later
// scvb_diag —— S1 路由 spike 诊断导出工具(J09 正式通道,常驻仓库,release 矩阵复用)。
//   读 ctrl 段的 Output 全局信息小节 + registry 各 slot + 各 audio 环头的 channels,
//   按 1Hz 追加 csv(含 [J57] channels 列,便于逐轨核对声道判定)。
// 零 JUCE 依赖;只读(OpenFileMappingW,不创建段)。

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "SegmentLayout.h"

namespace
{
struct Options
{
    std::string out;
    unsigned int group = 1;
    unsigned int intervalMs = 1000;
    bool help = false;
};

bool parseArgs(int argc, char** argv, Options& o, std::string& err)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h")
        {
            o.help = true;
            return true;
        }
        if (a == "--out" || a == "--group" || a == "--interval-ms")
        {
            if (i + 1 >= argc)
            {
                err = a + " 需要一个参数";
                return false;
            }
            const std::string v = argv[++i];
            if (a == "--out")
            {
                o.out = v;
            }
            else
            {
                char* end = nullptr;
                const long n = std::strtol(v.c_str(), &end, 10);
                if (end == v.c_str() || *end != '\0' || n < 0)
                {
                    err = a + " 需要非负整数";
                    return false;
                }
                if (a == "--group")
                {
                    o.group = static_cast<unsigned int>(n);
                }
                else
                {
                    o.intervalMs = static_cast<unsigned int>(n);
                }
            }
        }
        else
        {
            err = "未知选项: " + a;
            return false;
        }
    }
    if (!o.help && o.out.empty())
    {
        err = "--out <csv> 必填";
        return false;
    }
    return true;
}

void printUsage()
{
    std::cout << "scvb_diag —— SCVB S1 诊断导出(读 ctrl Output 全局信息小节 + registry + audio 环头)\n"
              << "用法: scvb_diag --out <csv> [--group <G>] [--interval-ms <ms>]\n"
              << "  按 interval-ms(默认 1000)读一次,追加 csv 行;Ctrl+C 停止。\n";
}

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty())
    {
        return std::wstring();
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

struct ReadView
{
    void* base = nullptr;
    HANDLE mapping = nullptr;
    bool open(const std::wstring& name)
    {
        close();
        mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (mapping == nullptr)
        {
            return false;
        }
        base = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (base == nullptr)
        {
            ::CloseHandle(mapping);
            mapping = nullptr;
            return false;
        }
        return true;
    }
    void close()
    {
        if (base != nullptr)
        {
            ::UnmapViewOfFile(base);
            base = nullptr;
        }
        if (mapping != nullptr)
        {
            ::CloseHandle(mapping);
            mapping = nullptr;
        }
    }
    ~ReadView() { close(); }
};

std::wstring segName(const std::string& suffix, unsigned int group)
{
    return L"Local\\SynchainSCVB.v1.g" + std::to_wstring(group) + L"." + utf8ToWide(suffix);
}

bool fileExistsNonEmpty(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    return f.good() && f.tellg() > 0;
}

void writeHeader(std::ofstream& f)
{
    f << "ts_ms,group,out_sr,capture,out_enabled,out_state,out_mask";
    for (unsigned int c = 1; c <= scvb::kMaxChannels; ++c)
    {
        f << ",ch" << (c < 10 ? "0" : "") << c << "_state"
          << ",ch" << (c < 10 ? "0" : "") << c << "_sr"
          << ",ch" << (c < 10 ? "0" : "") << c << "_channels"
          << ",ch" << (c < 10 ? "0" : "") << c << "_gap"
          << ",ch" << (c < 10 ? "0" : "") << c << "_overlap"
          << ",ch" << (c < 10 ? "0" : "") << c << "_epoch";
    }
    f << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    Options o;
    std::string err;
    if (!parseArgs(argc, argv, o, err))
    {
        std::cerr << "参数错误: " << err << "\n\n";
        printUsage();
        return 1;
    }
    if (o.help)
    {
        printUsage();
        return 0;
    }

    const bool needHeader = !fileExistsNonEmpty(o.out);
    std::ofstream csv(o.out, std::ios::binary | std::ios::app);
    if (!csv)
    {
        std::cerr << "无法打开输出文件: " << o.out << '\n';
        return 1;
    }
    if (needHeader)
    {
        writeHeader(csv);
        csv.flush();
    }

    ReadView registry;
    ReadView ctrl;
    ReadView audio[scvb::kMaxChannels];

    std::cout << "scvb_diag: 输出 " << o.out << " (group " << o.group << ", 每 " << o.intervalMs
              << "ms 一行),Ctrl+C 停止。\n";

    const auto steady = []() { return static_cast<unsigned long long>(::GetTickCount64()); };

    for (;;)
    {
        const bool regOk = registry.open(segName("registry", o.group));
        const bool ctrlOk = ctrl.open(segName("ctrl", o.group));
        for (unsigned int c = 1; c <= scvb::kMaxChannels; ++c)
        {
            audio[c - 1].open(segName("audio.ch" + std::to_string(c), o.group));
        }

        // 全局
        const auto* outSlot = regOk ? reinterpret_cast<const scvb::OutputSlot*>(
                                          static_cast<const char*>(registry.base) + sizeof(scvb::RegistryHeader) +
                                          scvb::kMaxChannels * sizeof(scvb::InputSlot))
                                    : nullptr;
        const auto* info = ctrlOk ? static_cast<const scvb::OutputGlobalInfo*>(ctrl.base) : nullptr;

        csv << steady() << ',' << o.group << ',' << (info ? info->output_sample_rate.load() : 0u) << ','
            << (info ? info->capture_enabled.load() : 0u) << ',' << (info ? info->flags.load() : 0u) << ','
            << (outSlot ? outSlot->state.load() : 0u) << ',' << (outSlot ? outSlot->connected_mask.load() : 0u);

        for (unsigned int c = 1; c <= scvb::kMaxChannels; ++c)
        {
            const auto* slot = regOk ? reinterpret_cast<const scvb::InputSlot*>(
                                           static_cast<const char*>(registry.base) + sizeof(scvb::RegistryHeader) +
                                           (c - 1) * sizeof(scvb::InputSlot))
                                     : nullptr;
            const auto* ringHdr =
                audio[c - 1].base != nullptr ? static_cast<const scvb::AudioRingHeader*>(audio[c - 1].base) : nullptr;

            csv << ',' << (slot ? slot->state.load() : 0u) << ',' << (slot ? slot->sample_rate : 0u) << ','
                << (ringHdr ? ringHdr->channels : 0u) << ',' << (info ? info->gap_count[c - 1].load() : 0u) << ','
                << (info ? info->overlap_count[c - 1].load() : 0u) << ','
                << (info ? info->epoch_summary[c - 1].load() : 0u);
        }
        csv << '\n';
        csv.flush();

        ::Sleep(static_cast<DWORD>(o.intervalMs));
    }
    return 0;
}
