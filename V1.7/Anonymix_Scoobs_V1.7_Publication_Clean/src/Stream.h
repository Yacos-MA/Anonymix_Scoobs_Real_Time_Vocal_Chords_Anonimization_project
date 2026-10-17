// Public duplex audio interface. Processor instances and temporary buffers are private to Stream.cpp.

#pragma once
#include "State.h"

// Handle one duplex audio block. Recording mode publishes cleaned samples to a lock-free PCM16
// queue; Stream mode processes floats and sends them to the output device.
int paUnifiedCallback(const void*, void*, unsigned long, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*);

// Request a clean high-pass, expander and channel/gain state for a newly opened device stream.
// The callback observes the new generation and resets its own state on its next invocation.
void ResetCaptureProcessingForNewDevice() noexcept;

// Create duplex processors and buffers on demand before enabling stream mode.
bool InitStreamMode();
// Initialize processors if necessary, then enable the atomic stream-mode flag.
void ActivateStreamMode();
// Disable duplex processing through the public stream API.
void DeactivateStreamMode();
// Release the private live processors and their temporary buffers on application shutdown.
void CleanupStreamMode();
