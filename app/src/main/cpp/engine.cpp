// Oboe ile tam çift yönlü ses: iRig (kulaklık mikrofonu) girişi → DSP → kulaklık çıkışı
#include <jni.h>
#include <cstring>
#include <oboe/Oboe.h>
#include <android/log.h>
#include <mutex>
#include <thread>
#include <vector>
#include "dsp.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "IrigAmp", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "IrigAmp", __VA_ARGS__)

class Engine : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    amp::Dsp dsp;
    std::shared_ptr<oboe::AudioStream> in, out;
    std::mutex lock;
    std::atomic<bool> running{false};
    std::atomic<int> drain{0};
    std::vector<float> inBuf, monoIn, monoOut;
    int inCh = 1, outCh = 2, usedPreset = 0;
    int inDev = 0, outDev = 0;
    std::atomic<int> underruns{0};

    bool start(int inDeviceId, int outDeviceId) {
        std::lock_guard<std::mutex> g(lock);
        closeStreams();
        inDev = inDeviceId; outDev = outDeviceId;
        // 1) Çıkış: düşük gecikme, float, stereo
        oboe::AudioStreamBuilder ob;
        ob.setDirection(oboe::Direction::Output)
          ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
          ->setSharingMode(oboe::SharingMode::Exclusive)
          ->setFormat(oboe::AudioFormat::Float)
          ->setFormatConversionAllowed(true)
          ->setChannelCount(2)
          ->setChannelConversionAllowed(true)
          ->setUsage(oboe::Usage::Game)
          ->setContentType(oboe::ContentType::Music)
          ->setDataCallback(this)
          ->setErrorCallback(this);
        if (outDev > 0) ob.setDeviceId(outDev);
        oboe::Result r = ob.openStream(out);
        if (r != oboe::Result::OK) { LOGE("çıkış açılamadı: %s", oboe::convertToText(r)); return false; }
        outCh = out->getChannelCount();
        int sr = out->getSampleRate();

        // 2) Giriş: aynı örnekleme hızı, mono, ses işleme kapalı (Unprocessed → VoiceRecognition → Generic)
        const oboe::InputPreset presets[3] = {oboe::InputPreset::Unprocessed, oboe::InputPreset::VoiceRecognition, oboe::InputPreset::Generic};
        r = oboe::Result::ErrorInternal;
        for (int k = 0; k < 3 && r != oboe::Result::OK; k++) {
            oboe::AudioStreamBuilder ib;
            ib.setDirection(oboe::Direction::Input)
              ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
              ->setSharingMode(oboe::SharingMode::Exclusive)
              ->setFormat(oboe::AudioFormat::Float)
              ->setFormatConversionAllowed(true)
              ->setChannelCount(1)
              ->setChannelConversionAllowed(true)
              ->setSampleRate(sr)
              ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
              ->setInputPreset(presets[k]);
            if (inDev > 0) ib.setDeviceId(inDev);
            r = ib.openStream(in);
            if (r == oboe::Result::OK) usedPreset = k;
        }
        if (r != oboe::Result::OK) { LOGE("giriş açılamadı: %s", oboe::convertToText(r)); out->close(); out.reset(); return false; }
        inCh = in->getChannelCount();

        // Çıkış tamponunu en küçük güvenli değere indir (2 burst)
        out->setBufferSizeInFrames(out->getFramesPerBurst() * 2);
        int cap = std::max(out->getBufferCapacityInFrames(), 4096);
        inBuf.assign((size_t)cap * std::max(1, inCh), 0.0f);
        monoIn.assign(cap, 0.0f); monoOut.assign(cap, 0.0f);
        dsp.prepare((float)sr);
        drain.store(20);
        underruns.store(0);

        r = in->requestStart();
        if (r != oboe::Result::OK) { LOGE("giriş başlatılamadı"); closeStreams(); return false; }
        r = out->requestStart();
        if (r != oboe::Result::OK) { LOGE("çıkış başlatılamadı"); closeStreams(); return false; }
        running.store(true);
        LOGI("başladı sr=%d burst=%d preset=%d", sr, out->getFramesPerBurst(), usedPreset);
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> g(lock);
        running.store(false);
        closeStreams();
    }

    void closeStreams() {
        if (out) { out->stop(); out->close(); out.reset(); }
        if (in) { in->stop(); in->close(); in.reset(); }
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *, void *audioData, int32_t numFrames) override {
        float *o = static_cast<float *>(audioData);
        auto inS = in;  // yerel kopya
        if (!inS || numFrames <= 0 || (size_t)numFrames > monoIn.size()) {
            std::fill(o, o + (size_t)numFrames * outCh, 0.0f);
            return oboe::DataCallbackResult::Continue;
        }
        // Başlangıçta birikmiş girişi boşalt (gecikme düşük kalsın)
        if (drain.load() > 0) {
            drain.fetch_sub(1);
            int guard = 0;
            while (guard++ < 16) { auto rr = inS->read(inBuf.data(), numFrames, 0); if (!rr || rr.value() <= 0) break; }
            std::fill(o, o + (size_t)numFrames * outCh, 0.0f);
            return oboe::DataCallbackResult::Continue;
        }
        // Giriş fazla birikirse fazlasını at
        auto av = inS->getAvailableFrames();
        if (av && av.value() > numFrames * 3) {
            int extra = av.value() - numFrames;
            while (extra > 0) { int k = std::min(extra, numFrames); auto rr = inS->read(inBuf.data(), k, 0); if (!rr || rr.value() <= 0) break; extra -= rr.value(); }
        }
        auto rr = inS->read(inBuf.data(), numFrames, 0);
        int got = rr ? rr.value() : 0;
        if (got < numFrames) underruns.fetch_add(1);
        for (int i = 0; i < numFrames; i++) monoIn[i] = i < got ? inBuf[(size_t)i * inCh] : 0.0f;
        dsp.process(monoIn.data(), monoOut.data(), numFrames);
        for (int i = 0; i < numFrames; i++) for (int c = 0; c < outCh; c++) o[(size_t)i * outCh + c] = monoOut[i];
        return oboe::DataCallbackResult::Continue;
    }

    // Kulaklık çıkarılırsa ya da cihaz değişirse durdur. Ses hoparlöre kaymasın (geri besleme ve
    // yüksek ses riski); kulaklık takılıysa uygulama yeniden başlatır.
    void onErrorAfterClose(oboe::AudioStream *, oboe::Result error) override {
        LOGE("akış kapandı: %s", oboe::convertToText(error));
        if (running.exchange(false)) std::thread([this]() { stop(); }).detach();
    }

    double latencyMs() {
        std::lock_guard<std::mutex> g(lock);
        double l = 0;
        if (out) { auto r = out->calculateLatencyMillis(); if (r) l += r.value(); }
        if (in) { auto r = in->calculateLatencyMillis(); if (r) l += r.value(); }
        return l;
    }
};

static Engine *E = nullptr;
static Engine &eng() { if (!E) E = new Engine(); return *E; }

extern "C" {

JNIEXPORT jboolean JNICALL Java_com_irig_amp_AmpEngine_nStart(JNIEnv *, jclass, jint inDev, jint outDev) {
    return eng().start(inDev, outDev) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_irig_amp_AmpEngine_nStop(JNIEnv *, jclass) { eng().stop(); }

JNIEXPORT void JNICALL Java_com_irig_amp_AmpEngine_nSet(JNIEnv *, jclass, jint id, jfloat v) { eng().dsp.set(id, v); }

// [çalışıyor, örnekleme, gecikme ms, giriş tepe, çıkış tepe, takılma, burst, giriş ön ayarı]
JNIEXPORT jfloatArray JNICALL Java_com_irig_amp_AmpEngine_nStatus(JNIEnv *env, jclass) {
    Engine &e = eng();
    float st[8];
    st[0] = e.running.load() ? 1.0f : 0.0f;
    st[1] = e.dsp.fs;
    st[2] = e.running.load() ? (float)e.latencyMs() : 0.0f;
    st[3] = e.dsp.inPeak.exchange(0.0f);
    st[4] = e.dsp.outPeak.exchange(0.0f);
    st[5] = (float)e.underruns.load();
    st[6] = 0; { std::lock_guard<std::mutex> g(e.lock); if (e.out) st[6] = (float)e.out->getFramesPerBurst(); }
    st[7] = (float)e.usedPreset;
    jfloatArray a = env->NewFloatArray(8);
    env->SetFloatArrayRegion(a, 0, 8, st);
    return a;
}

JNIEXPORT jfloat JNICALL Java_com_irig_amp_AmpEngine_nPitch(JNIEnv *, jclass) {
    Engine &e = eng();
    if (!e.running.load()) return -1;
    const int N = 4096;
    std::vector<float> buf(N);
    int end = e.dsp.ringPos.load();
    for (int i = 0; i < N; i++) buf[i] = e.dsp.ring[(end - N + i + amp::Dsp::RING) & (amp::Dsp::RING - 1)];
    return amp::yinPitch(buf.data(), N, e.dsp.fs);
}

}
