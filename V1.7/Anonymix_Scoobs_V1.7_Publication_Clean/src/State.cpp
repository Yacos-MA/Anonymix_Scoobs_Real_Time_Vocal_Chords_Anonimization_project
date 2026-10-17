// Definitions for State.h. Keep each global here so all translation units link to one instance.

#include "State.h"
#include "Widgets.h"

SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
TTF_Font* gFont = nullptr;

// These rectangles and toggles are the inherited decorative animation, independent of microphone
// samples.
const SDL_FRect rect1 = { 0, 0, 320, 480 };
const SDL_FRect rect2 = { 320, 0, 320, 480 };
SDL_FRect rect3 = { 200, 160, 40, 40 };
SDL_FRect rect4 = { 200, 160, 40, 40 };


bool toggle1 = false, toggle2 = false, toggle3 = false, toggle4 = false, toggle5 = false;
bool toggle6 = false, toggle7 = false, toggle8 = false, toggle9 = false, toggle10 = false;
int sevenBitsValue = 0, tenBitsValue = 0;

// The SDL frame loop refreshes the pointer position; the event handler reads it to compute drag
// offsets.
float mx, my;
float red, green, blue;

// Remember pointer-to-handle displacement at mouse-down so a drag does not jump the handle center.
SDL_FPoint mouseClick_OFFSET = { 0, 0 };

// Protect both the FFT sample ring and its read/write positions. Other shared state has its own
// ownership rules: atomics for controls and capture flags, mutexes for recording and waveform.
mutex paFftMutex;
size_t paFftWritePos = 0;
size_t paFftSamplesAvailable = 0;
mutex waveformMutex;

// This ring is sized at startup and written by the callback. Visualization reads it from the SDL
// thread.
vector<float> waveformBuffer;
size_t waveformWritePos = 0;
atomic<bool> waveformDataReady{ false };

// Fixed-capacity circular history of cleaned samples, sized during SDL_AppInit to two FFT
// windows. The callback overwrites old samples and never grows this vector.
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
atomic<bool> paIsRecording{ false };

// The duplex stream is opened by Audio.cpp and closed in CleanupPortAudio.
PaStream* paStream = nullptr;
atomic<bool> paIsPlayingSelected{ false };
atomic<uint8_t> g_selectedEffects{ EFFECT_PITCH | EFFECT_FORMANT };
mutex paAudioBufferMutex;

// Initial UI settings: 0 semitones, neutral formant ratio 1.00, neutral timbre,
// jitter off at 0.000 and 50% output volume.
atomic<float> g_pitchShiftSemitones{ 0.0f };
atomic<float> g_formantShiftRatio{ 1.0f };
atomic<float> g_timbrePercent{ 0.0f };
atomic<float> g_prosodyPercent{ 0.0f };
atomic<float> g_jitterAmount{ JITTER_AMOUNT };
atomic<float> g_outputVolume{ 0.50f };

int paMicrophoneChannels = 1;
int paOutputChannels = 1;
UiWidget* hoveredWidget = nullptr;
UiWidget* pressedWidget = nullptr;

// One named collection shared by event, update and render code. Add each lookup key during
// SDL_AppInit.
UiWidgetCollection widgets;
atomic<bool> g_streamModeActive{ false };
