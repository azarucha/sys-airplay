#include "state.hpp"
#include "audio.hpp"
#include "player.hpp"
#include "util.hpp"

#include <atomic>
#include <cstring>

namespace state {

static Mutex g_lock;
static u32 g_switchIp = 0;
static bool g_connected = false;
static u32 g_clientIp = 0;
static bool g_remoteOk = false;
static std::string g_title, g_artist, g_album;
static bool g_hasProgress = false;
static u32 g_progStart = 0, g_progCur = 0, g_progEnd = 0;
static u64 g_progAt = 0;

static std::atomic<int> g_remote{-1};
static std::atomic<float> g_wantedVolume{0.0f};
static std::atomic<bool> g_volumePending{false};
static std::atomic<u64> g_configDirtyAt{0};

void setSwitchIp(u32 ip) {
    Lock l(g_lock);
    g_switchIp = ip;
}

void setClient(bool connected, u32 clientIp) {
    Lock l(g_lock);
    g_connected = connected;
    g_clientIp = clientIp;
    if (!connected) {
        g_remoteOk = false;
        g_title.clear();
        g_artist.clear();
        g_album.clear();
        g_hasProgress = false;
    }
}

void setRemoteAvailable(bool ok) {
    Lock l(g_lock);
    g_remoteOk = ok;
}

void setTrack(const std::string& title, const std::string& artist, const std::string& album) {
    Lock l(g_lock);
    g_title = title;
    g_artist = artist;
    g_album = album;
}

void clearTrack() {
    Lock l(g_lock);
    g_title.clear();
    g_artist.clear();
    g_album.clear();
    g_hasProgress = false;
}

void setProgress(u32 start, u32 cur, u32 end) {
    Lock l(g_lock);
    g_hasProgress = true;
    g_progStart = start;
    g_progCur = cur;
    g_progEnd = end;
    g_progAt = nowMs();
}

void fill(AirplayStatus& out) {
    memset(&out, 0, sizeof(out));
    u64 now = nowMs();
    u64 lastPkt = player::lastPacketMs();
    bool receiving = lastPkt && now - lastPkt < 1000;

    Lock l(g_lock);
    if (!g_connected) out.state = AirplayState_Idle;
    else if (receiving || audio::isPlaying()) out.state = AirplayState_Playing;
    else if (player::active()) out.state = AirplayState_Paused;
    else out.state = AirplayState_Connected;

    if (g_remoteOk) out.flags |= AIRPLAY_FLAG_REMOTE_OK;
    out.sender_volume_db = audio::senderVolumeDb();
    out.local_volume = audio::localVolume();
    out.buffer_ms = audio::bufferedMs();
    out.lost_packets = player::lostPackets();

    if (g_hasProgress && g_progEnd != g_progStart) {
        out.flags |= AIRPLAY_FLAG_HAS_PROGRESS;
        s64 pos;
        if (lastPkt > g_progAt) {
            // Position aus dem zuletzt empfangenen Paket minus dem, was noch im Puffer liegt
            pos = (s32)(player::lastRtpTime() - g_progStart);
            pos -= (s64)out.buffer_ms * 44100 / 1000;
        } else {
            pos = (s32)(g_progCur - g_progStart);
        }
        s64 dur = (u32)(g_progEnd - g_progStart);
        if (pos < 0) pos = 0;
        if (pos > dur) pos = dur;
        out.position_ms = (u32)(pos * 1000 / 44100);
        out.duration_ms = (u32)(dur * 1000 / 44100);
    }

    copyUtf8(out.device_name, sizeof(out.device_name), config().name.data(), config().name.size());
    if (g_switchIp) {
        std::string s = ipToString(g_switchIp);
        copyUtf8(out.switch_ip, sizeof(out.switch_ip), s.data(), s.size());
    }
    if (g_connected) {
        std::string s = ipToString(g_clientIp);
        copyUtf8(out.client_ip, sizeof(out.client_ip), s.data(), s.size());
    }
    copyUtf8(out.title, sizeof(out.title), g_title.data(), g_title.size());
    copyUtf8(out.artist, sizeof(out.artist), g_artist.data(), g_artist.size());
    copyUtf8(out.album, sizeof(out.album), g_album.data(), g_album.size());
}

void requestRemote(int cmd) { g_remote = cmd; }
int takeRemote() { return g_remote.exchange(-1); }

void requestSenderVolume(float db) {
    g_wantedVolume = db;
    g_volumePending = true;
}

bool takeSenderVolume(float& db) {
    if (!g_volumePending.exchange(false)) return false;
    db = g_wantedVolume;
    return true;
}

void markConfigDirty() { g_configDirtyAt = nowMs(); }

bool takeConfigDirty(u64 olderThanMs) {
    u64 t = g_configDirtyAt;
    if (!t || nowMs() - t < olderThanMs) return false;
    g_configDirtyAt = 0;
    return true;
}

}  // namespace state
