// Declarations of shared state. Each extern is defined once in State.cpp. The audio callback and SDL frame code may run on different threads.

#pragma once
#include "Common.h"

class UiWidget;
class UiWidgetCollection;

// SDL handles and drawing state belong to the UI. Widget and visualization functions use renderer
// and gFont.
extern SDL_Window* window;
extern SDL_Renderer* renderer;
extern TTF_Font* gFont;
extern const SDL_FRect rect1;
extern const SDL_FRect rect2;
extern SDL_FRect rect3;
extern SDL_FRect rect4;
// Legacy toggles drive the decorative animation in AppRender.cpp; they are not audio-mode flags.
extern bool toggle1;
extern bool toggle2;
extern bool toggle3;
extern bool toggle4;
extern bool toggle5;
extern bool toggle6;
extern bool toggle7;
extern bool toggle8;
extern bool toggle9;
extern bool toggle10;
extern int sevenBitsValue;
extern int tenBitsValue;
// Mouse coordinates and the pressed/hovered widget pointers connect SDL_AppEvent with per-frame
// hit testing.
extern float mx, my;
extern float red, green, blue;
extern SDL_FPoint mouseClick_OFFSET;

// The callback appends FFT input while the UI copies it for analysis. Each display ring has its
// own mutex, which protects both its storage and its write position.
extern mutex paFftMutex;
// FFT samples are kept in a fixed-size ring. Both positions are protected by paFftMutex.
extern size_t paFftWritePos;
extern size_t paFftSamplesAvailable;
extern mutex waveformMutex;
// Circular float waveform used only for display; waveformWritePos indexes the next value to be
// written.
extern vector<float> waveformBuffer;
extern size_t waveformWritePos;
extern atomic<bool> waveformDataReady;
// Recent cleaned float samples feed the FFT. The resulting magnitudes, smoothing and calibration
// state drive the spectrum view.
extern vector<float> paFftInputBuffer;
extern vector<float> paFftMagnitudes;
extern vector<float> paFftSmoothed;
extern bool paFftDataReady;
extern vector<float> paAdaptiveNoiseFloor;
extern bool paIsCalibrating;
extern int paCalibrationFrames;

// Capture storage is raw little-endian mono PCM16 without a WAV header. The duplex callback
// publishes cleaned samples to RecordingQueue; the collector thread appends bytes here.
extern vector<Uint8> paAudioBuffer;
// Recording and playback flags cross between the SDL and PortAudio threads.
extern atomic<bool> paIsRecording;
// The duplex stream and the selected recording preview are owned by Audio.cpp.
extern PaStream* paStream;
extern atomic<bool> paIsPlayingSelected;
extern atomic<uint8_t> g_selectedEffects;

// SDL publishes slider settings through atomics; the audio callback reads them without touching
// widgets. Display rings and the recording vector use separate synchronization mechanisms.
extern atomic<float> g_pitchShiftSemitones;
extern atomic<float> g_formantShiftRatio;
// -100 is darker, zero is neutral and +100 is brighter. The audio thread smooths changes.
extern atomic<float> g_timbrePercent;
// Negative bends give restrained falling accents, positive bends rising accents;
// zero leaves speech unchanged by this effect.
extern atomic<float> g_prosodyPercent;
// The recording collector locks this mutex while growing the PCM vector. The device worker and
// export path lock it to reset or copy the take. The PortAudio callback never takes this lock.
extern mutex paAudioBufferMutex;
extern atomic<float> g_jitterAmount;
extern atomic<float> g_outputVolume;
// Shared mode normally captures mono; Exclusive may open a stereo microphone array. The duplex
// callback selects one channel and all downstream effect processing remains mono.
extern int paMicrophoneChannels;
// Set before the duplex stream starts; Exclusive endpoints may require stereo output.
extern int paOutputChannels;
// Pointer lifetime depends on the named list of widgets in State.cpp. Removing or replacing a
// widget while pressed would invalidate pointers.
extern UiWidget* hoveredWidget;
extern UiWidget* pressedWidget;
extern UiWidgetCollection widgets;

// The unified callback uses this flag to select recording or duplex processing.
extern atomic<bool> g_streamModeActive;
