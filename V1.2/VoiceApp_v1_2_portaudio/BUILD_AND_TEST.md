# Anonymix Scoobs V1.2 — Windows release candidate

This package contains the complete application source set (9 `.cpp` files and
11 `.h` files) for Visual Studio Community 2026. It uses SDL3 and SDL3_ttf for
the user interface and PortAudio for live audio, recording, and playback. It is
**source code, not a prebuilt executable**. Publish it only after building and
testing it on your Windows machine.

## Replace the sources in the existing Visual Studio project

1. Back up your existing Visual Studio project. Extract the ZIP to a writable
   directory and copy **every** file in `src/` into the existing application
   source directory, replacing matching files.
2. In Solution Explorer, remove all old application translation units or
   headers that are not listed in `src/`. Ensure the project compiles the nine
   `.cpp` files from `src/` exactly once each. If the earlier experimental
   configuration added other framework modules, remove those module files and
   their include directories, library paths, link dependencies, and preprocessor
   definitions from **all** project configurations. A clean rebuild removes
   stale object files from the previous configuration.
3. Retain the SDL3, SDL3_ttf and PortAudio include directories, link settings,
   and import libraries already used by your working project. Choose **x64**,
   **C++20**, and the same working Release settings as your previous build.
4. Copy the files from `runtime/` beside the newly built executable:
   `SDL3.dll`, `SDL3_ttf.dll`, `portaudio.dll`, and `logo.bmp`. The bundled
   binaries come from the previous portable distribution (SDL3 3.4.16,
   SDL3_ttf 3.2.2, and the previously supplied PortAudio DLL). Keep the
   supplied `THIRD_PARTY_NOTICES.txt` and `LICENSES/` with any redistributed
   runtime files. If you substitute different DLLs, use their matching notices.

There is no extra audio framework setup or build switch in this source set.
The processing and saved audio run at 48 kHz. The SDL visual loop targets 60
frames per second; actual frame rate can be lower when the system is busy.

## Test before publishing

- Start in Recording mode. Record a short take, stop it, test `Play OG`,
  `Play Pitch`, `Play Formant`, `Play P+F`, and `Play Timbre`, then save each
  recording variant. Verify the WAV files play and use 48 kHz mono output.
- Change the default Windows output and try all five playback buttons again.
  Each recorded playback stream closes on stop or completion and opens the
  current default output on its next start.
- Enter Stream mode. Listen to pitch, formant and timbre at several slider
  positions, as well as neutral values. Confirm that the FFT and waveform
  remain visible and that the UI stays responsive.
- Change or reconnect a microphone or live output, click `Reopen Audio`, then
  check the actual device names and the `AUDIO DEVICE REOPEN` result in the
  Output window. Recorded output selection does not require this button, but
  live input/output changes do. Stop an active recording before reopening.
- Observe `Reopen UI: ... intervals over 33 ms, longest ... ms` and the
  ten-second callback report for dropouts, overflow and underflow counts.
  Repeat in Release x64 and close the application during a normal Stream run.

Reopening a device may take 1–2 seconds on the Windows configuration already
tested. The device worker keeps the SDL window active during that operation,
but the spectrum has no fresh input and holds its last trace until audio
resumes. Reported callback and host timing estimates do not measure physical
microphone-to-speaker delay. A 48 kHz stream request does not prove that the
Windows shared audio engine performs no conversion internally.

## Verification status

The source inventory and archive integrity were checked here. This environment
cannot compile the full Windows Visual Studio application or exercise its
microphone, speakers and device switching. The archive is a candidate for your
local tests, not an already validated executable or a published release.
