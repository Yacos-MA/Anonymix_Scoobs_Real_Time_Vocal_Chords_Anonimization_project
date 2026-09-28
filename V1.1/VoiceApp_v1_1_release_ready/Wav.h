// RIFF/WAVE PCM16 output declarations. Inputs are raw PCM bytes, without a WAV header.

#pragma once
#include "Common.h"

// Create recordings/ beside the executable and write a PCM16 WAV file. The caller must supply
// data matching numChannels.
bool SaveWAV_Adaptive(const string&, const vector<Uint8>&, int, float sampleRate = PA_SAMPLE_RATE);

// Append local date and time to a prefix; two saves with the same prefix within one second can
// collide.
string GenerateWAVFilename(const string&);
