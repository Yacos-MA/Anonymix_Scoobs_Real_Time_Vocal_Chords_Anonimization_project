// PortAudio setup, recording and playback API. Processors here are used for recorded audio and playback.

#pragma once
#include "State.h"

// Reset the capture buffer and enable recording on the existing unified stream.
bool StartPortAudioRecording();
// Disable capture while retaining the PCM16 buffer for playback and saving.
void StopPortAudioRecording();

// V1.5 presents one recorded playback/output stream and one export action. The effect
// selection and slider values may change while the take is playing.
bool StartPortAudioPlaybackSelected();
void StopPortAudioPlaybackSelected();
bool SavePortAudioSelectedRecording();
// SDL polls completion without waiting for the export thread.
int TakeSelectedSaveResult() noexcept;
void JoinSelectedSaveWorker();

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

