#include "player.hpp"
#include "audio.hpp"
#include "util.hpp"

#include "alac/ALACBitUtilities.h"
#include "alac/ALACDecoder.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace player {

static const int kSlots = 64;         // Umordnungsfenster (~0,5 s bei 352 Frames/Paket)
static const int kWaitPackets = 25;   // so lange auf ein fehlendes Paket warten (~200 ms)
static const u32 kMaxFrameLen = 1024;

struct Slot {
    bool used;
    u16 seq;
    u32 ts;
    u32 frames;
    s16* pcm;
};

static Params g_p;
static u32 g_frameLen = 352;
static u32 g_channels = 2;
static int g_data = -1, g_ctrl = -1, g_timing = -1;
static Thread g_thread;
static bool g_threadUp = false;
static std::atomic<bool> g_run{false};

static ALACDecoder* g_dec = nullptr;
static Aes128CbcContext g_aes;
static Slot g_slots[kSlots];
static s16* g_pcmPool = nullptr;
static s16* g_silence = nullptr;
static s16* g_monoTmp = nullptr;

// Nur vom Empfangs-Thread benutzt
static bool g_haveExpected = false;
static u16 g_expected = 0;
static u16 g_highest = 0;
static bool g_dropOld = false;
static u16 g_flushFrom = 0;
static u16 g_resendReqSeq = 0;

static std::atomic<bool> g_flushReq{false};
static std::atomic<int> g_flushSeq{-1};
static std::atomic<u64> g_lastPacket{0};
static std::atomic<u32> g_lastRtp{0};
static std::atomic<u32> g_lost{0};

u64 lastPacketMs() { return g_lastPacket; }
u32 lastRtpTime() { return g_lastRtp; }
u32 lostPackets() { return g_lost; }
bool active() { return g_threadUp; }

// ---------------------------------------------------------------- Netzwerk

static int openUdp(u16 startPort, u16& portOut) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    for (u16 port = startPort; port < startPort + 200; port++) {
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (bind(fd, (sockaddr*)&a, sizeof(a)) == 0) {
            portOut = port;
            return fd;
        }
    }
    close(fd);
    return -1;
}

static void sendToClient(int fd, u16 port, const void* data, size_t len) {
    if (fd < 0 || port == 0) return;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = g_p.clientIp;
    a.sin_port = htons(port);
    sendto(fd, data, len, 0, (sockaddr*)&a, sizeof(a));
}

static void ntpNow(u8* out) {
    u64 ns = armTicksToNs(armGetSystemTick());
    u32 sec = (u32)(ns / 1000000000ULL) + 0x83AA7E80u;  // Offset 1900 -> 1970, Wert ist nur relativ
    u32 frac = (u32)(((ns % 1000000000ULL) << 32) / 1000000000ULL);
    wr32(out, sec);
    wr32(out + 4, frac);
}

static void sendTimingRequest() {
    u8 pkt[32] = {0x80, 0xD2, 0x00, 0x07};
    ntpNow(pkt + 24);
    sendToClient(g_timing, g_p.clientTimingPort, pkt, sizeof(pkt));
}

static void answerTimingRequest(const u8* req, int len) {
    if (len < 32) return;
    u8 pkt[32] = {0x80, 0xD3, 0x00, 0x07};
    memcpy(pkt + 8, req + 24, 8);  // Sendezeit der Anfrage als Referenz zurueck
    ntpNow(pkt + 16);
    ntpNow(pkt + 24);
    sendToClient(g_timing, g_p.clientTimingPort, pkt, sizeof(pkt));
}

static void requestResend(u16 first, u16 count) {
    u8 pkt[8] = {0x80, 0xD5};
    wr16(pkt + 2, g_resendReqSeq++);
    wr16(pkt + 4, first);
    wr16(pkt + 6, count);
    sendToClient(g_ctrl, g_p.clientControlPort, pkt, sizeof(pkt));
}

// ---------------------------------------------------------------- Dekodieren

static bool decode(const u8* payload, int plen, s16* out, u32& frames) {
    static u8 tmp[8192];
    frames = 0;
    if (plen <= 0 || plen > (int)sizeof(tmp)) return false;

    if (g_p.encrypted) {
        int enc = plen & ~15;  // nur volle 16-Byte-Bloecke sind verschluesselt
        if (enc) {
            aes128CbcContextResetIv(&g_aes, g_p.aesIv);
            aes128CbcDecrypt(&g_aes, tmp, payload, enc);
        }
        memcpy(tmp + enc, payload + enc, plen - enc);
    } else {
        memcpy(tmp, payload, plen);
    }

    if (g_p.alac) {
        BitBuffer bits;
        BitBufferInit(&bits, tmp, (uint32_t)plen);
        uint32_t outFrames = 0;
        s16* dst = g_channels == 1 ? g_monoTmp : out;
        int32_t st = g_dec->Decode(&bits, (uint8_t*)dst, g_frameLen, g_channels, &outFrames);
        if (st != 0 || outFrames > g_frameLen) return false;
        if (g_channels == 1)
            for (u32 i = 0; i < outFrames; i++) out[i * 2] = out[i * 2 + 1] = g_monoTmp[i];
        frames = outFrames;
    } else {
        // L16: Big-Endian-PCM, Stereo
        u32 n = (u32)plen / 4;
        if (n > g_frameLen) n = g_frameLen;
        for (u32 i = 0; i < n * 2; i++) out[i] = (s16)rd16(tmp + i * 2);
        frames = n;
    }
    return true;
}

// ---------------------------------------------------------------- Reihenfolge

static void resetSlots() {
    for (auto& s : g_slots) s.used = false;
}

static void deliver() {
    while (true) {
        Slot& s = g_slots[g_expected % kSlots];
        if (s.used && s.seq == g_expected) {
            if (s.frames) audio::push(s.pcm, s.frames);
            g_lastRtp = s.ts;
            s.used = false;
            g_expected++;
            continue;
        }
        s16 behind = (s16)(g_highest - g_expected);
        if (behind > kWaitPackets) {
            // Paket kam nicht rechtzeitig: mit Stille ueberbruecken
            audio::push(g_silence, g_frameLen);
            g_lost++;
            g_expected++;
            continue;
        }
        break;
    }
}

static void handleAudio(const u8* pkt, int len) {
    if (len < 12) return;
    u16 seq = rd16(pkt + 2);
    u32 ts = rd32(pkt + 4);

    if (g_dropOld) {
        if ((s16)(seq - g_flushFrom) < 0) return;  // Pakete von vor dem FLUSH
        g_dropOld = false;
    }
    g_lastPacket = nowMs();

    if (!g_haveExpected) {
        g_haveExpected = true;
        g_expected = seq;
        g_highest = seq - 1;
    }
    s16 d = (s16)(seq - g_expected);
    if (d < 0) return;  // zu spaet oder doppelt
    if (d >= kSlots) {  // grosser Sprung: neu synchronisieren
        resetSlots();
        g_expected = seq;
        g_highest = seq - 1;
    }

    Slot& s = g_slots[seq % kSlots];
    if (s.used && s.seq == seq) return;
    u32 frames = 0;
    if (!decode(pkt + 12, len - 12, s.pcm, frames)) frames = 0;
    s.used = true;
    s.seq = seq;
    s.ts = ts;
    s.frames = frames;

    s16 ahead = (s16)(seq - g_highest);
    if (ahead > 1) requestResend(g_highest + 1, ahead - 1);
    if (ahead > 0) g_highest = seq;

    deliver();
}

// ---------------------------------------------------------------- Thread

static void rxThread(void*) {
    static u8 buf[2048];
    u64 lastTiming = 0;

    while (g_run) {
        if (g_flushReq.exchange(false)) {
            resetSlots();
            g_haveExpected = false;
            int fs = g_flushSeq;
            if (fs >= 0) {
                g_dropOld = true;
                g_flushFrom = (u16)fs;
            }
            audio::clear();
        }

        u64 now = nowMs();
        if (now - lastTiming > 3000) {
            sendTimingRequest();
            lastTiming = now;
        }

        pollfd fds[3] = {{g_data, POLLIN, 0}, {g_ctrl, POLLIN, 0}, {g_timing, POLLIN, 0}};
        int r = poll(fds, 3, 50);
        if (r <= 0) continue;

        if (fds[0].revents & POLLIN) {
            int n = recv(g_data, buf, sizeof(buf), 0);
            if (n >= 12 && (buf[1] & 0x7F) == 0x60) handleAudio(buf, n);
        }
        if (fds[1].revents & POLLIN) {
            int n = recv(g_ctrl, buf, sizeof(buf), 0);
            // 0x56 = nachgeliefertes Paket, 0x54 = Sync (wird nicht gebraucht)
            if (n >= 16 && (buf[1] & 0x7F) == 0x56) handleAudio(buf + 4, n - 4);
        }
        if (fds[2].revents & POLLIN) {
            int n = recv(g_timing, buf, sizeof(buf), 0);
            if (n >= 32 && (buf[1] & 0x7F) == 0x52) answerTimingRequest(buf, n);
        }
    }
}

// ---------------------------------------------------------------- Steuerung

static void freeAll() {
    if (g_data >= 0) close(g_data);
    if (g_ctrl >= 0) close(g_ctrl);
    if (g_timing >= 0) close(g_timing);
    g_data = g_ctrl = g_timing = -1;
    delete g_dec;
    g_dec = nullptr;
    free(g_pcmPool);
    free(g_silence);
    free(g_monoTmp);
    g_pcmPool = g_silence = g_monoTmp = nullptr;
}

bool setup(const Params& p, u16& dataPort, u16& controlPort, u16& timingPort) {
    teardown();
    g_p = p;
    g_frameLen = p.alac ? p.fmtp[0] : 352;
    if (g_frameLen == 0 || g_frameLen > kMaxFrameLen) g_frameLen = 352;
    g_channels = (p.alac && p.fmtp[6] == 1) ? 1 : 2;

    if (p.alac) {
        // ALACSpecificConfig (24 Byte, Big Endian) aus den fmtp-Werten
        u8 cookie[24];
        wr32(cookie + 0, g_frameLen);
        cookie[4] = (u8)p.fmtp[1];
        cookie[5] = (u8)p.fmtp[2];
        cookie[6] = (u8)p.fmtp[3];
        cookie[7] = (u8)p.fmtp[4];
        cookie[8] = (u8)p.fmtp[5];
        cookie[9] = (u8)g_channels;
        wr16(cookie + 10, (u16)p.fmtp[7]);
        wr32(cookie + 12, p.fmtp[8]);
        wr32(cookie + 16, p.fmtp[9]);
        wr32(cookie + 20, p.fmtp[10]);
        g_dec = new ALACDecoder();
        if (g_dec->Init(cookie, sizeof(cookie)) != 0) {
            logMsg("ALAC-Init fehlgeschlagen");
            freeAll();
            return false;
        }
        if (p.fmtp[2] != 16) logMsg("Warnung: Bittiefe %u wird nicht unterstuetzt", p.fmtp[2]);
    }
    if (p.encrypted) aes128CbcContextCreate(&g_aes, p.aesKey, p.aesIv, false);

    g_pcmPool = (s16*)calloc(kSlots * g_frameLen * 2, sizeof(s16));
    g_silence = (s16*)calloc(g_frameLen * 2, sizeof(s16));
    g_monoTmp = (s16*)calloc(g_frameLen, sizeof(s16));
    if (!g_pcmPool || !g_silence || !g_monoTmp) {
        logMsg("Kein Speicher fuer Audiopuffer");
        freeAll();
        return false;
    }
    for (int i = 0; i < kSlots; i++) {
        g_slots[i].used = false;
        g_slots[i].pcm = g_pcmPool + i * g_frameLen * 2;
    }

    g_data = openUdp(6000, dataPort);
    g_ctrl = openUdp(dataPort + 1, controlPort);
    g_timing = openUdp(controlPort + 1, timingPort);
    if (g_data < 0 || g_ctrl < 0 || g_timing < 0) {
        logMsg("UDP-Ports konnten nicht geoeffnet werden");
        freeAll();
        return false;
    }

    g_haveExpected = false;
    g_dropOld = false;
    g_flushReq = false;
    g_lost = 0;
    g_lastPacket = 0;
    audio::clear();

    g_run = true;
    Result rc = threadCreate(&g_thread, rxThread, nullptr, nullptr, 0x8000, 0x1E, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
    if (R_FAILED(rc)) {
        logMsg("RTP-Thread konnte nicht starten: 0x%x", rc);
        g_run = false;
        freeAll();
        return false;
    }
    g_threadUp = true;
    logMsg("RTP bereit: data=%u control=%u timing=%u (%s, %s)", dataPort, controlPort, timingPort,
           p.alac ? "ALAC" : "L16", p.encrypted ? "verschluesselt" : "unverschluesselt");
    return true;
}

void flush(bool hasSeq, u16 seq) {
    g_flushSeq = hasSeq ? (int)seq : -1;
    g_flushReq = true;
    audio::clear();
}

void teardown() {
    if (g_threadUp) {
        g_run = false;
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_threadUp = false;
        logMsg("RTP beendet");
    }
    freeAll();
    audio::clear();
}

}  // namespace player
