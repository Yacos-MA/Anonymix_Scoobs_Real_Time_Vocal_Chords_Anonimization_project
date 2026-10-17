// PortAudio setup, recording and playback API. Processors here are used for recorded audio and playback.

#pragma once
#include "State.h"

// Reset the capture buffer and enable recording on the existing unified stream.
bool StartPortAudioRecording();
// Disable capture while retaining the PCM16 buffer for playback and saving.
void StopPortAudioRecording();
// Counts the entire lifetime of a duplex callback, including cleanup before queue publication.
// Stop waits for callbacks that started before it disabled recording.
extern atomic<unsigned> g_duplexCallbacksInFlight;
// Called only by the duplex callback. It writes a fixed-capacity SPSC queue; no mutex, vector
// growth or operating-system wakeup occurs here.
void QueuePortAudioRecording(const float* samples, size_t frames) noexcept;

// V1.7 exposes one recorded-playback control and one export action. The effect selection and
// sliders can change during playback; export snapshots both when Save starts.
bool StartPortAudioPlaybackSelected();
void StopPortAudioPlaybackSelected();
bool SavePortAudioSelectedRecording();
// SDL polls completion without waiting for the export thread.
int TakeSelectedSaveResult() noexcept;
void JoinSelectedSaveWorker();

// The UI requests a WASAPI mode. The audio worker publishes the mode actually opened:
// -2 means no stream, -1 another host, 0 Shared, 1 Exclusive. A failed Exclusive request
// can fall back to Shared; the UI displays the result after asynchronous reopen.
extern atomic<int> g_requestedWasapiMode;
extern atomic<int> g_activeWasapiMode;
// Exclusive mode keeps the duplex output endpoint occupied, so recorded preview is rendered
// by the same callback rather than opening a second output stream.
void RenderRecordedPlaybackInDuplex(void* output, unsigned long frames) noexcept;

// Initialize PortAudio and open a 48 kHz duplex stream. Some Exclusive devices use stereo
// capture/output; the callback converts capture to mono and duplicates processed mono output.
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
// Queue recorded-preview device work without waiting on SDL's visual thread.
enum class RecordedPlaybackResult { None, Started, StartFailed, Stopped };
bool RequestRecordedPlaybackStart();
bool RequestRecordedPlaybackStop();
bool IsRecordedPlaybackTransitionPending();
RecordedPlaybackResult TakeRecordedPlaybackResult();

