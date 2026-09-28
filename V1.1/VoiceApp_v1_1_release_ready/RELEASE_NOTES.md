# Anonymix Scoobs v1.1 — Windows x64

V1.1 was originally planned immediately after V1 in February 2026. More
urgent commitments prevented me from returning to the project at that time.
I resumed work later and am publishing V1.1 at the end of September 2026.

This release provides a portable Windows x64 build of the real-time voice
processing application.

## Install and run

1. Download the ZIP and extract it into a writable folder.
2. Launch `Anonymix_Scoobs.exe` from that folder, keeping the included DLLs
   and `logo.bmp` alongside it.
3. Saved WAV files appear in `recordings/` beside the executable.

A compatible microphone/output device and the Microsoft Visual C++
Redistributable for x64 are required. This is a portable build, not an
installer. The project author tested recording and live processing on Windows.

Runtime components: SDL3 3.4.16, SDL_ttf 3.2.2, and the supplied PortAudio
binary reporting V19.7.0-devel. See `THIRD_PARTY_NOTICES.txt` and `LICENSES/`.
Known implementation limitations are described in the developer guide in
the source repository.
