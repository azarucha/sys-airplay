#include "audio.hpp"
#include "util.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace audio {

static const u32 kInRate = 44100;
static const u32 kRingFrames = kInRate * 2;          // 2 s Ringpuffer
static const u32 kMaxFillFrames = kInRate * 9 / 5;   // darueber werden alte Daten verworfen
static const u32 kOutFrames = 1024;                  // pro audout-Puffer: 1024 * 2 * 2 = 0x1000 Byte
static const int kNumBufs = 4;

// ---------------------------------------------------------------- Ringpuffer

static Mutex g_ringLock;
static s16* g_ring = nullptr;
static u32 g_rd = 0, g_wr = 0, g_count = 0;

static void ringDropLocked(u32 frames) {
    if (frames > g_count) frames = g_count;
    g_rd = (g_rd + frames) % kRingFrames;
    g_count -= frames;
}

void push(const s16* src, u32 frames) {
    Lock l(g_ringLock);
    if (!g_ring) return;
    if (frames > kRingFrames) {
        src += (frames - kRingFrames) * 2;
        frames = kRingFrames;
    }
    if (g_count + frames > kMaxFillFrames) ringDropLocked(g_count + frames - kMaxFillFrames);
    while (frames) {
        u32 n = kRingFrames - g_wr;
        if (n > frames) n = frames;
        memcpy(g_ring + g_wr * 2, src, n * 4);
        g_wr = (g_wr + n) % kRingFrames;
        g_count += n;
        src += n * 2;
        frames -= n;
    }
}

static u32 ringRead(s16* dst, u32 frames) {
    Lock l(g_ringLock);
    if (frames > g_count) frames = g_count;
    u32 done = 0;
    while (done < frames) {
        u32 n = kRingFrames - g_rd;
        if (n > frames - done) n = frames - done;
        memcpy(dst + done * 2, g_ring + g_rd * 2, n * 4);
        g_rd = (g_rd + n) % kRingFrames;
        done += n;
    }
    g_count -= frames;
    return frames;
}

static u32 ringCount() {
    Lock l(g_ringLock);
    return g_count;
}

static std::atomic<bool> g_resetResampler{false};

void clear() {
    {
        Lock l(g_ringLock);
        g_rd = g_wr = g_count = 0;
    }
    g_resetResampler = true;
}

u32 bufferedMs() { return (u32)((u64)ringCount() * 1000 / kInRate); }

// ---------------------------------------------------------------- Lautstaerke

static std::atomic<float> g_senderDb{0.0f};
static std::atomic<float> g_local{1.0f};
static std::atomic<bool> g_playing{false};
static std::atomic<bool> g_run{false};

void setSenderVolumeDb(float db) { g_senderDb = db; }
void setLocalVolume(float v) { g_local = v < 0 ? 0 : (v > 1 ? 1 : v); }
float senderVolumeDb() { return g_senderDb; }
float localVolume() { return g_local; }
bool isPlaying() { return g_playing; }

static float effectiveVolume() {
    float db = g_senderDb;
    float sender = db <= -30.0f ? 0.0f : powf(10.0f, db / 20.0f);
    return sender * g_local;
}

// ---------------------------------------------------------------- Resampler 44,1 -> 48 kHz (linear)

struct Resampler {
    float step = 1.0f;   // Eingangsframes pro Ausgangsframe
    float pos = 1.0f;    // Position zwischen prev und next
    s16 prev[2] = {}, next[2] = {};
    s16 in[512 * 2];
    u32 inLen = 0, inIdx = 0;

    void reset() {
        pos = 1.0f;
        prev[0] = prev[1] = next[0] = next[1] = 0;
        inLen = inIdx = 0;
    }

    bool nextFrame() {
        if (inIdx >= inLen) {
            inLen = ringRead(in, 512);
            inIdx = 0;
            if (!inLen) return false;
        }
        prev[0] = next[0];
        prev[1] = next[1];
        next[0] = in[inIdx * 2];
        next[1] = in[inIdx * 2 + 1];
        inIdx++;
        return true;
    }

    // Erzeugt bis zu n Ausgangsframes, gibt die tatsaechliche Anzahl zurueck.
    u32 produce(s16* out, u32 n, float vol) {
        for (u32 i = 0; i < n; i++) {
            while (pos >= 1.0f) {
                if (!nextFrame()) return i;
                pos -= 1.0f;
            }
            for (int c = 0; c < 2; c++) {
                float s = (prev[c] + (next[c] - prev[c]) * pos) * vol;
                if (s > 32767.0f) s = 32767.0f;
                if (s < -32768.0f) s = -32768.0f;
                out[i * 2 + c] = (s16)s;
            }
            pos += step;
        }
        return n;
    }
};

// ---------------------------------------------------------------- audout

alignas(0x1000) static s16 g_outMem[kNumBufs][kOutFrames * 2];
static AudioOutBuffer g_outBuf[kNumBufs];
static Resampler g_rs;
static Thread g_thread;
alignas(0x1000) static u8 g_threadStack[0x4000];
static bool g_audoutUp = false;

static void audioThread(void*) {
    bool playing = false;

    while (g_run) {
        if (g_resetResampler.exchange(false)) {
            g_rs.reset();
            playing = false;
        }
        if (!playing && ringCount() >= kInRate * config().bufferMs / 1000) playing = true;

        bool queuedAny = false;
        float vol = effectiveVolume();
        for (int i = 0; i < kNumBufs; i++) {
            bool queued = false;
            if (R_FAILED(audoutContainsAudioOutBuffer(&g_outBuf[i], &queued))) queued = false;
            if (queued) {
                queuedAny = true;
                continue;
            }
            if (!playing) continue;
            u32 got = g_rs.produce(g_outMem[i], kOutFrames, vol);
            if (got == 0) {
                playing = false;  // Puffer leer: neu vorpuffern
                break;
            }
            if (got < kOutFrames) memset(g_outMem[i] + got * 2, 0, (kOutFrames - got) * 4);
            g_outBuf[i].data_size = kOutFrames * 4;
            if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&g_outBuf[i]))) queuedAny = true;
        }
        g_playing = playing;

        if (queuedAny) {
            AudioOutBuffer* released = nullptr;
            u32 count = 0;
            audoutWaitPlayFinish(&released, &count, 50'000'000ULL);
        } else {
            svcSleepThread(10'000'000ULL);
        }
    }
}

bool init() {
    g_local = config().localVolume;

    g_ring = (s16*)malloc(kRingFrames * 4);
    if (!g_ring) {
        logMsg("Kein Speicher fuer den Ringpuffer");
        return false;
    }

    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        logMsg("audoutInitialize fehlgeschlagen: 0x%x", rc);
        return false;
    }
    g_audoutUp = true;
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        logMsg("audoutStartAudioOut fehlgeschlagen: 0x%x", rc);
        return false;
    }

    u32 outRate = audoutGetSampleRate();
    if (!outRate) outRate = 48000;
    g_rs.step = (float)kInRate / (float)outRate;
    g_rs.reset();
    logMsg("audout: %u Hz, %u Kanaele", outRate, audoutGetChannelCount());

    for (int i = 0; i < kNumBufs; i++) {
        memset(&g_outBuf[i], 0, sizeof(g_outBuf[i]));
        g_outBuf[i].buffer = g_outMem[i];
        g_outBuf[i].buffer_size = sizeof(g_outMem[i]);
        g_outBuf[i].data_size = sizeof(g_outMem[i]);
        g_outBuf[i].data_offset = 0;
    }

    g_run = true;
    rc = threadCreate(&g_thread, audioThread, nullptr, g_threadStack, sizeof(g_threadStack), 0x1C, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
    if (R_FAILED(rc)) {
        logMsg("Audio-Thread konnte nicht starten: 0x%x", rc);
        g_run = false;
        return false;
    }
    logMsg("Audio bereit");
    return true;
}

void exit() {
    if (g_run) {
        g_run = false;
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
    }
    if (g_audoutUp) {
        audoutStopAudioOut();
        audoutExit();
        g_audoutUp = false;
    }
}

}  // namespace audio
