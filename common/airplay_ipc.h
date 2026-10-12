// Gemeinsame IPC-Definitionen zwischen sys-airplay (Sysmodule) und dem Tesla-Overlay.
#pragma once
#include <stdint.h>

#define AIRPLAY_SERVICE_NAME "airplay"
#define AIRPLAY_IPC_VERSION  1
#define AIRPLAY_TITLE_ID     0x420000000000A1A7ULL

enum AirplayCmd {
    AirplayCmd_GetStatus      = 0,  // out: AirplayStatus (Puffer)
    AirplayCmd_Remote         = 1,  // in:  u32 AirplayRemote
    AirplayCmd_SetLocalVolume = 2,  // in:  float 0..1
    AirplayCmd_GetVersion     = 3,  // out: u32
    AirplayCmd_SetSenderVolume = 4, // in:  float dB (-30..0, -144 = stumm), Lautstaerke am iPhone
};

enum AirplayRemote {
    AirplayRemote_PlayPause = 0,
    AirplayRemote_Next      = 1,
    AirplayRemote_Prev      = 2,
    AirplayRemote_VolUp     = 3,
    AirplayRemote_VolDown   = 4,
};

enum AirplayState {
    AirplayState_Idle      = 0,  // niemand verbunden
    AirplayState_Connected = 1,  // Sender verbunden, noch kein Ton
    AirplayState_Playing   = 2,
    AirplayState_Paused    = 3,
};

#define AIRPLAY_FLAG_REMOTE_OK    (1u << 0)  // Fernsteuerung (DACP) gefunden
#define AIRPLAY_FLAG_HAS_PROGRESS (1u << 1)

typedef struct {
    uint32_t state;
    uint32_t flags;
    float    sender_volume_db;  // -30..0, -144 = stumm
    float    local_volume;      // 0..1, Lautstaerke auf der Switch
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t buffer_ms;
    uint32_t lost_packets;
    char     device_name[64];   // AirPlay-Name der Switch
    char     switch_ip[16];
    char     client_ip[16];
    char     title[128];
    char     artist[128];
    char     album[128];
} AirplayStatus;
