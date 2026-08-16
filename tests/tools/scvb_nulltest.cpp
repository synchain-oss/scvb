// SPDX-License-Identifier: GPL-3.0-or-later
// scvb_nulltest —— wav null test 比对器 + click 对位素材生成器。
// 任务 T01c(07-execution-plan §2.4);功能真源 = 10-validation §4.2。
// 零外部依赖:WAV 读写 + 整数互相关 + 残差统计全部自研,不链接 JUCE / Catch2。
// 用途:S1 透明性 null(模式 A)的逐样本判据来源;S1-P21 声道语义判据依赖逐声道残差(J57)。

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
constexpr int kExitOk = 0;
constexpr int kExitError = 1;

struct WavFile
{
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint32_t numFrames = 0;
    std::vector<float> samples; // interleaved, size = numFrames * channels
};

struct Options
{
    bool genClick = false;
    bool align = false;
    bool help = false;
    std::string refPath;
    std::string testPath;
    std::string clickPath;
    std::string jsonPath;
    double gainDb = 0.0;
    double thresholdDb = -120.0;
    int tracks = 15;
    int stereoTracks = 2;
    int seconds = 30;
    int sampleRate = 48000;
};

uint32_t fourcc(const char* s)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

uint16_t readU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// 24-bit PCM(小端),符号扩展到 int32。
int32_t readI24(const uint8_t* p)
{
    int32_t v = static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) | (static_cast<int32_t>(p[2]) << 16);
    if ((v & 0x00800000) != 0)
    {
        v -= 0x01000000;
    }
    return v;
}

uint32_t floatBits(float x)
{
    uint32_t u = 0;
    std::memcpy(&u, &x, sizeof(u));
    return u;
}

void putU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void putU32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void putBytes(std::vector<uint8_t>& out, const void* p, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

bool readWav(const std::string& path, WavFile& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        err = "无法打开文件: " + path;
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    {
        err = "不是 RIFF/WAVE 文件: " + path;
        return false;
    }

    bool haveFmt = false;
    bool haveData = false;
    uint16_t fmtTag = 0;
    uint16_t channels = 0;
    uint16_t blockAlign = 0;
    uint16_t bits = 0;
    uint32_t sampleRate = 0;
    uint32_t dataOffset = 0;
    uint32_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= bytes.size())
    {
        const uint32_t id = readU32(bytes.data() + pos);
        const uint32_t size = readU32(bytes.data() + pos + 4);
        const uint8_t* body = bytes.data() + pos + 8;
        if (pos + 8 + static_cast<size_t>(size) > bytes.size())
        {
            err = "chunk 截断: " + path;
            return false;
        }
        if (id == fourcc("fmt ") && !haveFmt)
        {
            if (size < 16)
            {
                err = "fmt chunk 过短: " + path;
                return false;
            }
            fmtTag = readU16(body);
            channels = readU16(body + 2);
            sampleRate = readU32(body + 4);
            blockAlign = readU16(body + 12);
            bits = readU16(body + 14);
            if (fmtTag == 0xFFFE) // WAVE_FORMAT_EXTENSIBLE:SubFormat GUID 前 2 字节即有效格式码
            {
                if (size < 40)
                {
                    err = "WAVE_FORMAT_EXTENSIBLE fmt chunk 过短: " + path;
                    return false;
                }
                fmtTag = readU16(body + 24);
            }
            haveFmt = true;
        }
        else if (id == fourcc("data") && !haveData)
        {
            dataOffset = static_cast<uint32_t>(pos + 8);
            dataSize = size;
            haveData = true;
        }
        pos += 8 + static_cast<size_t>(size) + (size & 1u); // chunk 大小按偶数字节对齐
    }

    if (!haveFmt || !haveData)
    {
        err = "缺少 fmt 或 data chunk: " + path;
        return false;
    }
    if (channels == 0 || blockAlign == 0)
    {
        err = "fmt chunk 非法(channels/blockAlign 为 0): " + path;
        return false;
    }

    const uint32_t frames = dataSize / blockAlign;
    out.sampleRate = sampleRate;
    out.channels = channels;
    out.numFrames = frames;
    out.samples.resize(static_cast<size_t>(frames) * channels);

    const uint8_t* d = bytes.data() + dataOffset;
    size_t src = 0;
    for (uint32_t fr = 0; fr < frames; ++fr)
    {
        for (uint16_t ch = 0; ch < channels; ++ch)
        {
            float v = 0.0f;
            if (fmtTag == 3) // IEEE float
            {
                if (bits == 32)
                {
                    const uint32_t u = readU32(d + src);
                    std::memcpy(&v, &u, sizeof(v));
                    src += 4;
                }
                else if (bits == 64)
                {
                    uint64_t u = 0;
                    std::memcpy(&u, d + src, sizeof(u));
                    const double dv = *reinterpret_cast<double*>(&u);
                    v = static_cast<float>(dv);
                    src += 8;
                }
                else
                {
                    err = "不支持的浮点位深: " + path;
                    return false;
                }
            }
            else if (fmtTag == 1) // PCM
            {
                if (bits == 16)
                {
                    const int16_t s = static_cast<int16_t>(readU16(d + src));
                    v = static_cast<float>(s) / 32768.0f;
                    src += 2;
                }
                else if (bits == 24)
                {
                    const int32_t s = readI24(d + src);
                    v = static_cast<float>(s) / 8388608.0f;
                    src += 3;
                }
                else if (bits == 32)
                {
                    const int32_t s = static_cast<int32_t>(readU32(d + src));
                    v = static_cast<float>(s) / 2147483648.0f;
                    src += 4;
                }
                else
                {
                    err = "不支持的 PCM 位深: " + path;
                    return false;
                }
            }
            else
            {
                err = "不支持的格式码(fmtTag): " + path;
                return false;
            }
            out.samples[static_cast<size_t>(fr) * channels + ch] = v;
        }
    }
    return true;
}

bool writeWavFloat(const std::string& path, int sampleRate, int channels, const std::vector<float>& samples,
                   std::string& err)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
    {
        err = "无法创建文件: " + path;
        return false;
    }

    const uint16_t ch = static_cast<uint16_t>(channels);
    const uint32_t sr = static_cast<uint32_t>(sampleRate);
    const uint16_t blockAlign = static_cast<uint16_t>(ch * 4u);
    const uint32_t byteRate = sr * blockAlign;
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    const uint32_t riffSize = 36u + dataBytes;

    std::vector<uint8_t> h;
    putBytes(h, "RIFF", 4);
    putU32(h, riffSize);
    putBytes(h, "WAVE", 4);
    putBytes(h, "fmt ", 4);
    putU32(h, 16);
    putU16(h, 3); // IEEE float
    putU16(h, ch);
    putU32(h, sr);
    putU32(h, byteRate);
    putU16(h, blockAlign);
    putU16(h, 32);
    putBytes(h, "data", 4);
    putU32(h, dataBytes);

    f.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
    f.write(reinterpret_cast<const char*>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(float)));
    f.flush();
    if (!f)
    {
        err = "写入失败: " + path;
        return false;
    }
    return true;
}

std::vector<double> downmix(const WavFile& w)
{
    std::vector<double> m(static_cast<size_t>(w.numFrames), 0.0);
    for (uint32_t f = 0; f < w.numFrames; ++f)
    {
        double sum = 0.0;
        for (uint16_t c = 0; c < w.channels; ++c)
        {
            sum += w.samples[static_cast<size_t>(f) * w.channels + c];
        }
        m[f] = sum / w.channels;
    }
    return m;
}

// 整数互相关求样本偏移:正偏移 = test 相对 ref 滞后。窗口与滞后范围均受限,避免 O(N^2)。
int64_t findOffset(const std::vector<double>& a, const std::vector<double>& b, int64_t frames, int64_t fs)
{
    if (frames < 2 || fs < 1)
    {
        return 0;
    }
    const int64_t lagLimit = std::min<int64_t>(fs, (frames - 1) / 2);
    const int64_t window = std::min<int64_t>(fs, frames - 2 * lagLimit);
    if (window < 1)
    {
        return 0;
    }

    int64_t bestLag = 0;
    double best = -std::numeric_limits<double>::infinity();
    for (int64_t lag = -lagLimit; lag <= lagLimit; ++lag)
    {
        double sum = 0.0;
        for (int64_t n = 0; n < window; ++n)
        {
            sum += a[static_cast<size_t>(lagLimit + n)] * b[static_cast<size_t>(lagLimit + n + lag)];
        }
        // 周期信号(如 click 串)在周期整数倍处互相关并列最高:取绝对值最小的 lag,保证
        // 自比对/无偏移时报告 0,而非周期倍数(否则 S1 的「样本偏移 == 0」判据会被周期性误判)。
        const int64_t absLag = lag < 0 ? -lag : lag;
        const int64_t absBest = bestLag < 0 ? -bestLag : bestLag;
        if (sum > best || (sum == best && absLag < absBest))
        {
            best = sum;
            bestLag = lag;
        }
    }
    return bestLag;
}

double dbToLinear(double db)
{
    return std::pow(10.0, db / 20.0);
}

double toDb(double linear)
{
    if (linear <= 0.0)
    {
        return -std::numeric_limits<double>::infinity();
    }
    return 20.0 * std::log10(linear);
}

std::string dbString(double db)
{
    if (db == -std::numeric_limits<double>::infinity())
    {
        return "-inf";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", db);
    return std::string(buf);
}

std::string zeroPad(int value, int width)
{
    std::string s = std::to_string(value);
    while (static_cast<int>(s.size()) < width)
    {
        s = "0" + s;
    }
    return s;
}

void splitPath(const std::string& path, std::string& stem, std::string& ext)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    {
        stem = path.substr(0, dot);
        ext = path.substr(dot);
    }
    else
    {
        stem = path;
        ext = ".wav";
    }
}

void printUsage(std::ostream& os)
{
    os << "scvb_nulltest —— wav null test 比对器 + click 对位素材生成器(10-validation §4.2)\n"
       << "\n"
       << "比对(默认):\n"
       << "  scvb_nulltest <ref.wav> <test.wav> [--gain-db <db>] [--align] [--threshold-db <db>] [--json <out.json>]\n"
       << "    输出: 样本偏移(互相关求得;--align 则先对齐再比)、残差峰值/RMS dBFS(分声道 + 合并)、\n"
       << "          首个超阈值样本位置、是否按位相等。--gain-db 为施加于 test 的固定补偿增益(dB)。\n"
       << "\n"
       << "生成对位素材:\n"
       << "  scvb_nulltest --gen-click <out.wav> [--tracks <n>] [--stereo-tracks <n>] [--seconds <s>] [--fs <hz>]\n"
       << "    按 out.wav 的 stem 生成 N 个文件 <stem>_<NN>.wav(NN 从 1 起零填充);最后 --stereo-tracks 条\n"
       << "    为双通道,L 脉冲在偶数样本、R 脉冲在奇数样本(互换 L/R 会在逐声道残差里立即可见,J57)。\n"
       << "\n"
       << "选项默认:--tracks 15 --stereo-tracks 2 --seconds 30 --fs 48000 --threshold-db -120.0\n";
}

bool parseInt(const std::string& s, int& out)
{
    if (s.empty())
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || errno != 0 || v < INT_MIN || v > INT_MAX)
    {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool parseDouble(const std::string& s, double& out)
{
    if (s.empty())
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || errno != 0)
    {
        return false;
    }
    out = v;
    return true;
}

std::string requireValue(const std::vector<std::string>& args, size_t& i)
{
    if (i + 1 >= args.size())
    {
        return std::string();
    }
    return args[++i];
}

int parseArgs(int argc, char** argv, Options& o, std::string& err)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    std::vector<std::string> positional;
    for (size_t i = 0; i < args.size(); ++i)
    {
        std::string a = args[i];
        std::string key = a;
        std::string inlineValue;
        const size_t eq = a.find('=');
        if (a.rfind("--", 0) == 0 && eq != std::string::npos)
        {
            key = a.substr(0, eq);
            inlineValue = a.substr(eq + 1);
        }

        auto next = [&]() -> std::string {
            if (!inlineValue.empty())
            {
                return inlineValue;
            }
            return requireValue(args, i);
        };

        if (key == "--help" || key == "-h")
        {
            o.help = true;
            return kExitOk;
        }
        if (key == "--gen-click")
        {
            o.genClick = true;
            o.clickPath = next();
            if (o.clickPath.empty())
            {
                err = "--gen-click 需要一个输出路径参数";
                return kExitError;
            }
        }
        else if (key == "--align")
        {
            o.align = true;
        }
        else if (key == "--gain-db")
        {
            const std::string v = next();
            if (v.empty() || !parseDouble(v, o.gainDb))
            {
                err = "--gain-db 需要数值参数(dB)";
                return kExitError;
            }
        }
        else if (key == "--threshold-db")
        {
            const std::string v = next();
            if (v.empty() || !parseDouble(v, o.thresholdDb))
            {
                err = "--threshold-db 需要数值参数(dBFS)";
                return kExitError;
            }
        }
        else if (key == "--json")
        {
            o.jsonPath = next();
            if (o.jsonPath.empty())
            {
                err = "--json 需要输出文件路径";
                return kExitError;
            }
        }
        else if (key == "--tracks")
        {
            const std::string v = next();
            if (v.empty() || !parseInt(v, o.tracks))
            {
                err = "--tracks 需要整数参数";
                return kExitError;
            }
        }
        else if (key == "--stereo-tracks")
        {
            const std::string v = next();
            if (v.empty() || !parseInt(v, o.stereoTracks))
            {
                err = "--stereo-tracks 需要整数参数";
                return kExitError;
            }
        }
        else if (key == "--seconds")
        {
            const std::string v = next();
            if (v.empty() || !parseInt(v, o.seconds))
            {
                err = "--seconds 需要整数参数";
                return kExitError;
            }
        }
        else if (key == "--fs")
        {
            const std::string v = next();
            if (v.empty() || !parseInt(v, o.sampleRate))
            {
                err = "--fs 需要整数参数(Hz)";
                return kExitError;
            }
        }
        else if (a.rfind("-", 0) == 0 && a.size() > 1)
        {
            err = "未知选项: " + a;
            return kExitError;
        }
        else
        {
            positional.push_back(a);
        }
    }

    if (o.genClick)
    {
        return kExitOk;
    }
    if (positional.size() != 2)
    {
        err = "比对模式需要两个位置参数: <ref.wav> <test.wav>";
        return kExitError;
    }
    o.refPath = positional[0];
    o.testPath = positional[1];
    return kExitOk;
}

int runCompare(const Options& o, std::string& err)
{
    WavFile ref;
    WavFile test;
    if (!readWav(o.refPath, ref, err) || !readWav(o.testPath, test, err))
    {
        return kExitError;
    }
    if (ref.sampleRate != test.sampleRate)
    {
        err = "采样率不一致: ref=" + std::to_string(ref.sampleRate) + " test=" + std::to_string(test.sampleRate);
        return kExitError;
    }
    if (ref.channels != test.channels)
    {
        err = "声道数不一致: ref=" + std::to_string(ref.channels) + " test=" + std::to_string(test.channels);
        return kExitError;
    }

    const uint16_t channels = ref.channels;
    const int64_t refFrames = static_cast<int64_t>(ref.numFrames);
    const int64_t testFrames = static_cast<int64_t>(test.numFrames);

    const std::vector<double> refM = downmix(ref);
    const std::vector<double> testM = downmix(test);
    const int64_t offset =
        findOffset(refM, testM, std::min<int64_t>(refFrames, testFrames), static_cast<int64_t>(ref.sampleRate));

    const double gain = (o.gainDb == 0.0) ? 1.0 : dbToLinear(o.gainDb);
    const int64_t off = o.align ? offset : 0;

    const int64_t start = std::max<int64_t>(0, -off);
    const int64_t end = std::min<int64_t>(refFrames, testFrames - off);
    const int64_t compared = (end > start) ? (end - start) : 0;

    std::vector<double> peak(channels, 0.0);
    std::vector<double> sumSq(channels, 0.0);
    bool bitExact = true;
    int64_t firstOverFrame = -1;
    int firstOverChannel = -1;
    const double threshold = dbToLinear(o.thresholdDb);

    if (compared > 0)
    {
        for (int64_t i = start; i < end; ++i)
        {
            const int64_t ti = i + off;
            for (uint16_t c = 0; c < channels; ++c)
            {
                const float r = ref.samples[static_cast<size_t>(i) * channels + c];
                const float ts = test.samples[static_cast<size_t>(ti) * channels + c];
                const float t = (o.gainDb == 0.0) ? ts : static_cast<float>(static_cast<double>(ts) * gain);
                if (floatBits(r) != floatBits(t))
                {
                    bitExact = false;
                }
                const double d = static_cast<double>(r) - static_cast<double>(t);
                const double ad = std::fabs(d);
                if (ad > peak[c])
                {
                    peak[c] = ad;
                }
                sumSq[c] += d * d;
                if (firstOverFrame < 0 && ad > threshold)
                {
                    firstOverFrame = i;
                    firstOverChannel = static_cast<int>(c);
                }
            }
        }
    }

    double mergedPeak = 0.0;
    double mergedSumSq = 0.0;
    std::vector<double> peakDb(channels, 0.0);
    std::vector<double> rmsDb(channels, 0.0);
    for (uint16_t c = 0; c < channels; ++c)
    {
        mergedPeak = std::max(mergedPeak, peak[c]);
        mergedSumSq += sumSq[c];
        peakDb[c] = toDb(peak[c]);
        rmsDb[c] = toDb(std::sqrt(compared > 0 ? sumSq[c] / static_cast<double>(compared) : 0.0));
    }
    const double mergedPeakDb = toDb(mergedPeak);
    const double mergedRmsDb =
        toDb(std::sqrt(compared > 0 ? mergedSumSq / (static_cast<double>(compared) * channels) : 0.0));

    // 控制台输出
    std::cout << "ref:              " << o.refPath << '\n';
    std::cout << "test:             " << o.testPath << '\n';
    std::cout << "channels:         " << channels << '\n';
    std::cout << "sample_rate:      " << ref.sampleRate << '\n';
    std::cout << "frames:           " << refFrames << " (ref) / " << testFrames << " (test)\n";
    std::cout << "frames_compared:  " << compared << '\n';
    std::cout << "sample_offset:    " << offset << (o.align ? " (aligned)" : " (未对齐)") << '\n';
    std::cout << "gain_db:          " << dbString(o.gainDb) << '\n';
    std::cout << "residual_peak_db: merged " << dbString(mergedPeakDb) << ", per-channel [";
    for (uint16_t c = 0; c < channels; ++c)
    {
        if (c > 0)
        {
            std::cout << ", ";
        }
        std::cout << dbString(peakDb[c]);
    }
    std::cout << "]\n";
    std::cout << "residual_rms_db:  merged " << dbString(mergedRmsDb) << ", per-channel [";
    for (uint16_t c = 0; c < channels; ++c)
    {
        if (c > 0)
        {
            std::cout << ", ";
        }
        std::cout << dbString(rmsDb[c]);
    }
    std::cout << "]\n";
    std::cout << "first_over_threshold (>= " << dbString(o.thresholdDb) << " dBFS): ";
    if (firstOverFrame < 0)
    {
        std::cout << "无\n";
    }
    else
    {
        std::cout << "frame " << firstOverFrame << " channel " << firstOverChannel << '\n';
    }
    std::cout << "bit_exact:        " << (bitExact ? "true" : "false") << '\n';

    if (!o.jsonPath.empty())
    {
        std::ofstream j(o.jsonPath, std::ios::binary);
        if (!j)
        {
            err = "无法创建 JSON 文件: " + o.jsonPath;
            return kExitError;
        }
        auto dbField = [](std::ostream& os, double db) {
            if (db == -std::numeric_limits<double>::infinity())
            {
                os << "\"-inf\"";
            }
            else
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.6f", db);
                os << buf;
            }
        };
        j << "{\n";
        j << "  \"ref\": \"" << o.refPath << "\",\n";
        j << "  \"test\": \"" << o.testPath << "\",\n";
        j << "  \"channels\": " << channels << ",\n";
        j << "  \"sample_rate\": " << ref.sampleRate << ",\n";
        j << "  \"frames\": " << refFrames << ",\n";
        j << "  \"frames_compared\": " << compared << ",\n";
        j << "  \"sample_offset\": " << offset << ",\n";
        j << "  \"aligned\": " << (o.align ? "true" : "false") << ",\n";
        j << "  \"gain_db\": " << o.gainDb << ",\n";
        j << "  \"bit_exact\": " << (bitExact ? "true" : "false") << ",\n";
        j << "  \"threshold_db\": " << o.thresholdDb << ",\n";
        j << "  \"residual_peak_db\": {\"merged\": ";
        dbField(j, mergedPeakDb);
        j << ", \"per_channel\": [";
        for (uint16_t c = 0; c < channels; ++c)
        {
            if (c > 0)
            {
                j << ", ";
            }
            dbField(j, peakDb[c]);
        }
        j << "]},\n";
        j << "  \"residual_rms_db\": {\"merged\": ";
        dbField(j, mergedRmsDb);
        j << ", \"per_channel\": [";
        for (uint16_t c = 0; c < channels; ++c)
        {
            if (c > 0)
            {
                j << ", ";
            }
            dbField(j, rmsDb[c]);
        }
        j << "]},\n";
        j << "  \"first_over_threshold_frame\": " << firstOverFrame << ",\n";
        j << "  \"first_over_threshold_channel\": " << firstOverChannel << "\n";
        j << "}\n";
        if (!j)
        {
            err = "写入 JSON 失败: " + o.jsonPath;
            return kExitError;
        }
    }
    return kExitOk;
}

int runGenClick(const Options& o, std::string& err)
{
    if (o.tracks <= 0)
    {
        err = "--tracks 必须 > 0";
        return kExitError;
    }
    if (o.stereoTracks < 0 || o.stereoTracks > o.tracks)
    {
        err = "--stereo-tracks 必须在 [0, tracks] 范围内";
        return kExitError;
    }
    if (o.seconds <= 0)
    {
        err = "--seconds 必须 > 0";
        return kExitError;
    }
    if (o.sampleRate <= 0)
    {
        err = "--fs 必须 > 0";
        return kExitError;
    }

    std::string stem;
    std::string ext;
    splitPath(o.clickPath, stem, ext);

    const int monoCount = o.tracks - o.stereoTracks;
    const int64_t frames = static_cast<int64_t>(o.seconds) * o.sampleRate;
    const int64_t period = std::max<int64_t>(1, o.sampleRate / 10); // 100ms 周期,10 click/s
    const int width = static_cast<int>(std::to_string(o.tracks).size());

    for (int t = 0; t < o.tracks; ++t)
    {
        const bool stereo = (t >= monoCount);
        const int channels = stereo ? 2 : 1;
        std::vector<float> samples(static_cast<size_t>(frames) * channels, 0.0f);

        if (stereo)
        {
            const int s = t - monoCount;
            const int64_t lOff = 2 * s; // L 脉冲在偶数样本
            const int64_t rOff = 2 * s + 1; // R 脉冲在奇数样本(与 L 错开 1 样本,互换可见)
            for (int64_t f = lOff; f < frames; f += period)
            {
                samples[static_cast<size_t>(f) * 2 + 0] = 1.0f;
            }
            for (int64_t f = rOff; f < frames; f += period)
            {
                samples[static_cast<size_t>(f) * 2 + 1] = 1.0f;
            }
        }
        else
        {
            for (int64_t f = t; f < frames; f += period)
            {
                samples[static_cast<size_t>(f)] = 1.0f;
            }
        }

        const std::string path = stem + "_" + zeroPad(t + 1, width) + ext;
        if (!writeWavFloat(path, o.sampleRate, channels, samples, err))
        {
            return kExitError;
        }
        std::cout << "已生成: " << path << " (" << channels << " ch, " << o.sampleRate << " Hz, " << o.seconds << " s, "
                  << frames << " frames)\n";
    }
    return kExitOk;
}
} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    // 控制台按 UTF-8 解释输出(工具文案为 UTF-8 中文;GBK 控制台下会乱码)。
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
#endif
    Options o;
    std::string err;
    const int rc = parseArgs(argc, argv, o, err);
    if (rc != kExitOk)
    {
        std::cerr << "参数错误: " << err << "\n\n";
        printUsage(std::cerr);
        return kExitError;
    }
    if (o.help)
    {
        printUsage(std::cout);
        return kExitOk;
    }

    const int runRc = o.genClick ? runGenClick(o, err) : runCompare(o, err);
    if (runRc != kExitOk)
    {
        std::cerr << "错误: " << err << '\n';
        return kExitError;
    }
    return kExitOk;
}
