#pragma once
#include <switch.h>

// Tonausgabe ueber audout (wie sys-tune). Ein eigener Thread liest PCM (44,1 kHz, Stereo, s16)
// aus einem Ringpuffer und gibt ihn neben dem laufenden Spiel aus.
namespace audio {

bool init();
void exit();

// Vom Empfangs-Thread aufgerufen.
void push(const s16* frames, u32 count);
void clear();

void setSenderVolumeDb(float db);
void setLocalVolume(float v);
float senderVolumeDb();
float localVolume();

u32 bufferedMs();
bool isPlaying();

}  // namespace audio
