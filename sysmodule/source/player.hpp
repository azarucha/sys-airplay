#pragma once
#include <switch.h>

// Empfang der RTP-Audiopakete (AirPlay 1): entschluesseln, ALAC dekodieren,
// in die richtige Reihenfolge bringen und an audio:: weitergeben.
namespace player {

struct Params {
    bool encrypted = false;
    u8 aesKey[16] = {};
    u8 aesIv[16] = {};
    bool alac = true;          // false = L16 (unkomprimiertes PCM)
    u32 fmtp[11] = {352, 0, 16, 40, 10, 14, 2, 255, 0, 0, 44100};
    u32 clientIp = 0;          // Netzwerk-Bytereihenfolge
    u16 clientControlPort = 0;
    u16 clientTimingPort = 0;
};

bool setup(const Params& p, u16& dataPort, u16& controlPort, u16& timingPort);
void flush(bool hasSeq, u16 seq);
void teardown();
bool active();

u64 lastPacketMs();
u32 lastRtpTime();   // RTP-Zeitstempel des zuletzt eingereihten Pakets
u32 lostPackets();

}  // namespace player
