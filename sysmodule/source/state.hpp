#pragma once
#include <switch.h>
#include <string>
#include "airplay_ipc.h"

// Gemeinsamer Zustand fuer das Overlay (alles thread-sicher).
namespace state {

void setSwitchIp(u32 ip);
void setClient(bool connected, u32 clientIp);
void setRemoteAvailable(bool ok);
void setTrack(const std::string& title, const std::string& artist, const std::string& album);
void clearTrack();
void setProgress(u32 start, u32 cur, u32 end);

void fill(AirplayStatus& out);

// Vom Overlay angestossen, vom Haupt-Thread ausgefuehrt.
void requestRemote(int cmd);
int takeRemote();  // -1 = nichts
void requestSenderVolume(float db);
bool takeSenderVolume(float& db);
void markConfigDirty();
bool takeConfigDirty(u64 olderThanMs);

}  // namespace state
