#pragma once
#include <switch.h>
#include "airplay_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

bool airplayIsRunning(void);
Result airplayInit(void);
void airplayExit(void);
Result airplayGetStatus(AirplayStatus* out);
Result airplayRemote(u32 cmd);
Result airplaySetLocalVolume(float v);
Result airplaySetSenderVolume(float db);

#ifdef __cplusplus
}
#endif
