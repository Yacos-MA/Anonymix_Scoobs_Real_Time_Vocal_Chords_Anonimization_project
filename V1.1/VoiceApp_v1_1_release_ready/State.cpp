// Definitions for State.h. Keep each global here so all translation units link to one instance.

#include "State.h"
#include "Widgets.h"

// Allocate the diagnostics object in SDL_AppInit after audio setup; nullptr means there is no
// report to publish.
LatencyMetrics* g_latencyMetrics = nullptr;
SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Surface* surface = NULL;
SDL_Texture* texture = NULL;
TTF_Font* gFont = nullptr;

// These rectangles and toggles are the inherited decorative animation, independent of microphone
// samples.
const SDL_FRect rect1 = { 0, 0, 320, 480 };
const SDL_FRect rect2 = { 320, 0, 320, 480 };
SDL_FRect rect3 = { 200, 160, 40, 40 };
SDL_FRect rect4 = { 200, 160, 40, 40 };

const SDL_PixelFormatDetails* pxFormatRGBA64 = nullptr;

SDL_Texture* gLogoTexture = nullptr;
SDL_FRect    gLogoRect = { 0.f, 0.f, 0.f, 0.f };

bool toggle1 = false, toggle2 = false, toggle3 = false, toggle4 = false, toggle5 = false;
bool toggle6 = false, toggle7 = false, toggle8 = false, toggle9 = false, toggle10 = false;
int sevenBitsValue = 0, tenBitsValue = 0;

// The SDL frame loop refreshes the pointer position; the event handler reads it to compute drag
// offsets.
float mx, my;
float x, y;
Uint32 mousePos;

bool boolVar = false;
double angleGeo = 0;
float red, green, blue;

Uint8 grey_r = 127;
Uint8 grey_g = 127;
Uint8 grey_b = 127;
Uint8 grey_a = 255;

Uint32 color_grey = 0;

// Remember pointer-to-handle displacement at mouse-down so a drag does not jump the handle center.
SDL_FPoint mouseClick_OFFSET = { 0, 0 };

// Protect the FFT input snapshot. Other shared UI/audio values still require a separate
// concurrency review.
mutex paFftMutex;

// This ring is sized at startup and written by the callback. Visualization reads it from the SDL
// thread.
vector<float> waveformBuffer;
size_t waveformWritePos = 0;
bool waveformDataReady = false;

// FIFO history of cleaned samples, capped by callback code at roughly two FFT windows.
vector<float> paFftInputBuffer;
vector<float> paFftMagnitudes;
vector<float> paFftSmoothed;
bool paFftDataReady = false;

// Per-bar noise-floor estimate acquired during the initial spectrum calibration phase.
vector<float> paAdaptiveNoiseFloor;
bool paIsCalibrating = true;
int paCalibrationFrames = 0;

// The recording buffer persists after stop for playback and WAV export; starting a new recording
// clears it.
vector<Uint8> paAudioBuffer;
bool paIsRecording = false;

// The duplex stream and variant playback streams are opened by Audio.cpp and closed in
// CleanupPortAudio.
PaStream* paStream = nullptr;

bool paIsPlaying = false;
size_t paPlaybackPosition = 0;
PaStream* paPlaybackStream = nullptr;

bool paIsPlayingAnonymized = false;
size_t paPlaybackAnonymizedPosition = 0;
PaStream* paPlaybackAnonymizedStream = nullptr;
vector<Uint8> paProcessedBuffer;

bool paIsPlayingAnonymizedFormant = false;
size_t paPlaybackAnonymizedFormantPosition = 0;
PaStream* paPlaybackAnonymizedFormantStream = nullptr;
vector<Uint8> paProcessedFormantBuffer;

bool paIsPlayingAnonymizedPitchFormant = false;
size_t paPlaybackAnonymizedPitchFormantPosition = 0;
PaStream* paPlaybackAnonymizedPitchFormantStream = nullptr;
vector<Uint8> paProcessedPitchFormantBuffer;

// Initial UI settings: +3 semitones, formant ratio 1.18, jitter 0.005 and 50% output volume.
atomic<float> g_pitchShiftSemitones{ 3.0f };
atomic<float> g_formantShiftRatio{ 1.18f };
atomic<float> g_jitterAmount{ JITTER_AMOUNT };
atomic<float> g_outputVolume{ 0.50f };

int paMicrophoneChannels = 1;
UiWidget* hoveredWidget = nullptr;
UiWidget* pressedWidget = nullptr;

// One named collection shared by event, update and render code. Add each lookup key during
// SDL_AppInit.
UiWidgetCollection widgets;
atomic<bool> g_streamModeActive{ false };
