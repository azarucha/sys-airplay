#pragma once
#include <switch.h>
#include <string>

bool dacpSend(u32 ipNetOrder, u16 port, const std::string& activeRemote, int cmd);
// Lautstaerke dieses AirPlay-Lautsprechers am Sender setzen (-30..0 dB, -144 = stumm).
bool dacpSetVolume(u32 ipNetOrder, u16 port, const std::string& activeRemote, float db);
