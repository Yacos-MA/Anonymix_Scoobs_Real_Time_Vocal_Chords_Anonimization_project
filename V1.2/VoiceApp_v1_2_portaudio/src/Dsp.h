// PCM16 sample processing and FFT analysis declarations. Byte vectors contain packed signed 16-bit samples.

#pragma once
#include "State.h"

// Scale signed 16-bit audio samples in place toward the existing 70% target peak.
void NormalizeAudio(vector<Uint8>&);
// Apply the existing simple high-pass stage to packed PCM16 samples.
void ApplyHighPassFilter(vector<Uint8>&, float cutoffFreq = 80.0f);
// Perturb samples while retaining the original behavior for silence and sample limits.
void ApplyJitter(vector<Uint8>&, float);

// Permute FFT input indices before the in-place radix-2 butterfly passes.
void BitReversalPermutation(vector<complex<float>>&);
// Compute a radix-2 FFT; the input length must be a power of two.
void FFT_CooleyTukey(vector<complex<float>>&);
// Reduce FFT edge discontinuity by weighting each sample with a Hann window.
void ApplyHannWindow(vector<float>&);

// Copy recent captured samples under paFftMutex, calculate bins and update the visible magnitudes.
void ComputeFFT_PortAudio();
// Blend current FFT bars with earlier values to reduce visual flicker.
void SmoothFFTDisplay_PortAudio(float smoothingFactor = 0.3f);
