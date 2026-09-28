# VoiceApp V1.1 — developer guide

This guide describes the implementation, including inherited limitations. The
project consists of nine source files and nine headers. Source comments explain
local decisions; use this guide to follow data across modules and threads.

## Build in Visual Studio Community 2026 v18.10.2

1. Copy your working Visual Studio project.
2. Add the nine `.cpp` files under **Source Files** and the nine `.h` files under
   **Header Files**. Exclude the earlier monolithic `.cpp` from compilation.
3. Keep your existing C++20 configuration and SDL3, SDL3_ttf and PortAudio
   headers, libraries and DLLs. `App.cpp` defines `SDL_MAIN_USE_CALLBACKS` and
   implements `SDL_AppInit`, `SDL_AppEvent`, `SDL_AppIterate`, `SDL_AppQuit`;
   do not add a separate `main()`.
4. Test Debug and Release x64 on the same input and output devices. This
   package does not include a `.vcxproj`, external SDKs or runtime assets.

`logo.bmp` is loaded from the working directory; launch the portable build from
its own directory if you include this asset. The font paths are set in
`App.cpp`. `Wav.cpp` saves into `recordings/` beside the executable, so extract
the portable build into a writable directory.

## Where to find a change

| File | Responsibility | Read this when... |
| --- | --- | --- |
| `Common.h` | Shared types, constants, widget actions, WAV header | A default, unit or format is unclear |
| `State.h/.cpp` | Declarations and single definitions of shared state | Data crosses modules or threads |
| `App.cpp` | SDL lifecycle, widgets and button action dispatch | A UI action needs changing |
| `AppUpdate.cpp` | Playback state, slider publication, FFT updates | A DSP value or spectrum is stale |
| `AppRender.cpp` | Pointer interaction and drawing | A widget moves or draws incorrectly |
| `Widgets.h` | `UiWidget` geometry, hit testing and registry | Working on buttons or sliders |
| `Audio.h/.cpp` | PortAudio setup, capture and recorded playback | Recording or playback fails |
| `Stream.h/.cpp` | Live duplex callback and processors | Live microphone processing fails |
| `Dsp.h/.cpp` | PCM16 transformations and spectrum FFT | Processing saved data or FFT |
| `Visualization.h/.cpp` | SDL text, waveform and spectrum | Visual output differs from audio |
| `Wav.h/.cpp` | Filename, RIFF header and PCM16 output | WAV files are missing or malformed |

`g_streamPitchShifter`, `g_streamFormantShifter` and `g_streamBuffers` are
private to `Stream.cpp`. UI code uses `ActivateStreamMode()` and
`DeactivateStreamMode()`; it cannot access those processor pointers directly.

## Runtime and data flow

1. `SDL_AppInit` initializes visualization buffers, SDL, PortAudio, latency
   metrics, window, font and the named `widgets` collection. `InitPortAudio`
   opens the duplex stream with `paUnifiedCallback`.
2. PortAudio calls `paUnifiedCallback` for each input/output block. Input is
   interleaved `float32` if there are multiple channels. The callback reads
   the first channel, applies a simplified high-pass, estimates RMS, and uses
   a noise threshold and expander.
3. In **recording mode**, the callback updates waveform/FFT inputs, stores
   mono PCM16 bytes in `paAudioBuffer` when `paIsRecording` is true, and writes
   silence to the output. In **stream mode**, pitch, formant, jitter and volume
   processing feed the output channels. `g_streamModeActive` selects the mode.
4. `SDL_AppIterate` first calls `UpdatePlaybackAndAnalysis`, then `RenderFrame`.
   The update publishes four slider values into `atomic<float>` settings,
   handles playback completion and calculates the display FFT. Rendering
   draws controls, waveform and spectrum, then presents the frame.
5. `SDL_AppEvent` records the pressed widget on mouse-down and dispatches its
   `WidgetAction` on release if the pointer remains over that widget. Buttons
   start/stop recording or one of the playback variants, write four WAV
   variants, or toggle stream mode.
6. `SDL_AppQuit` tears down stream processors, PortAudio and other explicitly
   owned state. Its inherited order destroys callback-visible objects before
   stopping the audio stream, which can race an active callback. Playback and
   WAV transformations in `Audio.cpp` use separate processors from the live
   duplex ones in `Stream.cpp`.

Lookup strings matter: `AnimatedWidget`, `WidgetInfo`, `AudioMode`,
`Slider_PitchShift`, `Slider_FormantShift`, `Slider_Jitter` and `Slider_Volume`
are names registered in `SDL_AppInit` and referenced from other modules. Keep
creation and lookup strings identical. The audio mode caption is also compared
as text; update both the comparison and the text if you change it.

## Units and parameters

| Value | Meaning |
| --- | --- |
| `PA_SAMPLE_RATE = 16000` | Requested PortAudio and WAV sample rate in Hz |
| `STREAM_BUFFER_SIZE = 320` | Float array capacity: 20 ms at 16 kHz |
| `PA_FFT_SIZE = 1024` | Display FFT window: 64 ms at 16 kHz |
| `PA_NUM_BARS = 69` | Number of displayed spectrum columns |
| `paAudioBuffer` | Headerless signed 16-bit PCM bytes |
| `g_pitchShiftSemitones` | Slider from -12 to +12 semitones, default +3 |
| `g_formantShiftRatio` | Slider from 0.5 to 2.0, default 1.18 |
| `g_jitterAmount` | Slider from 0 to 0.02, default 0.005 |
| `g_outputVolume` | Multiplier from 0 to 1, default 0.50 |

The playback callbacks do not all use the duplex callback size. Changing the
sample rate or block size requires reviewing filters, delay lines, FFT
windows, static arrays and time conversion. Widget positions are in renderer
pixels; slider labels use displayed units. Higher values on vertical sliders
are at the top of the track.

## Threads and callback behavior

SDL frame logic and PortAudio callbacks can run on different threads. Four DSP
settings and the stream flag are atomic. The display FFT input uses
`paFftMutex`. `paIsRecording`, `paAudioBuffer`, the waveform and several
playback positions do not have a complete synchronization contract.

The callback still allocates a `cleanedSamples` vector, grows some vectors,
and acquires FFT and latency locks. Preallocated buffers elsewhere do not make
this callback allocation-free or bounded in execution time.

`LatencyMetrics` timestamps T1..T4 are captured **inside** the audio callback.
The report measures portions of software processing; it does not measure the
physical time from microphone to speaker. A round-trip measurement would
need separate instrumentation.

## Known inherited limitations

- The stereo detection in `InitPortAudio` inspects `paAudioBuffer` during a
  trial in which recording is disabled, so it cannot reliably establish true
  stereo input. Recording stores one sample per frame even with two input
  channels. A stereo WAV header for that mono byte stream would be inconsistent.
- The stream “formant” processor uses a ratio, delay and crossfade without an
  explicit spectral-envelope analysis. Its name does not prove independent
  formant preservation.
- `AppUpdate.cpp` adds 256 to an FFT counter per SDL frame; the counter does
  not reflect the number of captured audio samples.
- `UiWidget::updatePos()` has branches without an explicit return.
  `UiWidgetCollection::remove()` copies a widget with internal pointers into
  its own vertices. Review both before depending on their edge behavior.
- The branch for `Pa_GetDefaultOutputDevice() == paNoDevice` later accesses
  output device information despite claiming to continue with input only.
- Waveform rendering reads shared data without a complete lock protocol.
  Spectrum calibration depends on the number of initial FFT frames.
- Three playback callbacks check for a null output pointer but then write
  through it on that path. Fix this before relying on null-output recovery.

Make each future fix separately and compare it with a reference recording so
that unintended audio changes are detectable.

## Adding and verifying changes

To add a button: extend `WidgetAction`, construct and configure its widget in
`SDL_AppInit`, handle the action in `SDL_AppEvent`, then test press/release.

To add a DSP control: configure its units and range in `SDL_AppInit`, publish
its value in `AppUpdate.cpp`, read it in the audio callback, and define the
cross-thread synchronization policy.

To change processing: decide whether it affects recorded PCM16 playback/saving
(`Audio.cpp`, `Dsp.cpp`) or live duplex (`Stream.cpp`). Compare silence,
transients, extreme settings and mode transitions using the same sample.

For a delivery test: build Debug and Release x64, record and play the original
and three processed variants, save and inspect all four WAV files in another
player, toggle stream mode, exercise every slider, and restart the app. Use
the previous executable as a listening reference. No Windows build or live
audio test was possible in the environment that produced this package.
