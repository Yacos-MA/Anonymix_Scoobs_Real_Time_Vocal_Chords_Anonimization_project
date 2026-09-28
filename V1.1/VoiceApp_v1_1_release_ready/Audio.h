// PortAudio setup, recording and playback API. Processors here are used for recorded audio and playback.

#pragma once
#include "State.h"

// Reset the capture buffer and enable recording on the existing unified stream.
bool StartPortAudioRecording();
// Disable capture while retaining the PCM16 buffer for playback and saving.
void StopPortAudioRecording();

// Open or start playback of the captured original audio.
bool StartPortAudioPlayback();
void StopPortAudioPlayback();

// Open or start slider-controlled anonymized playback.
bool StartPortAudioPlaybackAnonymized();
void StopPortAudioPlaybackAnonymized();
// Open or start the formant-only recorded playback variant.
bool StartPortAudioPlaybackAnonymizedFormant();
void StopPortAudioPlaybackAnonymizedFormant();
// Open or start the combined pitch/formant playback variant.
bool StartPortAudioPlaybackAnonymizedPitchFormant();
void StopPortAudioPlaybackAnonymizedPitchFormant();

// Select PortAudio devices and open the unified duplex stream; stream initialization is separate
// from UI setup.
bool InitPortAudio();
// Close playback and duplex streams before terminating PortAudio.
void CleanupPortAudio();

// Convert PCM16 to float, process a recorded buffer, then write PCM16 back in place.
void ApplyRealtimeFormantShift(vector<Uint8>&, float);
// Process recorded PCM16 data with the playback pitch shifter; distinct from the duplex processor.
void ApplyRealtimePitchShift(vector<Uint8>&, float);
