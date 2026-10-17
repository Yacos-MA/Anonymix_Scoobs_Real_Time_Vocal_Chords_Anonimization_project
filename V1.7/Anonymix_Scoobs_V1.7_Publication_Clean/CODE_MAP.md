# Anonymix Scoobs V1.7 — code map

This guide applies to both V1.7 source variants. The development variant retains
diagnostics; the publication variant removes application log output and timing
probes. They are separate deliverables. Start in `App.cpp`, then follow the
audio path through `Stream.cpp` and `Audio.cpp`.

## Execution and ownership

| Owner | Files | Responsibilities |
| --- | --- | --- |
| SDL/UI thread | `App.cpp`, `AppUpdate.cpp`, `AppRender.cpp`, `Visualization.cpp`, `Widgets.h` | Create the window and controls, receive events, publish effect settings, take short display-ring snapshots, calculate the display FFT, and draw frames toward a 60 Hz target. |
| PortAudio device worker | `App.cpp`, `Audio.cpp` | Initialize/terminate PortAudio and Windows COM on one thread; open, close and reopen devices; start/stop recorded preview. The UI observes asynchronous results on later frames. |
| PortAudio callback | `Stream.cpp`, selected-preview callback in `Audio.cpp` | Capture, clean and process small audio blocks. Keep processing state across calls. Never allocate a growing recording vector, write a WAV or wait for the UI. |
| Recording collector | `Audio.cpp`, `RecordingQueue.h` | Drain a fixed-capacity single-producer/single-consumer queue into a mutex-protected PCM16 vector. One collector is created for each take and joined on Stop. |
| WAV export worker | `Audio.cpp`, `Wav.cpp` | Process a copied take with a private effect state, then write a mono 48 kHz PCM16 WAV. |

`Common.h` defines shared constants and effect bits. `State.h` declares
cross-module UI/audio state; `State.cpp` defines it. A widget's name is a
lookup key shared by event, update and rendering code. Renaming one requires
updating every lookup.

## Audio path

The duplex PortAudio stream is requested at 48 kHz. Shared normally captures
one input channel. Exclusive may capture two microphone channels and/or use
stereo output. `paUnifiedCallback` chooses one microphone channel, then applies
high-pass filtering and capture-level processing. Later processing is mono.
Opening the device does not establish the actual microphone-to-speaker
latency: PortAudio and the driver can select a different device period.

- **Recording:** the callback converts cleaned samples to PCM16 and publishes
  them into `RecordingQueue`. The collector appends them to `paAudioBuffer`.
  Stop disables publication, waits for in-flight callbacks, and joins the
  collector before the take is read for Play or Save. Overflow marks the take
  incomplete; it must not be played or exported.
- **Stream:** the same callback runs pitch, the effect labeled Formant, timbre,
  prosody, jitter and volume according to the enabled bits. Callback blocks
  larger than `STREAM_BUFFER_SIZE` are split into 256-frame chunks. The pitch
  and Formant delay lines stay warm at neutral settings; a short crossfade
  bypasses their delay when their effects are inactive or neutral.
- **Play:** a captured take is copied for preview. Shared normally opens a
  separate output stream; Exclusive plays through the already-open duplex
  output. Effects and sliders can change while the take is playing.
- **Save WAV:** the take and selected settings are snapshotted when Save starts.
  The export worker owns a separate processor state and retains the take's
  original duration after accounting for the Formant processor's initial delay.

`Formant.h` implements a variable-delay read-rate transformation used by
Stream, Play and Save. It also moves harmonics and may affect perceived pitch.
Its UI name must not be interpreted as independent, pitch-preserving
spectral-envelope shifting. `Prosody.h` responds to short speech-energy
onsets; it does not understand words or linguistic intonation. `PitchJitter.h`
generates a creative slow pitch instability, not a clinical jitter measurement.

## Thread handoffs and display

Effect selection, slider values, recording state and device-result flags use
atomics. `RecordingQueue` has one audio producer and one collector consumer;
its read/write indices use release/acquire publication. Its fixed buffer
capacity is about 5.46 seconds at 48 kHz. `paAudioBufferMutex` protects the
recording vector, and the PortAudio callback never locks it.

Waveform and FFT input each have a separate fixed-size ring protected by their
own mutex. The callback uses `try_to_lock` and may skip a display update when
the UI owns that mutex. The UI copies the ring quickly under the lock, then
draws or computes the FFT after releasing it. The 4096-sample FFT window is
for visualization only; it does not add a 4096-sample audio-output buffer.
FFT calibration spans the first 60 visual analysis frames with enough input.
The frame scheduler targets 60 FPS; actual delivery depends on the machine.

When an audio device changes, the UI requests a reopen and keeps rendering
while the device worker owns PortAudio operations. A reconnect can pause
incoming waveform updates. On shutdown, stop device callbacks and worker
threads before destroying any data they read; release SDL textures before
destroying the renderer and font.

## Where to change behavior

| Goal | First files to inspect |
| --- | --- |
| Device selection, WASAPI fallback or playback output | `Audio.cpp`, `WasapiPortAudio.h`, device worker in `App.cpp` |
| Live capture or effect ordering | `Stream.cpp`, `Formant.h`, `Timbre.h`, `Prosody.h`, `PitchJitter.h` |
| Recorded preview or WAV processing | `Audio.cpp`, `Wav.cpp`, `RecordingQueue.h` |
| Effect button or slider | `App.cpp`, `AppUpdate.cpp`, `AppRender.cpp`, `Widgets.h` |
| Display FFT or waveform | `Dsp.cpp`, `Visualization.cpp`, `State.h` |

Source comments describe ownership and non-obvious constraints. Simple
getters and direct assignments are intentionally not narrated line by line.
