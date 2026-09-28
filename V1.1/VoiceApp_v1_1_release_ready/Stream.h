// Public duplex audio interface. Processor instances and temporary buffers are private to Stream.cpp.

#pragma once
#include "State.h"

// Handle one duplex audio block. Capture mode stores PCM16 and mutes output; stream mode processes
// and emits floats.
int paUnifiedCallback(const void*, void*, unsigned long, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*);

// Create duplex processors and buffers on demand before enabling stream mode.
bool InitStreamMode();
// Initialize processors if necessary, then enable the atomic stream-mode flag.
void ActivateStreamMode();
// Disable duplex processing through the public stream API.
void DeactivateStreamMode();
// Release the private live processors and their temporary buffers on application shutdown.
void CleanupStreamMode();
