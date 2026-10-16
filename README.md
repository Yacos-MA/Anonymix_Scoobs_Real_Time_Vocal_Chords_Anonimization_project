# Anonymix Scoobs

Anonymix Scoobs is an experimental real-time voice transformation application for Windows. It began as a project for testing and learning about audio processing as part of my capstone thesis. I have since continued developing it beyond that original scope.

The application offers recording, playback, and live monitoring with pitch, formant, and timbre controls. It uses SDL3 for the interface and PortAudio for audio input and output. The V1.2 application processes audio at 48 kHz.

## Current status

V1.2 has been tested locally. Its Windows release package uses PortAudio without JUCE. JUCE was evaluated during V1.2 development for audio handling and selected processing tasks; the tests did not show a clear enough benefit to include it in the official V1.2 package. These experiments can inform future comparisons.

This is a research and hobby project. **It does not guarantee anonymity or protect a speaker's identity.** Do not rely on it for situations where voice identification would have serious consequences. The project's name may change as its goals and capabilities evolve.

## Roadmap

- **V1.1 — completed:** Reorganize and document the C++ codebase.
- **V1.2 — completed:** Run the application at 48 kHz, improve the live audio path and interface responsiveness, add timbre control, and compare experimental JUCE approaches with the PortAudio implementation. The release package uses PortAudio only.
- **V1.5 — planned:** Improve audio quality and reduce measured end-to-end latency where practical. Continue evaluating device handling and additional voice transformations. Revisit JUCE only if a targeted comparison shows a meaningful benefit while keeping SDL3 as the interface.
- **V2.0 — proposed:** Consider a predominantly Rust implementation, especially for memory and thread safety, if V1.5 measurements and a working prototype justify the migration.

The roadmap may change as the work is tested. No release dates have been set.
