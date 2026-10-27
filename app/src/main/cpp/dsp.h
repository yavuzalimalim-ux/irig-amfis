// Gitar amfi DSP zinciri (platformdan bağımsız, tek kanal, float)
// Giriş kazancı → DC filtresi → noise gate → distorsiyon → ton (bas/orta/tiz) → kabin → delay → reverb → master
#pragma once
#include <atomic>
#include <cmath>
#include <vector>
#include <algorithm>
#include <cstring>

namespace amp {

enum Param {
    P_INGAIN = 0,   // dB  -12..+24
    P_GATE_ON,      // 0/1
    P_GATE_THR,     // dB  -80..-30
    P_TYPE,         // 0 temiz, 1 crunch, 2 lead, 3 fuzz
    P_DRIVE,        // 0..1
    P_BASS,         // dB -12..12
    P_MID,          // dB -12..12
    P_TREBLE,       // dB -12..12
    P_CAB,          // 0/1
    P_DLY_ON,       // 0/1
    P_DLY_MS,       // 40..900
    P_DLY_FB,       // 0..0.9
    P_DLY_MIX,      // 0..1
    P_REV_ON,       // 0/1
    P_REV_SIZE,     // 0..1
    P_REV_MIX,      // 0..1
    P_MASTER,       // dB -40..+6
    P_BYPASS,       // 0/1 (yalnız giriş kazancı, efektsiz)
    P_MUTE,         // 0/1 (akort ederken sessiz)
    P_COUNT
};

static inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

// RBJ biquad
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
    void set(float nb0, float nb1, float nb2, float na0, float na1, float na2) {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
    }
    void lowpass(float fs, float f, float q) {
        float w = 2 * (float)M_PI * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void highpass(float fs, float f, float q) {
        float w = 2 * (float)M_PI * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void peak(float fs, float f, float q, float db) {
        float A = std::pow(10.0f, db / 40.0f), w = 2 * (float)M_PI * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void lowshelf(float fs, float f, float db) {
        float A = std::pow(10.0f, db / 40.0f), w = 2 * (float)M_PI * f / fs, c = std::cos(w), s = std::sin(w);
        float al = s / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
            (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
    }
    void highshelf(float fs, float f, float db) {
        float A = std::pow(10.0f, db / 40.0f), w = 2 * (float)M_PI * f / fs, c = std::cos(w), s = std::sin(w);
        float al = s / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
            (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
    }
};

struct Comb {
    std::vector<float> buf; size_t i = 0; float store = 0;
    inline float process(float x, float fb, float damp) {
        float y = buf[i];
        store = y * (1 - damp) + store * damp;
        buf[i] = x + store * fb;
        if (++i >= buf.size()) i = 0;
        return y;
    }
};
struct Allpass {
    std::vector<float> buf; size_t i = 0;
    inline float process(float x) {
        float b = buf[i], y = -x + b;
        buf[i] = x + b * 0.5f;
        if (++i >= buf.size()) i = 0;
        return y;
    }
};

class Dsp {
public:
    std::atomic<float> p[P_COUNT];
    std::atomic<int> version{0};

    Dsp() {
        const float def[P_COUNT] = {0, 1, -62, 1, 0.45f, 2, 0, 1, 1, 0, 380, 0.3f, 0.25f, 1, 0.5f, 0.18f, -6, 0, 0};
        for (int i = 0; i < P_COUNT; i++) p[i].store(def[i]);
    }

    void set(int id, float v) {
        if (id < 0 || id >= P_COUNT || !std::isfinite(v)) return;
        p[id].store(v); version.fetch_add(1);
    }

    void prepare(float sampleRate) {
        fs = sampleRate > 8000 ? sampleRate : 48000;
        dc.highpass(fs, 25, 0.707f);
        dly.assign((size_t)(fs * 1.0f) + 8, 0.0f); dw = 0;
        dlyLp.lowpass(fs, 3200, 0.7f);
        // Freeverb ayarları (44.1 kHz için), örnekleme hızına göre ölçeklenir
        const int ct[4] = {1116, 1188, 1277, 1356}, at[2] = {556, 441};
        float k = fs / 44100.0f;
        for (int i = 0; i < 4; i++) { combs[i].buf.assign((size_t)(ct[i] * k), 0.0f); combs[i].i = 0; combs[i].store = 0; }
        for (int i = 0; i < 2; i++) { aps[i].buf.assign((size_t)(at[i] * k), 0.0f); aps[i].i = 0; }
        env = 0; gateGain = 1;
        seen = -1;
        update();
    }

    // Bir blok işler (yerinde). in ve out aynı olabilir.
    void process(const float *in, float *out, int n) {
        int v = version.load();
        if (v != seen) { seen = v; update(); }
        for (int i = 0; i < n; i++) out[i] = sample(in[i]);
    }

    // Akort ve ölçüm için son giriş örnekleri
    static const int RING = 8192;
    float ring[RING] = {0};
    std::atomic<int> ringPos{0};
    std::atomic<float> inPeak{0}, outPeak{0};
    float fs = 48000;

private:
    Biquad dc, preHp, preLp, postLp, bass, mid, treb, cab[5], dlyLp;
    std::vector<float> dly; size_t dw = 0;
    Comb combs[4]; Allpass aps[2];
    float env = 0, gateGain = 1, inGain = 1, gateThr = 0, master = 0.5f;
    float pre = 1, makeup = 1, drive = 0, dlyFb = 0, dlyMix = 0, revMix = 0, revFb = 0.84f, revDamp = 0.3f;
    int type = 1, seen = -1, dlySamples = 1000; bool gateOn = true, cabOn = true, dlyOn = false, revOn = false, bypass = false, mute = false;
    float atk = 0, rel = 0, gOpen = 0, gClose = 0;

    void update() {
        inGain = dbToLin(p[P_INGAIN].load());
        gateOn = p[P_GATE_ON].load() > 0.5f; gateThr = dbToLin(p[P_GATE_THR].load());
        type = (int)std::lround(p[P_TYPE].load()); type = std::max(0, std::min(3, type));
        drive = std::max(0.0f, std::min(1.0f, p[P_DRIVE].load()));
        // Distorsiyon türüne göre ön kazanç ve seviye dengeleme
        switch (type) {
            case 0: pre = 1.0f + drive * 3.0f; makeup = 0.9f / (0.6f + drive * 0.9f); break;        // temiz
            case 1: pre = dbToLin(6 + drive * 30); makeup = 0.55f; break;                          // crunch
            case 2: pre = dbToLin(14 + drive * 34); makeup = 0.45f; break;                         // lead
            default: pre = dbToLin(20 + drive * 30); makeup = 0.35f; break;                        // fuzz
        }
        preHp.highpass(fs, type >= 2 ? 520.0f : (type == 1 ? 180.0f : 60.0f), 0.6f);  // gevşek bası kes, sıkı ses
        preLp.lowpass(fs, 7000, 0.7f);
        postLp.lowpass(fs, type == 0 ? 9000.0f : 6500.0f, 0.7f);
        bass.lowshelf(fs, 120, std::max(-12.0f, std::min(12.0f, p[P_BASS].load())) + (type >= 2 ? 4.0f : 0.0f));
        mid.peak(fs, 750, 0.8f, std::max(-12.0f, std::min(12.0f, p[P_MID].load())));
        treb.highshelf(fs, 2800, std::max(-12.0f, std::min(12.0f, p[P_TREBLE].load())));
        cabOn = p[P_CAB].load() > 0.5f;
        cab[0].highpass(fs, 75, 0.7f);
        cab[1].peak(fs, 115, 1.0f, 4.0f);
        cab[2].peak(fs, 2600, 2.0f, 3.0f);
        cab[3].lowpass(fs, 4800, 0.9f);
        cab[4].lowpass(fs, 5600, 0.6f);
        dlyOn = p[P_DLY_ON].load() > 0.5f;
        dlySamples = (int)(std::max(40.0f, std::min(900.0f, p[P_DLY_MS].load())) * fs / 1000.0f);
        dlySamples = std::min(dlySamples, (int)dly.size() - 2);
        dlyFb = std::max(0.0f, std::min(0.9f, p[P_DLY_FB].load()));
        dlyMix = std::max(0.0f, std::min(1.0f, p[P_DLY_MIX].load()));
        revOn = p[P_REV_ON].load() > 0.5f;
        float sz = std::max(0.0f, std::min(1.0f, p[P_REV_SIZE].load()));
        revFb = 0.70f + sz * 0.26f; revDamp = 0.45f - sz * 0.2f;
        revMix = std::max(0.0f, std::min(1.0f, p[P_REV_MIX].load()));
        master = dbToLin(std::max(-60.0f, std::min(6.0f, p[P_MASTER].load())));
        bypass = p[P_BYPASS].load() > 0.5f;
        mute = p[P_MUTE].load() > 0.5f;
        atk = 1 - std::exp(-1.0f / (0.001f * fs));
        rel = 1 - std::exp(-1.0f / (0.120f * fs));
        gOpen = 1 - std::exp(-1.0f / (0.002f * fs));
        gClose = 1 - std::exp(-1.0f / (0.060f * fs));
    }

    static inline float clipSoft(float x) { return std::tanh(x); }

    inline float sample(float x) {
        if (!std::isfinite(x)) x = 0;
        x = dc.process(x);
        // akort için ham giriş
        int rp = ringPos.load(std::memory_order_relaxed);
        ring[rp] = x; ringPos.store((rp + 1) & (RING - 1), std::memory_order_relaxed);
        x *= inGain;
        float ax = std::fabs(x);
        if (ax > inPeak.load(std::memory_order_relaxed)) inPeak.store(ax, std::memory_order_relaxed);
        if (bypass) return finish(x);

        // Noise gate
        env += (ax > env ? atk : rel) * (ax - env);
        float target = (!gateOn || env > gateThr) ? 1.0f : 0.0f;
        gateGain += (target > gateGain ? gOpen : gClose) * (target - gateGain);
        x *= gateGain;

        // Distorsiyon
        float y = preHp.process(x) * pre;
        switch (type) {
            case 0: y = clipSoft(y * 0.8f) / 0.8f; break;
            case 1: y = y > 0 ? clipSoft(y) : clipSoft(y * 0.75f) / 0.75f; break;      // asimetrik tüp benzeri
            case 2: y = clipSoft(y); y = clipSoft(1.6f * y) * 0.85f; break;            // iki kademe
            default: y = std::max(-0.7f, std::min(0.7f, y)) / 0.7f; break;             // sert kırpma
        }
        y = postLp.process(preLp.process(y)) * makeup;
        if (type == 0) y = y + 0.0f;
        // Ton katı
        y = treb.process(mid.process(bass.process(y)));
        // Kabin
        if (cabOn) for (auto &b : cab) y = b.process(y);
        // Delay
        if (dlyOn) {
            size_t rd = (dw + dly.size() - (size_t)dlySamples) % dly.size();
            float d = dly[rd];
            dly[dw] = y + dlyLp.process(d) * dlyFb;
            if (++dw >= dly.size()) dw = 0;
            y = y + d * dlyMix;
        }
        // Reverb
        if (revOn && revMix > 0.001f) {
            float in = y * 0.12f, s = 0;
            for (auto &c : combs) s += c.process(in, revFb, revDamp);
            for (auto &a : aps) s = a.process(s);
            y = y * (1 - revMix * 0.5f) + s * revMix * 2.2f;
        }
        return finish(y);
    }

    inline float finish(float y) {
        y *= master;
        if (mute) y = 0;
        // Kulak koruma: yumuşak sınırlayıcı
        if (y > 0.95f || y < -0.95f) y = 0.95f * std::tanh(y / 0.95f);
        if (!std::isfinite(y)) y = 0;
        float ay = std::fabs(y);
        if (ay > outPeak.load(std::memory_order_relaxed)) outPeak.store(ay, std::memory_order_relaxed);
        return y;
    }
};

// YIN perde bulma: sonuç Hz, bulunamazsa -1
static inline float yinPitch(const float *x, int n, float fs) {
    int W = n / 2; if (W < 64) return -1;
    double rms = 0; for (int i = 0; i < n; i++) rms += (double)x[i] * x[i];
    if (std::sqrt(rms / n) < 0.004) return -1;
    int maxTau = std::min(W, (int)(fs / 60.0f)), minTau = (int)(fs / 1400.0f);
    std::vector<float> d(maxTau + 1, 0.0f);
    for (int tau = 1; tau <= maxTau; tau++) {
        double s = 0;
        for (int i = 0; i < W; i++) { float t = x[i] - x[i + tau]; s += t * t; }
        d[tau] = (float)s;
    }
    // birikimli ortalama ile normalize
    double run = 0; d[0] = 1;
    for (int tau = 1; tau <= maxTau; tau++) { run += d[tau]; d[tau] = run > 0 ? (float)(d[tau] * tau / run) : 1.0f; }
    int best = -1;
    for (int tau = std::max(2, minTau); tau < maxTau; tau++) {
        if (d[tau] < 0.15f) { while (tau + 1 < maxTau && d[tau + 1] < d[tau]) tau++; best = tau; break; }
    }
    if (best < 0) return -1;
    // parabolik ara değer
    float a = d[best - 1], b = d[best], c = d[best + 1 <= maxTau ? best + 1 : best];
    float den = a + c - 2 * b, shift = std::fabs(den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f;
    return fs / (best + shift);
}

} // namespace amp
