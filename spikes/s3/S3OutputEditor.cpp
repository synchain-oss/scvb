// SPDX-License-Identifier: GPL-3.0-or-later
#include "S3OutputEditor.h"

#include "BinaryData.h" // 由 juce_add_binary_data 生成

#include <cmath>
#include <cstring>

#if JUCE_WINDOWS
// WebView2 静态 loader(JUCE NEEDS_WEBVIEW2 + 静态链接)导出;ole32 提供 CoTaskMemFree。
extern "C" {
long __stdcall GetAvailableCoreWebView2BrowserVersionString(const wchar_t* browserExecutableFolder,
                                                            wchar_t** versionInfo);
void __stdcall CoTaskMemFree(void* pv);
}
#endif

namespace
{
using WBC = juce::WebBrowserComponent;

// 设计盒真源 05 §1.2(设计定稿回流①):Output 1180×780。
constexpr int kDesignW = 1180;
constexpr int kDesignH = 780;
constexpr int kTracks = 15;
constexpr int kStereoFrom = 14; // V14/V15 为 stereo([J57])

// 确定性伪随机波形(与 web 侧 mock-data.js 同构)。
double dbAt(int ch, double t)
{
    const double env = 0.55 + 0.45 * std::sin(t * 0.11 + ch * 0.7);
    const double v = -24.0 + 18.0 * env *
                                 (0.5 * std::sin(t * 1.7 + ch * 0.9) + 0.3 * std::sin(t * 5.3 + ch * 1.3) +
                                  0.2 * std::sin(t * 11.1 + ch * 2.1));
    return juce::jlimit(-60.0, 0.0, v);
}

// WebView2 缺失/超时时的最小原生兜底面板(spike 只给信息文案;完整版归 T26/T27)。
class FallbackPanel final : public juce::Component
{
public:
    FallbackPanel()
    {
        text_.setText("WebView2 Runtime unavailable or load timed out.\n"
                      "SCVB S3 spike audio stays available (passthrough).",
                      juce::dontSendNotification);
        text_.setJustificationType(juce::Justification::centred);
        text_.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(text_);
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff18161d)); }

    void resized() override { text_.setBounds(getLocalBounds().reduced(24)); }

private:
    juce::Label text_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FallbackPanel)
};
} // namespace

S3OutputEditor::S3OutputEditor(S3OutputProcessor& p)
    : juce::AudioProcessorEditor(&p), processor_(p), webView_(makeOptions())
{
    addAndMakeVisible(webView_);
    setResizable(false, false);
    {
        const float s = processor_.getUiScale();
        setSize(juce::roundToInt(kDesignW * s), juce::roundToInt(kDesignH * s));
    }

    if (webView2RuntimeAvailable())
    {
        webView_.goToURL(WBC::getResourceProviderRoot());
        startMs_ = juce::Time::getMillisecondCounter();
        startTimerHz(25);
    }
    else
    {
        webView_.setVisible(false);
        showFallback();
    }
}

S3OutputEditor::~S3OutputEditor()
{
    stopTimer();
}

void S3OutputEditor::resized()
{
    webView_.setBounds(getLocalBounds());
    if (fallback_ != nullptr)
    {
        fallback_->setBounds(getLocalBounds());
    }
}

bool S3OutputEditor::webView2RuntimeAvailable()
{
#if JUCE_WINDOWS
    wchar_t* version = nullptr;
    const long hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    const bool ok = hr >= 0 && version != nullptr && version[0] != L'\0';
    if (version != nullptr)
    {
        CoTaskMemFree(version);
    }
    return ok;
#else
    return true;
#endif
}

void S3OutputEditor::showFallback()
{
    if (fallback_ != nullptr)
    {
        return;
    }
    webView_.setVisible(false);
    fallback_ = std::make_unique<FallbackPanel>();
    addAndMakeVisible(*fallback_);
    resized();
}

juce::WebBrowserComponent::Options S3OutputEditor::makeOptions()
{
    WBC::Options options;

#if JUCE_WINDOWS
    WBC::Options::WinWebView2 wv2;
    wv2 = wv2.withUserDataFolder(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SCVBS3WV2"));
    options = options.withBackend(WBC::Options::Backend::webview2).withWinWebView2Options(wv2);
#endif

    // 通用回执(所有非核心写函数)。
    auto genericOk = [](const juce::Array<juce::var>&, WBC::NativeFunctionCompletion complete) {
        auto* o = new juce::DynamicObject();
        o->setProperty("ok", true);
        complete(juce::var(o));
    };

    options = options.withNativeIntegrationEnabled()
                  .withResourceProvider([this](const juce::String& url) { return provideResource(url); },
                                        juce::URL(WBC::getResourceProviderRoot()).getOrigin())
                  .withInitialisationData("version", juce::var(juce::String(JucePlugin_VersionString)))
                  .withInitialisationData("lang", juce::var(juce::String("zh")))
                  .withInitialisationData("uiScale", juce::var(processor_.getUiScale()))
                  .withNativeFunction(juce::Identifier("requestInitialState"),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleRequestInitialState(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier("requestWaveform"),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleRequestWaveform(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier("setUiScale"),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleSetUiScale(a, std::move(c));
                                      })
                  .withNativeFunction(juce::Identifier("commitUiScale"),
                                      [this](const juce::Array<juce::var>& a, WBC::NativeFunctionCompletion c) {
                                          handleCommitUiScale(a, std::move(c));
                                      });

    // 05 §1.4 其余原生函数(统一 {ok:true} 回执;spike 不做真实语义)。
    for (const char* name : {"setCaptureEnabled",
                             "setOutputEnabled",
                             "setGroupId",
                             "previewAnalyze",
                             "analyze",
                             "cancelAnalyze",
                             "setRange",
                             "setVersionActive",
                             "setVersionName",
                             "copyVersion",
                             "beginParamGesture",
                             "setParam",
                             "endParamGesture",
                             "setChannelConfig",
                             "setTrackManual",
                             "setPanCurve",
                             "setVadParams",
                             "setSegmentation",
                             "setTransitionRamp",
                             "editSegment",
                             "recaptureArm",
                             "clearCoverage",
                             "undo",
                             "redo",
                             "setLang",
                             "setActiveTab",
                             "setGuideSeen",
                             "setTourSeen"})
    {
        options = options.withNativeFunction(juce::Identifier(name), genericOk);
    }

    return options;
}

std::optional<juce::WebBrowserComponent::Resource> S3OutputEditor::provideResource(const juce::String& url) const
{
    const auto path =
        (url == "/" || url.isEmpty()) ? juce::String("index.html") : url.fromFirstOccurrenceOf("/", false, false);
    const auto baseName = path.fromLastOccurrenceOf("/", false, false);

    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        if (juce::String(BinaryData::originalFilenames[i]) == baseName)
        {
            int size = 0;
            if (const char* data = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size))
            {
                std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(data),
                                             reinterpret_cast<const std::byte*>(data) + size);
                const auto ext = baseName.fromLastOccurrenceOf(".", false, false);
                return WBC::Resource{std::move(bytes), juce::String(mimeForExtension(ext))};
            }
        }
    }
    return std::nullopt;
}

const char* S3OutputEditor::mimeForExtension(const juce::String& ext)
{
    const auto e = ext.toLowerCase();
    if (e == "html" || e == "htm")
        return "text/html";
    if (e == "css")
        return "text/css";
    if (e == "js" || e == "mjs")
        return "text/javascript"; // ES module 必须是 JS MIME
    if (e == "json")
        return "application/json";
    if (e == "svg")
        return "image/svg+xml";
    if (e == "woff2")
        return "font/woff2";
    if (e == "png")
        return "image/png";
    return "application/octet-stream";
}

void S3OutputEditor::handleRequestInitialState(const juce::Array<juce::var>&, WBC::NativeFunctionCompletion complete)
{
    bridgeReady_ = true;
    complete(buildSnapshot());
}

void S3OutputEditor::handleRequestWaveform(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    const int ch = args.size() > 0 ? static_cast<int>(args[0]) : 1;
    const double startS = args.size() > 1 ? static_cast<double>(args[1]) : 0.0;
    const double endS = args.size() > 2 ? static_cast<double>(args[2]) : 300.0;
    const int cols = args.size() > 3 ? static_cast<int>(args[3]) : 1180;
    complete(buildWaveform(ch, startS, endS, cols));
}

void S3OutputEditor::handleSetUiScale(const juce::Array<juce::var>& args, WBC::NativeFunctionCompletion complete)
{
    const double f = args.size() > 0 ? static_cast<double>(args[0]) : 1.0;
    processor_.setUiScale(static_cast<float>(f));
    setSize(juce::roundToInt(kDesignW * processor_.getUiScale()), juce::roundToInt(kDesignH * processor_.getUiScale()));
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", true);
    o->setProperty("scale", processor_.getUiScale());
    o->setProperty("w", getWidth());
    o->setProperty("h", getHeight());
    complete(juce::var(o));
}

void S3OutputEditor::handleCommitUiScale(const juce::Array<juce::var>&, WBC::NativeFunctionCompletion complete)
{
    processor_.persistUiScaleAsDefault();
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", true);
    o->setProperty("scale", processor_.getUiScale());
    complete(juce::var(o));
}

juce::var S3OutputEditor::buildSnapshot() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty("group_id", 1);
    root->setProperty("version_active", 1);

    auto* global = new juce::DynamicObject();
    global->setProperty("width", 100);
    global->setProperty("ms_balance", 0);
    global->setProperty("lead_select", 0);
    root->setProperty("global", juce::var(global));

    auto* analysis = new juce::DynamicObject();
    analysis->setProperty("transition_ramp_ms", 80);
    root->setProperty("analysis", juce::var(analysis));

    // 2 版曲线(每版 5 点;参数面真源 params-v0)
    juce::Array<juce::var> versions;
    for (int v = 1; v <= 2; ++v)
    {
        auto* ver = new juce::DynamicObject();
        ver->setProperty("v", v);
        ver->setProperty("name", "V" + juce::String(v));
        juce::Array<juce::var> pts;
        const double angles[5] = {-100.0, -50.0, 0.0, 50.0, 100.0};
        const double gains[5] = {0.0, -3.0, 0.0, 2.5, 0.0};
        for (int i = 0; i < 5; ++i)
        {
            auto* p = new juce::DynamicObject();
            p->setProperty("angle", angles[i]);
            p->setProperty("gain_db", gains[i]);
            p->setProperty("shape", "bell");
            p->setProperty("q", 1.0);
            p->setProperty("side", "out");
            pts.add(juce::var(p));
        }
        ver->setProperty("pan_curve", juce::var(pts));
        versions.add(juce::var(ver));
    }
    root->setProperty("versions", juce::var(versions));

    // 15 轨(13 mono + 2 stereo)
    juce::Array<juce::var> channels;
    for (int ch = 1; ch <= kTracks; ++ch)
    {
        auto* c = new juce::DynamicObject();
        const bool stereo = ch >= kStereoFrom;
        c->setProperty("ch", ch);
        c->setProperty("label", "Track " + juce::String(ch).paddedLeft('0', 2));
        c->setProperty("priority", 1 + (ch % 15));
        c->setProperty("lead", ch == 1);
        c->setProperty("lead_vol_exempt", false);
        c->setProperty("participate_in_auto_pan", !stereo && (ch % 3 == 0));
        c->setProperty("pair_id", (ch % 7 == 0) ? 1 + (ch % 7) : 0);
        c->setProperty("source_channels", stereo ? 2 : 1);
        c->setProperty("enabled", true);
        c->setProperty("pan", ((ch * 13) % 200) - 100);
        c->setProperty("width", stereo ? ((ch * 17) % 100) : 0);
        c->setProperty("vol", ((ch * 7) % 36) - 24);
        c->setProperty("freeze", 0);
        channels.add(juce::var(c));
    }
    root->setProperty("channels", juce::var(channels));

    auto* ui = new juce::DynamicObject();
    ui->setProperty("lang", "zh");
    root->setProperty("ui", juce::var(ui));

    return juce::var(root);
}

juce::var S3OutputEditor::buildWaveform(int ch, double startS, double endS, int cols) const
{
    const int n = juce::jmax(1, cols);
    juce::Array<juce::var> minDb;
    juce::Array<juce::var> maxDb;
    juce::Array<juce::var> vad;
    juce::Array<juce::var> covered;
    juce::Array<juce::var> stale;
    juce::Array<juce::var> passId;

    const double span = juce::jmax(0.0001, endS - startS);
    for (int c = 0; c < n; ++c)
    {
        const double t0 = startS + (static_cast<double>(c) / n) * span;
        const double t1 = startS + (static_cast<double>(c + 1) / n) * span;
        double lo = 1e9;
        double hi = -1e9;
        for (int s = 0; s < 4; ++s)
        {
            const double t = t0 + ((t1 - t0) * s) / 4.0;
            const double v = dbAt(ch, t);
            lo = juce::jmin(lo, v);
            hi = juce::jmax(hi, v);
        }
        minDb.add(lo);
        maxDb.add(hi);
        const double mid = (t0 + t1) / 2.0;
        vad.add(dbAt(ch, mid) > -45.0 ? 1 : 0);
        covered.add(std::fmod(mid, 40.0) < 32.0 ? 1 : 0);
        stale.add(mid > 288.0 ? 1 : 0);
        passId.add(1 + static_cast<int>((mid / 300.0) * 8.0));
    }

    auto* o = new juce::DynamicObject();
    o->setProperty("minDb", juce::var(minDb));
    o->setProperty("maxDb", juce::var(maxDb));
    o->setProperty("vad", juce::var(vad));
    o->setProperty("covered", juce::var(covered));
    o->setProperty("stale", juce::var(stale));
    o->setProperty("passId", juce::var(passId));
    return juce::var(o);
}

void S3OutputEditor::timerCallback()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (lastTimerTick_ > 0.0)
    {
        const double dt = nowMs - lastTimerTick_;
        if (dt > 0.0)
        {
            const float hz = static_cast<float>(1000.0 / dt);
            timerHz25_ = (timerHz25_ <= 0.0f) ? hz : timerHz25_ * 0.95f + hz * 0.05f;
        }
    }
    lastTimerTick_ = nowMs;

    // 5s 看门狗:前端未就绪 → 兜底。
    if (!bridgeReady_ && fallback_ == nullptr && juce::Time::getMillisecondCounter() - startMs_ > 5000)
    {
        showFallback();
        return;
    }
    if (!bridgeReady_)
    {
        return;
    }

    // 伪随机推进(25Hz diff-then-emit)。
    meterPhase_ += 0.5;
    playheadS_ = std::fmod(playheadS_ + 1.0 / 25.0, 300.0);

    // scvb.meters:15 路 + 总线 L/R
    {
        auto* m = new juce::DynamicObject();
        juce::Array<juce::var> tracks;
        for (int ch = 1; ch <= kTracks; ++ch)
        {
            auto* t = new juce::DynamicObject();
            const double db = dbAt(ch, playheadS_);
            t->setProperty("db", db);
            t->setProperty("peakDb", juce::jmin(0.0, db + 3.0));
            tracks.add(juce::var(t));
        }
        m->setProperty("tracks", juce::var(tracks));
        m->setProperty("busL", dbAt(1, playheadS_));
        m->setProperty("busR", dbAt(2, playheadS_));
        webView_.emitEventIfBrowserIsVisible(juce::Identifier("scvb.meters"), juce::var(m));
    }

    // scvb.playhead
    {
        auto* p = new juce::DynamicObject();
        p->setProperty("timeS", playheadS_);
        p->setProperty("isPlaying", true);
        p->setProperty("inRange", true);
        webView_.emitEventIfBrowserIsVisible(juce::Identifier("scvb.playhead"), juce::var(p));
    }

    // scvb.state:含 50Hz/25Hz Timer 实测频率(验收④的量化依据)。
    {
        auto* s = new juce::DynamicObject();
        s->setProperty("timer50hz", processor_.timerHz50());
        s->setProperty("timer25hz", timerHz25_);
        s->setProperty("uiScale", processor_.getUiScale());
        webView_.emitEventIfBrowserIsVisible(juce::Identifier("scvb.state"), juce::var(s));
    }
}
