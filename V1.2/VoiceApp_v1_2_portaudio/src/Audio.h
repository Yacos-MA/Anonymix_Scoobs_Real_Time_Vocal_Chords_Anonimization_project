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
// The timbre-only recording preview follows the same slider as live stream processing.
bool StartPortAudioPlaybackTimbre();
void StopPortAudioPlaybackTimbre();
// Apply the timbre filter to a PCM16 copy for a saved recording.
void ApplyRecordedTimbre(vector<Uint8>& pcm16, float sliderPercent);
// Export a snapshot of the captured take with the selected timbre and output gain.
bool SavePortAudioTimbreRecording();

// Initialize PortAudio and open its live mono input/output stream at 48 kHz.
bool InitPortAudio();
// Close playback and duplex streams before terminating PortAudio.
void CleanupPortAudio();
// Request the current default input/output devices after a device change. PortAudio builds
// return whether the request was queued; SDL publishes its eventual result on a later frame.
bool ReopenAudioDevices();
// True while the PortAudio device transition runs on its dedicated worker thread.
bool IsAudioDeviceReopening();
// Invoke device operations on the thread that initialized WASAPI.
bool IsPortAudioDeviceWorkerThread();
bool RunPortAudioDeviceAction(bool (*action)());
void RunPortAudioDeviceAction(void (*action)());

// Convert PCM16 to float, process a recorded buffer, then write PCM16 back in place.
void ApplyRealtimeFormantShift(vector<Uint8>&, float);
// Process recorded PCM16 data with the playback pitch shifter; distinct from the duplex processor.
void ApplyRealtimePitchShift(vector<Uint8>&, float);
