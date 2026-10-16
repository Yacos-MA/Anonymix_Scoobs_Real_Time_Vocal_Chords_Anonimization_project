# Anonymix Scoobs

Anonymix Scoobs is an experimental Windows application for live voice effects, recording, playback, and audio visualization. It began as a capstone project to explore audio processing and has continued as a personal software project. The application runs locally; its audio features do not require an internet connection.

**Current version: V1.5.** This is a creative tool and a learning project, not a verified voice anonymization system. Do not rely on it to conceal your identity in a sensitive situation.

## Download

See the [V1.5 release](https://github.com/Yacos-MA/Anonymix_Scoobs_Real_Time_Vocal_Chords_Anonimization_project/releases/tag/v1.5) for the available Windows package and the complete source archive. If you download a Windows executable package, extract the whole ZIP and keep the supplied DLLs, icon, and third-party notices with the application.

The application sources and detailed build notes are in [`V1.5/Anonymix_Scoobs_V1.5_Dev/`](V1.5/Anonymix_Scoobs_V1.5_Dev/) and its [`BUILD_AND_TEST.md`](V1.5/Anonymix_Scoobs_V1.5_Dev/BUILD_AND_TEST.md). The source archive does not include a Visual Studio solution, import libraries, or a prebuilt executable.

## What V1.5 does

- **Stream mode:** processes microphone audio and plays it through the active audio output.
- **Recording mode:** captures a take, then uses one Play button and one Save WAV button with the currently selected effects. A saved WAV keeps the take's duration.
- **Selectable effects:** pitch, the formant-named voice transformation, timbre, prosody, and jitter. Each has one selection button and one slider, and effects can be combined.
- **Visuals:** waveform and FFT displays in an SDL3 interface targeting 60 frames per second. The spectrum display covers approximately 20 Hz to 20 kHz.
- **Device recovery:** Reopen Audio requests device discovery after an input or output change without stopping the interface's visual loop.

The application's capture, DSP, playback, and WAV export use a **48 kHz processing rate**. There is no internal 48-to-16-to-48 kHz conversion path. Windows shared-mode audio or a device driver may still convert between the application's format and a device's format. The application does not guarantee a particular end-to-end latency on every computer.

The effect labelled **Formant** uses the same variable-delay processor in Stream mode, recorded playback, and WAV export. It changes the rate at which stored audio is read and can affect the harmonics and perceived pitch. It does not independently shift the vocal-tract spectral envelope while keeping the fundamental pitch fixed. **Prosody** is an energy-onset-driven pitch and level gesture, not linguistic analysis; **Jitter** is an artistic pitch instability, not a clinical measurement.

## Build from source

The Windows source set targets **Visual Studio Community 2026**, **x64**, and **C++20**. It uses **SDL3** for the interface, **SDL3_ttf** for text, and **PortAudio** for audio input and output. V1.5 does **not** require JUCE. The supplied runtime package includes SDL3 3.4.16, SDL3_ttf 3.2.2, and the PortAudio DLL used in the earlier Windows package; the existing Visual Studio project still needs suitable headers, import libraries, and linker settings.

Follow the [V1.5 build instructions](V1.5/Anonymix_Scoobs_V1.5_Dev/BUILD_AND_TEST.md) when replacing the application's `.cpp` and `.h` files in that project. Keep the notices under `runtime/` with any distributed third-party DLLs.

## Project history and direction

| Version | Focus |
| --- | --- |
| V1 | Initial capstone exploration of voice processing and visualization. |
| V1.1 | Source organization, documentation, and a Windows package. |
| V1.2 | 48 kHz application processing and audio-backend experiments. The official application continues with PortAudio and SDL3. |
| V1.5 | Unified playback and WAV export, selectable voice effects, prosody and jitter, updated visuals, and the shared Stream/Recording formant-named processor. |

**V2.0** may move substantial parts of the application to Rust if measurements and development show a clear benefit. Its scope and architecture will follow the results of that work. No release date has been set.

The project name may change as the application evolves. A voice that sounds different is not necessarily anonymous: the degree of protection against speaker identification has not been established.
