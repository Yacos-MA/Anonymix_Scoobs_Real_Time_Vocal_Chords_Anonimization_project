#define SDL_MAIN_USE_CALLBACKS 1  /* use the callbacks instead of main() */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <list>
#include <type_traits>
#include <format>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <utility>
#include <stdexcept>
#include <random>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>
#include <mutex>
#include <ctime>
#include <atomic>
#include <array>

// Windows
#ifdef _WIN32
#include <direct.h>   // pour _mkdir
#define mkdir(dir, mode) _mkdir(dir)
#else
// #include <sys/stat.h> // pour mkdir sur Unix
#endif

// === PORTAUDIO ===
#include <portaudio.h>

using namespace std;

// ============================================================================
// SYSTÈME DE MESURE DE LATENCE AUDIO
// ============================================================================

#include <chrono>

// Structure pour stocker les timestamps et mesures de latence
struct LatencyMetrics {
    using TimePoint = chrono::high_resolution_clock::time_point;
    using Duration = chrono::duration<double, milli>;

    // === TIMESTAMPS CRITIQUES ===
    TimePoint t1_microphoneCapture;        // T1: Audio arrive du micro
    TimePoint t2_readyForDSP;              // T2: Prêt à être traité (après filtre HP 80Hz)
    TimePoint t3_readyForOutput;           // T3: Prêt à être envoyé (après DSP complet)
    TimePoint t4_sentToSpeaker;            // T4: Envoyé au driver audio

    // === MESURES DE LATENCE (ms) ===
    double latency_1_capture_to_dsp;       // T2 - T1
    double latency_2_dsp_processing;       // T3 - T2 (DSP + jitter + norm + volume)
    double latency_3_output_to_playback;   // T4 - T3
    double latency_4_non_processing;       // (T2-T1) + (T4-T3)
    double latency_5_total;                // T4 - T1

    // === COMPTEURS POUR MOYENNES ===
    vector<double> samples_latency1;
    vector<double> samples_latency2;
    vector<double> samples_latency3;
    vector<double> samples_latency4;
    vector<double> samples_latency5;

    mutex metricsMutex;
    int secondCounter = 0;

    LatencyMetrics() {
        samples_latency1.reserve(100);
        samples_latency2.reserve(100);
        samples_latency3.reserve(100);
        samples_latency4.reserve(100);
        samples_latency5.reserve(100);
    }

    void recordSample() {
        lock_guard<mutex> lock(metricsMutex);

        // Calculer les latences
        latency_1_capture_to_dsp = Duration(t2_readyForDSP - t1_microphoneCapture).count();
        latency_2_dsp_processing = Duration(t3_readyForOutput - t2_readyForDSP).count();
        latency_3_output_to_playback = Duration(t4_sentToSpeaker - t3_readyForOutput).count();
        latency_4_non_processing = latency_1_capture_to_dsp + latency_3_output_to_playback;
        latency_5_total = Duration(t4_sentToSpeaker - t1_microphoneCapture).count();

        // Stocker les échantillons
        samples_latency1.push_back(latency_1_capture_to_dsp);
        samples_latency2.push_back(latency_2_dsp_processing);
        samples_latency3.push_back(latency_3_output_to_playback);
        samples_latency4.push_back(latency_4_non_processing);
        samples_latency5.push_back(latency_5_total);
    }

    void printSecondReport() {
        lock_guard<mutex> lock(metricsMutex);

        if (samples_latency1.empty()) return;

        auto calcAvg = [](const vector<double>& vec) {
            double sum = 0.0;
            for (double v : vec) sum += v;
            return sum / vec.size();
            };

        secondCounter++;

        SDL_Log("=== RAPPORT LATENCE (Seconde #%d) ===", secondCounter);
        SDL_Log("  1) Micro --> Prêt DSP (avec filtre 80Hz): %.3f ms (moy sur %zu échantillons)",
            calcAvg(samples_latency1), samples_latency1.size());
        SDL_Log("  2) Traitement DSP complet: %.3f ms (pitch + formant + jitter + norm + volume)",
            calcAvg(samples_latency2), samples_latency2.size());
        SDL_Log("  3) Prêt sortie --> Joué par speaker: %.3f ms",
            calcAvg(samples_latency3), samples_latency3.size());
        SDL_Log("  4) Temps hors processing: %.3f ms (temps 1 + temps 3)",
            calcAvg(samples_latency4), samples_latency4.size());
        SDL_Log("  5) LATENCE TOTALE: %.3f ms (bout-en-bout)",
            calcAvg(samples_latency5), samples_latency5.size());
        SDL_Log("=====================================");

        // Vider les buffers pour la prochaine seconde
        samples_latency1.clear();
        samples_latency2.clear();
        samples_latency3.clear();
        samples_latency4.clear();
        samples_latency5.clear();
    }
};

// Instance globale
static LatencyMetrics* g_latencyMetrics = nullptr;

// ============================================================================
// DÉCLARATIONS DE CONSTANTES
// ============================================================================

#define ptsQuadr 4
#define vertexTri 3
#define nbColorsRGB 3
#define squareNbTriangle 2

constexpr size_t WAVEFORM_SAMPLES = 2048;
constexpr size_t PA_FFT_SIZE = 1024;
constexpr size_t PA_NUM_BARS = 69;
constexpr float PA_SAMPLE_RATE = 16000.0f;
constexpr size_t FFT_OVERLAP = PA_FFT_SIZE / 2;
constexpr int PA_CALIBRATION_FRAMES = 60;
constexpr float mouseLeftButtonClick_Color_OFFSET = 0.15f;
constexpr float JITTER_AMOUNT = 0.005f;

// ============================================================================
// ÉNUMÉRATIONS
// ============================================================================

enum class QuadType : uint8_t {
    Static_Full = 0,
    Static_DynaZoom,
    Button_StateMono,
    Button_StateDuo,
    Knob_PosN,
    Knob_FreeLimited,
    Knob_Free360,
    SliderH_PosN,
    SliderH_FreeLimited,
    SliderV_PosN,
    SliderV_FreeLimited,
    Slider2D_PosN,
    Slider2D_FreeLimited,
    TextBox,
    List
};

enum class QuadButtonEvent : uint8_t {
    None = 0,
    PA_AudioRecord_Start,
    PA_AudioRecord_Stop,
    PA_AudioPlay_Start,
    PA_AudioPlay_Stop,
    PA_AudioPlayAnonymizedPitch_Start,
    PA_AudioPlayAnonymizedPitch_Stop,
    PA_AudioPlayAnonymizedFormant_Start,
    PA_AudioPlayAnonymizedFormant_Stop,
    PA_AudioPlayAnonymizedPitchFormant_Start,
    PA_AudioPlayAnonymizedPitchFormant_Stop,
    PA_SaveOriginal,
    PA_SaveAnonymizedPitch,
    PA_SaveAnonymizedFormant,
    PA_SaveAnonymizedPitchFormant,
    AudioMode_Toggle
};

enum class colorRGB : Uint8 {
    r = 0,
    g,
    b
};

enum class QuadUnit : uint8_t {
    Pts = 0,
    Hz,
    dB,
    percent,
    cents,
    ms,
    degrees,
};

// ============================================================================
// DÉCLARATIONS DE CLASSES (forward declarations)
// ============================================================================

class SDL_GQuadr;
class SDL_GQuadrs;

// ============================================================================
// VARIABLES GLOBALES SDL
// ============================================================================

SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Surface* surface = NULL;
SDL_Texture* texture = NULL;
TTF_Font* gFont = nullptr;

static const SDL_FRect rect1 = { 0, 0, 320, 480 };
static const SDL_FRect rect2 = { 320, 0, 320, 480 };
SDL_FRect rect3 = { 200, 160, 40, 40 };
SDL_FRect rect4 = { 200, 160, 40, 40 };

// Retirer l'initialisation qui dépend d'une fonction
const SDL_PixelFormatDetails* pxFormatRGBA64 = nullptr;  // Sera initialisé dans SDL_AppInit

SDL_Texture* gLogoTexture = nullptr;
SDL_FRect    gLogoRect = { 0.f, 0.f, 0.f, 0.f };

// ============================================================================
// VARIABLES GLOBALES POUR L'ÉTAT DE L'APPLICATION
// ============================================================================

bool toggle1 = false, toggle2 = false, toggle3 = false, toggle4 = false, toggle5 = false;
bool toggle6 = false, toggle7 = false, toggle8 = false, toggle9 = false, toggle10 = false;
int sevenBitsValue = 0, tenBitsValue = 0;

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

// Retirer l'initialisation qui dépend de pxFormatRGBA64
Uint32 color_grey = 0;  // Sera initialisé dans SDL_AppInit

SDL_FPoint mouseClick_OFFSET = { 0, 0 };

// ============================================================================
// VARIABLES GLOBALES PORTAUDIO
// ============================================================================

mutex paFftMutex;

// Retirer les initialisations avec tailles constexpr dans les constructeurs
vector<float> waveformBuffer;  // Sera redimensionné dans SDL_AppInit
size_t waveformWritePos = 0;
bool waveformDataReady = false;

vector<float> paFftInputBuffer;
vector<float> paFftMagnitudes;  // Sera redimensionné dans SDL_AppInit
vector<float> paFftSmoothed;    // Sera redimensionné dans SDL_AppInit
bool paFftDataReady = false;

vector<float> paAdaptiveNoiseFloor;  // Sera redimensionné dans SDL_AppInit
bool paIsCalibrating = true;
int paCalibrationFrames = 0;

vector<Uint8> paAudioBuffer;
bool paIsRecording = false;

static PaStream* paStream = nullptr;

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

// === PARAMÈTRES DSP CONTRÔLABLES PAR SLIDERS ===
atomic<float> g_pitchShiftSemitones{ 3.0f };      // -12 à +12 semitones
atomic<float> g_formantShiftRatio{ 1.18f };       // 0.5 à 2.0 (×0.5 à ×2)
atomic<float> g_jitterAmount{ JITTER_AMOUNT };    // 0.0 à 0.02
atomic<float> g_outputVolume{ 0.50f };            // 0.0 à 1.0

// === Variable globale pour stocker le nombre de canaux du microphone ===
static int paMicrophoneChannels = 1; // Par défaut mono, sera détecté dans InitPortAudio()

// ============================================================================
// SAUVEGARDE WAV ADAPTATIVE (MONO OU STÉRÉO)
// ============================================================================

// Structure d'en-tête WAV (44 octets)
struct WAVHeader {
    // Chunk RIFF
    char riffID[4];           // "RIFF"
    uint32_t fileSize;        // Taille totale du fichier - 8
    char riffType[4];         // "WAVE"

    // Chunk fmt
    char fmtID[4];            // "fmt "
    uint32_t fmtSize;         // Taille du chunk fmt (16 pour PCM)
    uint16_t audioFormat;     // 1 = PCM
    uint16_t numChannels;     // 1 = mono, 2 = stéréo
    uint32_t sampleRate;      // 16000 Hz
    uint32_t byteRate;        // sampleRate * numChannels * bitsPerSample/8
    uint16_t blockAlign;      // numChannels * bitsPerSample/8
    uint16_t bitsPerSample;   // 16 bits

    // Chunk data
    char dataID[4];           // "data"
    uint32_t dataSize;        // Taille des données audio
};

// ============================================================================
// PROTOTYPES DE FONCTIONS TEMPLATE
// ============================================================================

template<typename T>
inline void Swap(T& a, T& b) noexcept;

template<typename T>
inline T Min(T a, T b) noexcept;

template<typename T>
inline T Max(T a, T b) noexcept;

// ============================================================================
// PROTOTYPES DE FONCTIONS UTILITAIRES
// ============================================================================

static double RandNb_0_to_1();
inline float FindMax(const vector<float>& data) noexcept;

// ============================================================================
// PROTOTYPES DE FONCTIONS DSP
// ============================================================================

void NormalizeAudio(vector<Uint8>& audioData);
void ApplyHighPassFilter(vector<Uint8>& audioData, float cutoffFreq);
void ApplyJitter(vector<Uint8>& audioData, float amount);
void BitReversalPermutation(vector<complex<float>>& data);
void FFT_CooleyTukey(vector<complex<float>>& data);
void ApplyHannWindow(vector<float>& data);
void ComputeFFT_PortAudio();
void SmoothFFTDisplay_PortAudio(float smoothingFactor = 0.3f);

// ============================================================================
// PROTOTYPES DE CALLBACKS PORTAUDIO
// ============================================================================

static int paRecordingCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void* userData);

static int paPlaybackCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void* userData);

static int paPlaybackAnonymizedCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void* userData);

// === DÉCLARATION DU CALLBACK UNIFIÉ (AVANT InitPortAudio) ===
static int paUnifiedCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void* userData);

// ============================================================================
// CONSTANTES MODE STREAM (DÉCLARER AVANT InitPortAudio)
// ============================================================================
constexpr size_t STREAM_BUFFER_SIZE = 320;  // 20ms @ 16kHz

// ============================================================================
// PROTOTYPES DE FONCTIONS PORTAUDIO
// ============================================================================

bool StartPortAudioRecording();
void StopPortAudioRecording();
bool StartPortAudioPlayback();
void StopPortAudioPlayback();
bool StartPortAudioPlaybackAnonymized();
void StopPortAudioPlaybackAnonymized();
bool StartPortAudioPlaybackAnonymizedFormant();
void StopPortAudioPlaybackAnonymizedFormant();
bool StartPortAudioPlaybackAnonymizedPitchFormant();
void StopPortAudioPlaybackAnonymizedPitchFormant();
bool InitPortAudio();
void CleanupPortAudio();

// ============================================================================
// PROTOTYPES DE FONCTIONS DE FORMATAGE ET AFFICHAGE
// ============================================================================

static string FormatDouble(double value, int precision = 2) noexcept;
static string ValueToText(double value, int precision = 0) noexcept;
static string FormatValueWithUnit(double value, QuadUnit unit, int precision = 2) noexcept;
void getSurfaceSize(const string& text, float& surfW, float& surfH, double scale = 1.0);
static bool RenderText(const string& text, float x, float y, SDL_Color color, float& outW, float& outH, double scale = 1.0);
void RenderWaveform(float x, float y, float width, float height);
void RenderFFTSpectrum_PortAudio(float x, float y, float width, float height);

// ============================================================================
// IMPLÉMENTATIONS DES FONCTIONS TEMPLATE (inline dans le header)
// ============================================================================

template<typename T>
inline void Swap(T& a, T& b) noexcept {
    T temp = a;
    a = b;
    b = temp;
}

template<typename T>
inline T Min(T a, T b) noexcept {
    return (a < b) ? a : b;
}

template<typename T>
inline T Max(T a, T b) noexcept {
    return (a > b) ? a : b;
}

// ============================================================================
// CLASSE SDL_GQuadr
// ============================================================================

class SDL_GQuadr {
private:
    int id;
    string name;
    QuadType type;
    uint16_t halfDiag1, halfDiag2;
    double angleDiag1_To2 = 0, angleFig = 0, angleFig_OFFSET = 0;
    double zoomFig = 1;
    double zoomFactor = 1.15;
    bool angD1_To2_Sup_90 = false;
    SDL_FPoint G = { 0, 0 };
    SDL_FPoint Btn_G = { 0, 0 };
    SDL_FPoint G_to_Btn_G_OFFSET = { 0, 0 };
    uint8_t eventType = 0; // 0: aucun, 1: clic-simple bouton-1-etat, 2: clic-simple bouton-2-etats, 3: clic->rotation, 4: clic->glissière-horizontal, 5: clic->glissière-vertical
    SDL_Vertex pointsTri[squareNbTriangle][vertexTri];
    SDL_Vertex* pointsQuadrX[ptsQuadr];
    SDL_Vertex* pointsQuadrY[ptsQuadr];
    SDL_FPoint HitBoxXYmin, HitBoxXYmax;

    bool isMouseLeftButtonDown = false;

    // Background rectangle for Slider types (illustration)
    SDL_FRect bgRect = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool hasBackground = false;

    // Knob rectangle (foreground) positioned on the track
    SDL_FRect bgRect2 = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool hasBgRect2 = false;

    // Couleur de fond (float 0.0..1.0)
    float bg_r = 0.12f;
    float bg_g = 0.12f;
    float bg_b = 0.12f;
    float bg_a = 1.0f;

    // Couleur du slider (noir par défaut)
    float slider_r = 0.0f;
    float slider_g = 0.0f;
    float slider_b = 0.0f;
    float slider_a = 1.0f;

    double slider_xMin;
    double slider_xMax;

    double slider_yMin;
    double slider_yMax;

    double legend_xMin_Val = 0;
    double legend_xMax_Val = 100;

    double legend_yMin_Val = 0;
    double legend_yMax_Val = 100;

    double slider_xVal;
    double slider_yVal;

    double legend_xVal;
    double legend_yVal;

    string legend_xMin_Text;
    string legend_xMax_Text;

    string legend_yMin_Text;
    string legend_yMax_Text;

    string legend_xText;
    string legend_yText;

    QuadUnit legendX_Unit;
    QuadUnit legendY_Unit;

    double valCurrent = 0.0f;

    // Slider value ranges and current values
    double sliderMinX = 0.0f, sliderMaxX = 1.0f, sliderValueX = 0.0f;
    double sliderMinY = 0.0f, sliderMaxY = 1.0f, sliderValueY = 0.0f;

    int nbIntervalsX = 0;
    double rangeSizeX = 0;
    int nbIntervalsY = 0;
    double rangeSizeY = 0;

    QuadButtonEvent buttonFunction = QuadButtonEvent::None;

    // --- TextBox members ---
    string textboxText;
    SDL_Color textboxColor = { 0, 0, 0, 255 }; // default noir

    static inline bool IsSliderType(QuadType t) noexcept {
        switch (t) {
        case QuadType::SliderH_PosN:
        case QuadType::SliderH_FreeLimited:
        case QuadType::SliderV_PosN:
        case QuadType::SliderV_FreeLimited:
        case QuadType::Slider2D_PosN:
        case QuadType::Slider2D_FreeLimited:
            return true;
        default:
            return false;
        }
    }

    static inline bool pointInRect(const SDL_FRect& r, float px, float py) noexcept {
        return (px >= r.x) && (px <= r.x + r.w) && (py >= r.y) && (py <= r.y + r.h);
    }

    // centralise la logique de recalcul du bgRect et du bgRect2 (knob)
    void updateBackgroundRect() noexcept {
        if (!IsSliderType(this->type)) {
            this->hasBackground = false;
            this->hasBgRect2 = false;
            this->bgRect = { 0.0f, 0.0f, 0.0f, 0.0f };
            this->bgRect2 = { 0.0f, 0.0f, 0.0f, 0.0f };
            return;
        }

        // Track (bgRect) : taille selon type et diag/zoom (conservatif)
        if (this->type == QuadType::SliderH_PosN || this->type == QuadType::SliderH_FreeLimited) {
            float w = 440.0f;
            float h = max(8.0f, static_cast<float>(this->halfDiag2) * 1.5f);

            this->bgRect2.w = w;
            this->bgRect.h = h;
            this->bgRect2.x = this->G.x - w / 2.0f;
            this->bgRect.y = this->G.y - h / 2.0f;

            // Knob geometry: thin bar that slides horizontally inside track
            this->bgRect2.h = max(12.0f, this->bgRect.h / 4.0f);
            this->bgRect.w = max(52.0f, this->bgRect2.w + 20.0f);
            this->bgRect.x = this->G.x - this->bgRect.w / 2.0f;
            this->bgRect2.y = this->G.y - this->bgRect2.h / 2.0f;
        }
        else if (this->type == QuadType::SliderV_PosN || this->type == QuadType::SliderV_FreeLimited) {
            float h = 440.0f;
            float w = max(8.0f, static_cast<float>(this->halfDiag1) * 1.5f);

            this->bgRect.w = w;
            this->bgRect2.h = h;
            this->bgRect.x = this->G.x - w / 2.0f;
            this->bgRect2.y = this->G.y - h / 2.0f;

            // Knob geometry: thin bar that slides vertically inside track
            this->bgRect2.w = max(12.0f, this->bgRect.w / 4.0f);
            this->bgRect.h = max(52.0f, this->bgRect2.h + 20.0f);
            this->bgRect2.x = this->G.x - this->bgRect2.w / 2.0f;
            this->bgRect.y = this->G.y - this->bgRect.h / 2.0f;
        }
        else { // 2D sliders: track is area, knob small square centered
            float w = 440.0f;
            float h = w;

            this->bgRect2.w = max(12.0f, w);
            this->bgRect2.h = max(12.0f, h);
            this->bgRect2.x = this->G.x - w / 2.0f;
            this->bgRect2.y = this->G.y - h / 2.0f;

            // Knob geometry: thin bar that slides vertically inside track
            this->bgRect.w = max(52.0f, this->bgRect2.w + 20.0f);
            this->bgRect.h = max(52.0f, this->bgRect2.h + 20.0f);
            this->bgRect.x = this->G.x - this->bgRect.w / 2.0f;
            this->bgRect.y = this->G.y - this->bgRect.h / 2.0f;
        }

        this->hasBackground = true;
        this->hasBgRect2 = true;
    }

public:
    // Constructeur avec validation
    SDL_GQuadr(int id, const string& name, uint16_t halfDiag1 = 50, uint16_t halfDiag2 = 50, double angleDiag1_To2 = SDL_PI_D / 2, double angleFig_OFFSET = 0, double zoomFig = 1, float Gx = 0, float Gy = 0, QuadType quadType = QuadType::Static_Full, uint8_t typeEtat = 0) {
        if (id <= 0) {
            throw invalid_argument("L'ID doit être positif.");
        }
        if (name.empty()) {
            throw invalid_argument("Le nom ne peut pas être vide.");
        }
        this->id = id;
        this->name = name;
        this->type = quadType;
        this->halfDiag1 = halfDiag1;
        this->halfDiag2 = halfDiag2;
        this->angleDiag1_To2 = fmod(angleDiag1_To2, SDL_PI_D);
        this->angleFig_OFFSET = angleFig_OFFSET;
        this->angleFig = angleFig_OFFSET;
        this->zoomFig = zoomFig;
        this->G.x = Gx;
        this->G.y = Gy;
        this->Btn_G.x = this->G.x + this->G_to_Btn_G_OFFSET.x;
        this->Btn_G.y = this->G.y + this->G_to_Btn_G_OFFSET.y;

        this->slider_xVal = this->Btn_G.x;
        this->legend_xVal = legend_xMin_Val + (slider_xVal - slider_xMin) * (legend_xMax_Val - legend_xMin_Val) / (slider_xMax - slider_xMin);
        this->legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);

        this->slider_yVal = this->Btn_G.y;
        this->legend_yVal = legend_yMax_Val - (slider_yVal - slider_yMin) * (legend_yMax_Val - legend_yMin_Val) / (slider_yMax - slider_yMin);
        this->legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);

        this->eventType = typeEtat;
        
        this->angD1_To2_Sup_90 = (angleDiag1_To2 > SDL_PI_D / 2);

        if (this->halfDiag1 > this->halfDiag2) {
            this->HitBoxXYmin.x = this->Btn_G.x - (this->halfDiag1 * this->zoomFig);
            this->HitBoxXYmin.y = this->Btn_G.y - (this->halfDiag1 * this->zoomFig);
            this->HitBoxXYmax.x = this->Btn_G.x + (this->halfDiag1 * this->zoomFig);
            this->HitBoxXYmax.y = this->Btn_G.y + (this->halfDiag1 * this->zoomFig);
        }
        else {
            this->HitBoxXYmin.x = this->Btn_G.x - (this->halfDiag2 * this->zoomFig);
            this->HitBoxXYmin.y = this->Btn_G.y - (this->halfDiag2 * this->zoomFig);
            this->HitBoxXYmax.x = this->Btn_G.x + (this->halfDiag2 * this->zoomFig);
            this->HitBoxXYmax.y = this->Btn_G.y + (this->halfDiag2 * this->zoomFig);
        }

        double pointAngle, pointDist;

        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 3; j++) {
                pointAngle = (j == 1) * this->angleDiag1_To2 + int(0.5 * ((2 * i + j) % 4)) * SDL_PI_D;
                pointDist = (j != 1) * this->halfDiag1 + (j == 1) * this->halfDiag2;
                pointDist = pointDist * this->zoomFig;
                this->pointsTri[i][j].position.x = this->Btn_G.x + pointDist * cos(pointAngle + this->angleFig);
                this->pointsTri[i][j].position.y = this->Btn_G.y + pointDist * sin(pointAngle + this->angleFig);
                this->pointsTri[i][j].color.r = 0.0;
                this->pointsTri[i][j].color.g = 0.0;
                this->pointsTri[i][j].color.b = 0.0;
                this->pointsTri[i][j].color.a = 1.0;

                if (j < 2) {
                    pointsQuadrX[i * 2 + j] = &pointsTri[i][j];
                    pointsQuadrY[i * 2 + j] = &pointsTri[i][j];
                }
            }
        }

        SDL_Vertex* pointsQuadrTemp;

        for (int i = 0; i < 3; i++) {
            for (int j = i + 1; j < 4; j++) {
                if (this->pointsQuadrX[j]->position.x < this->pointsQuadrX[i]->position.x) {
                    pointsQuadrTemp = pointsQuadrX[i];
                    pointsQuadrX[i] = pointsQuadrX[j];
                    pointsQuadrX[j] = pointsQuadrTemp;
                }
                if (this->pointsQuadrY[j]->position.y < this->pointsQuadrY[i]->position.y) {
                    pointsQuadrTemp = pointsQuadrY[i];
                    pointsQuadrY[i] = pointsQuadrY[j];
                    pointsQuadrY[j] = pointsQuadrTemp;
                }
            }
        }

        HitBoxXYmin.x = this->pointsQuadrX[0]->position.x; // HitBoxXYmin
        HitBoxXYmin.y = this->pointsQuadrY[0]->position.y; // HitBoxXYmin
        HitBoxXYmax.x = this->pointsQuadrX[3]->position.x; // HitBoxXYmax
        HitBoxXYmax.y = this->pointsQuadrY[3]->position.y; // HitBoxXYmax

        // centralise le calcul du bgRect et du knob
        updateBackgroundRect();

    }

    // Accesseurs get
    int getId() const { return id; }
    string getName() const { return name; }

    QuadType getType() const { return type; }

    uint16_t getHalfDiag1() { return halfDiag1; }
    uint16_t getHalfDiag2() { return halfDiag2; }
    double getAngleDiag1_To2() { return angleDiag1_To2; }
    double getAngleFig() { return angleFig; }
    double getAngleFig_OFFSET() { return angleFig_OFFSET; }
    double getZoomFig() { return zoomFig; }

    double getZoomFactor() { return zoomFactor; }

    float getGx() { return G.x; }
    float getGy() { return G.y; }

    float getBtn_Gx() { return Btn_G.x; }
    float getBtn_Gy() { return Btn_G.y; }

    float getG_to_Btn_G_OFFSETx() { return G_to_Btn_G_OFFSET.x; }
    float getG_to_Btn_G_OFFSETy() { return G_to_Btn_G_OFFSET.y; }

    float getBgRect2w() { return bgRect2.w; }
    float getBgRect2h() { return bgRect2.h; }

    float getEventType() { return eventType; }

    float getPointPosX(uint8_t triangle, uint8_t point) { return pointsTri[triangle][point].position.x; }
    float getPointPosY(uint8_t triangle, uint8_t point) { return pointsTri[triangle][point].position.y; }
    float getPointColorR(uint8_t triangle, uint8_t point) { return pointsTri[triangle][point].color.r; }
    float getPointColorG(uint8_t triangle, uint8_t point) { return pointsTri[triangle][point].color.g; }
    float getPointColorB(uint8_t triangle, uint8_t point) { return pointsTri[triangle][point].color.b; }

    bool get_isMouseLeftButtonDown() { return isMouseLeftButtonDown; }

    double getLegend_xMin_Val() { return legend_xMin_Val; }
    double getLegend_xMax_Val() { return legend_xMax_Val; }

    double getLegend_yMin_Val() { return legend_yMin_Val; }
    double getLegend_yMax_Val() { return legend_yMax_Val; }

    double getLegend_xVal() { return legend_xVal; }
    double getLegend_yVal() { return legend_yVal; }

    string getLegend_xMin_Text() { return legend_xMin_Text; }
    string getLegend_xMax_Text() { return legend_xMax_Text; }

    string getLegend_yMin_Text() { return legend_yMin_Text; }
    string getLegend_yMax_Text() { return legend_yMax_Text; }

    string getLegend_xText() { return legend_xText; }
    string getLegend_yText() { return legend_yText; }

    QuadUnit getLegendX_Unit() { return legendX_Unit; }
    QuadUnit getLegendY_Unit() { return legendY_Unit; }

    int getNbStepsX() { return nbIntervalsX; }
    int getNbStepsY() { return nbIntervalsY; }

    double getRangeSizeX() { return rangeSizeX; }
    double getRangeSizeY() { return rangeSizeY; }

    QuadButtonEvent getButtonFunction() { return buttonFunction; }

    // Accesseurs set

    void setId(int newId) {
        if (newId <= 0) throw invalid_argument("ID doit être positif.");
        id = newId;
    }
    void setName(const string& newName) {
        if (newName.empty()) throw invalid_argument("Nom vide interdit.");
        name = newName;
    }

    void setType(QuadType newType) {
        using UT = underlying_type_t<QuadType>;
        UT v = static_cast<UT>(newType);
        if (v < static_cast<UT>(QuadType::Static_Full) || v > static_cast<UT>(QuadType::List)) {
            throw invalid_argument("Valeur incorrecte.");
        }
        type = newType;

        // recompute background via helper
        updateBackgroundRect();

        if (type != QuadType::SliderH_PosN && type != QuadType::SliderH_FreeLimited && type != QuadType::Slider2D_FreeLimited && type != QuadType::Slider2D_PosN) {
            slider_xMin = G.x;
            slider_xMax = G.x;
        }
        else {
            slider_xMin = G.x - bgRect2.w / 2 + 20;
            slider_xMax = G.x + bgRect2.w / 2 - 20;
        }

        if (type != QuadType::SliderV_PosN && type != QuadType::SliderV_FreeLimited && type != QuadType::Slider2D_FreeLimited && type != QuadType::Slider2D_PosN) {
            slider_yMin = G.y;
            slider_yMax = G.y;
        }
        else {
            slider_yMin = G.y - bgRect2.h / 2 + 20;
            slider_yMax = G.y + bgRect2.h / 2 - 20;
        }
    }

    void setHalfDiag1(uint16_t length) { halfDiag1 = length; adaptZoomToLongestDiagonal(); if (IsSliderType(type)) updateBackgroundRect(); }
    void setHalfDiag2(uint16_t length) { halfDiag2 = length; adaptZoomToLongestDiagonal(); if (IsSliderType(type)) updateBackgroundRect(); }

    void setAngleDiag1_To2(double angle = 0.0) { angleDiag1_To2 = angle; }
    void setAngleFig(double angle = 0.0) { angleFig = angle; }
    void setAngleFig_OFFSET(double angle = 0.0) { angleFig_OFFSET = angle; }
    void setZoomFig(double zoom = 1.0) { zoomFig = zoom; }

    void setZoomFactor(double zoom = 1.15) { zoomFactor = zoom; }

    void adaptZoomToLongestDiagonal() {
        double longestDiag = static_cast<double>(max(halfDiag1, halfDiag2));
        if (longestDiag > 110) {
            zoomFactor = 1 + 0.15 * pow(110.0f / longestDiag, 1.4);
        }
        else {
            zoomFactor = 1.15;
        }
    }

    void setGx(uint16_t posX = 0) {
        G.x = posX;
        Btn_G.x = G.x + G_to_Btn_G_OFFSET.x;
        if (IsSliderType(type)) updateBackgroundRect();

        if (type != QuadType::SliderH_PosN && type != QuadType::SliderH_FreeLimited && type != QuadType::Slider2D_FreeLimited && type != QuadType::Slider2D_PosN) {
            slider_xMin = G.x;
            slider_xMax = G.x;
        }
        else {
            slider_xMin = G.x - bgRect2.w / 2 + 20;
            slider_xMax = G.x + bgRect2.w / 2 - 20;
        }

    }
    void setGy(uint16_t posY = 0) {
        G.y = posY;
        Btn_G.y = G.y + G_to_Btn_G_OFFSET.y;
        if (IsSliderType(type)) updateBackgroundRect();

        if (type != QuadType::SliderV_PosN && type != QuadType::SliderV_FreeLimited && type != QuadType::Slider2D_FreeLimited && type != QuadType::Slider2D_PosN) {
            slider_yMin = G.y;
            slider_yMax = G.y;
        }
        else {
            slider_yMin = G.y - bgRect2.h / 2 + 20;
            slider_yMax = G.y + bgRect2.h / 2 - 20;
        }
        
    }

    void setBtn_Gx(uint16_t posX = 0) { Btn_G.x = posX; G_to_Btn_G_OFFSET.x = Btn_G.x - G.x; }
    void setBtn_Gy(uint16_t posY = 0) { Btn_G.y = posY; G_to_Btn_G_OFFSET.y = Btn_G.y - G.y; }

    void setG_to_Btn_G_OFFSETx(uint16_t posX = 0) {
        G_to_Btn_G_OFFSET.x = posX; Btn_G.x = G.x + G_to_Btn_G_OFFSET.x;
    }
    void setG_to_Btn_G_OFFSETy(uint16_t posY = 0) {
        G_to_Btn_G_OFFSET.y = posY; Btn_G.y = G.y + G_to_Btn_G_OFFSET.y;
    }

    void setEventType(uint8_t typeEvenement) { eventType = typeEvenement; }

    void setPointColorRGB(uint8_t triangle, uint8_t point, float color_R = 0.0, float color_G = 0.0, float color_B = 0.0) {
        pointsTri[triangle][point].color.r = color_R;
        pointsTri[triangle][point].color.g = color_G;
        pointsTri[triangle][point].color.b = color_B;
    }

    void setTriangleColorRGB(uint8_t triangle, float color_R = 0.0, float color_G = 0.0, float color_B = 0.0) {
        for (int j = 0; j < 3; j++) {
            pointsTri[triangle][j].color.r = color_R;
            pointsTri[triangle][j].color.g = color_G;
            pointsTri[triangle][j].color.b = color_B;
        }
    }

    void setQuadrColorRGB(float color_R = 0.0, float color_G = 0.0, float color_B = 0.0) {
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 3; j++) {
                pointsTri[i][j].color.r = color_R;
                pointsTri[i][j].color.g = color_G;
                pointsTri[i][j].color.b = color_B;
            }
        }
    }

    void quadrColorRGB_ClickUpdate() {
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 3; j++) {
                pointsTri[i][j].color.r = pointsTri[i][j].color.r + (0.5 - (isMouseLeftButtonDown == true)) * 2 * mouseLeftButtonClick_Color_OFFSET;
                pointsTri[i][j].color.g = pointsTri[i][j].color.g + (0.5 - (isMouseLeftButtonDown == true)) * 2 * mouseLeftButtonClick_Color_OFFSET;
                pointsTri[i][j].color.b = pointsTri[i][j].color.b + (0.5 - (isMouseLeftButtonDown == true)) * 2 * mouseLeftButtonClick_Color_OFFSET;
            }
        }
    }

    // setter/getter pour la couleur du bgRect (valeurs 0.0f..1.0f)
    void setBgColor(float r, float g, float b, float a = 1.0f) noexcept {
        auto clamp01 = [](float v) -> float { if (v < 0.0f) return 0.0f; if (v > 1.0f) return 1.0f; return v; };
        bg_r = clamp01(r);
        bg_g = clamp01(g);
        bg_b = clamp01(b);
        bg_a = clamp01(a);
    }

    void getBgColor(float& r, float& g, float& b, float& a) const noexcept {
        r = bg_r; g = bg_g; b = bg_b; a = bg_a;
    }

    // setter/getter pour la couleur du knob (bgRect2)
    void setBgKnobColor(float r, float g, float b, float a = 1.0f) noexcept {
        auto clamp01 = [](float v) -> float { if (v < 0.0f) return 0.0f; if (v > 1.0f) return 1.0f; return v; };
        slider_r = clamp01(r);
        slider_g = clamp01(g);
        slider_b = clamp01(b);
        slider_a = clamp01(a);
    }

    void getBgKnobColor(float& r, float& g, float& b, float& a) const noexcept {
        r = slider_r; g = slider_g; b = slider_b; a = slider_a;
    }

    void set_isMouseLeftButtonDown(bool MouseLeftButtonDown_state) { isMouseLeftButtonDown = MouseLeftButtonDown_state; }



    // Slider ranges / values setters/getters
    void setSlider_xMin(double val) noexcept { slider_xMin = val; }
    void setSlider_xMax(double val) noexcept { slider_xMax = val; }

    void setSlider_yMin(double val) noexcept { slider_yMin = val; }
    void setSlider_yMax(double val) noexcept { slider_yMax = val; }

    void setSlider_xVal(double val) noexcept {
        slider_xVal = val;
        legend_xVal = legend_xMin_Val + (slider_xVal - slider_xMin) * (legend_xMax_Val - legend_xMin_Val) / (slider_xMax - slider_xMin);
        legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);
    }

    void setSlider_yVal(double val) noexcept {
        slider_yVal = val;
        legend_yVal = legend_yMax_Val - (slider_yVal - slider_yMin) * (legend_yMax_Val - legend_yMin_Val) / (slider_yMax - slider_yMin);
        legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
    }

    double getSlider_xMin() const noexcept { return slider_xMin; }
    double getSlider_xMax() const noexcept { return slider_xMax; }

    double getSlider_yMin() const noexcept { return slider_yMin; }
    double getSlider_yMax() const noexcept { return slider_yMax; }

    double getSlider_xVal() const noexcept { return slider_xVal; }
    double getSlider_yVal() const noexcept { return slider_yVal; }

    void setLegend_xMin_Val(double xMinLegend) {
        legend_xMin_Val = xMinLegend;
        slider_xVal = Btn_G.x;
        legend_xVal = legend_xMin_Val + (slider_xVal - slider_xMin) * (legend_xMax_Val - legend_xMin_Val) / (slider_xMax - slider_xMin);
        legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);
    }
    void setLegend_xMax_Val(double xMaxLegend) {
        legend_xMax_Val = xMaxLegend;
        slider_xVal = Btn_G.x;
        legend_xVal = legend_xMin_Val + (slider_xVal - slider_xMin) * (legend_xMax_Val - legend_xMin_Val) / (slider_xMax - slider_xMin);
        legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);
    }

    void setLegend_yMin_Val(double yMinLegend) {
        legend_yMin_Val = yMinLegend;
        slider_yVal = Btn_G.y;
        legend_yVal = legend_yMax_Val - (slider_yVal - slider_yMin) * (legend_yMax_Val - legend_yMin_Val) / (slider_yMax - slider_yMin);
        legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
    }
    void setLegend_yMax_Val(float yMaxLegend) {
        legend_yMax_Val = yMaxLegend;
        slider_yVal = Btn_G.y;
        legend_yVal = legend_yMax_Val - (slider_yVal - slider_yMin) * (legend_yMax_Val - legend_yMin_Val) / (slider_yMax - slider_yMin);
        legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
    }

    void setLegend_xVal(double xLegend) {
        legend_xVal = xLegend;
        legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);
    }
    void setLegend_yVal(double yLegend) {
        legend_yVal = yLegend;
        legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
    }

    void setLegend_xMin_Text(string xMinLegendStr) { legend_xMin_Text = xMinLegendStr; }
    void setLegend_xMax_Text(string xMaxLegendStr) { legend_xMax_Text = xMaxLegendStr; }

    void setLegend_yMin_Text(string yMinLegendStr) { legend_yMin_Text = yMinLegendStr; }
    void setLegend_yMax_Text(string yMaxLegendStr) { legend_yMax_Text = yMaxLegendStr; }

    void setLegend_xText(string xLegendStr) { legend_xText = xLegendStr; }
    void setLegend_yText(string yLegendStr) { legend_yText = yLegendStr; }

    void setLegendX_Unit(QuadUnit unitX) {
        legendX_Unit = unitX;
        slider_xVal = Btn_G.x;
        legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);
    }

    void setLegendY_Unit(QuadUnit unitY) {
        legendY_Unit = unitY;
        slider_yVal = Btn_G.y;
        legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
    }

    void setNbIntervalsX(int nIntervals) {
        nbIntervalsX = nIntervals;
        rangeSizeX = (slider_xMax - slider_xMin) / static_cast<double>(nbIntervalsX);
    }
    void setNbIntervalsY(int nIntervals) {
        nbIntervalsY = nIntervals;
        rangeSizeY = (slider_yMax - slider_yMin) / static_cast<double>(nbIntervalsY);
    }

    void setRangeSizeX(double rngSize) { rangeSizeX = rngSize; }
    void setRangeSizeY(double rngSize) { rangeSizeY = rngSize; }

    // TextBox API
    void setText(const string& t) noexcept { textboxText = t; }
    void setTextColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept { textboxColor = { r, g, b, a }; }
    string getText() const noexcept { return textboxText; }
    SDL_Color getTextColor() const noexcept { return textboxColor; }

    // (Other) Main functions

    void setButtonFunction(QuadButtonEvent newEvent) { buttonFunction = newEvent; }

    void setRectangleShape(uint16_t halfWidth, uint16_t halfHeight) noexcept {
        // convert to double for math
        const double w = static_cast<double>(halfWidth);
        const double h = static_cast<double>(halfHeight);

        // distance centre -> coin
        double cornerDist = hypot(w, h); // sqrt(w*w + h*h)

        // clamp helper
        auto clampToUint16 = [](double v) -> uint16_t {
            if (v <= 0.0) return 0;
            if (v >= static_cast<double>(numeric_limits<uint16_t>::max()))
                return numeric_limits<uint16_t>::max();
            return static_cast<uint16_t>(lround(v));
            };

        uint16_t diag = clampToUint16(cornerDist);

        // set radii (equal)
        this->halfDiag1 = diag;
        this->halfDiag2 = diag;

        // angle of the corner vector (direction to corner (w,h))
        double cornerAngle = 0.0;
        if (w != 0.0 || h != 0.0) cornerAngle = atan2(h, w);

        // compute angle between the two diagonal directions used by the mesh
        // Derived from the two corner directions (atan2(h,w) and atan2(h,-w) = pi - atan2(h,w))
        // angleDiag1_To2 = (pi - 2*atan2(h,w)) in [0, pi]
        double angleDiag = SDL_PI_D - 2.0 * cornerAngle;
        // Normalize into [0, SDL_PI_D)
        angleDiag = fmod(angleDiag + SDL_PI_D, SDL_PI_D);

        this->angleDiag1_To2 = angleDiag;
        this->angleFig_OFFSET = cornerAngle;
        this->angD1_To2_Sup_90 = (this->angleDiag1_To2 > (SDL_PI_D / 2.0));

        adaptZoomToLongestDiagonal();

        // Update dependent geometry
        if (IsSliderType(this->type)) updateBackgroundRect();
        (void)this->updatePos();

        // Pour les boutons, la hitbox doit correspondre exactement au rectangle visuel
        if (this->type == QuadType::Button_StateMono || this->type == QuadType::TextBox) {
            // Calculer la hitbox basée sur le rectangle RÉEL (pas sur les diagonales)
            float rectWidth = 2.0f * halfWidth;
            float rectHeight = 2.0f * halfHeight;

            HitBoxXYmin.x = this->Btn_G.x - rectWidth / 2.0f;
            HitBoxXYmin.y = this->Btn_G.y - rectHeight / 2.0f;
            HitBoxXYmax.x = this->Btn_G.x + rectWidth / 2.0f;
            HitBoxXYmax.y = this->Btn_G.y + rectHeight / 2.0f;

            SDL_Log("Bouton '%s' - HitBox recalculée: (%.0f, %.0f) -> (%.0f, %.0f)",
                this->name.c_str(),
                HitBoxXYmin.x, HitBoxXYmin.y,
                HitBoxXYmax.x, HitBoxXYmax.y);
        }
        else {
            // Pour les autres types, utiliser la méthode existante
            legend_xVal = legend_xMin_Val + (slider_xVal - slider_xMin) * (legend_xMax_Val - legend_xMin_Val) / (slider_xMax - slider_xMin);
            legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);

            legend_yVal = legend_yMax_Val - (slider_yVal - slider_yMin) * (legend_yMax_Val - legend_yMin_Val) / (slider_yMax - slider_yMin);
            legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
        }
    }

    bool updatePos() {
        double pointAngle, pointDist;

        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 3; j++) {
                pointAngle = (j == 1) * angleDiag1_To2 + int(0.5 * ((2 * i + j) % 4)) * SDL_PI_D;
                pointDist = (j != 1) * halfDiag1 + (j == 1) * halfDiag2;
                pointDist = pointDist * zoomFig;
                pointsTri[i][j].position.x = Btn_G.x + pointDist * cos(pointAngle + angleFig + angleFig_OFFSET);
                pointsTri[i][j].position.y = Btn_G.y + pointDist * sin(pointAngle + angleFig + angleFig_OFFSET);
            }
        }

        if (this->type != QuadType::Static_Full) {
            SDL_Vertex* pointsQuadrTemp;

            if ((this->pointsQuadrX[0]->position.x > this->pointsQuadrX[1]->position.x) or (this->pointsQuadrX[0]->position.x > this->pointsQuadrX[2]->position.x) or (this->pointsQuadrX[1]->position.x > this->pointsQuadrX[2]->position.x)) {
                for (int i = 0; i < 3; i++) {
                    for (int j = i + 1; j < 4; j++) {
                        if (this->pointsQuadrX[j]->position.x < this->pointsQuadrX[i]->position.x) {
                            pointsQuadrTemp = pointsQuadrX[i];
                            pointsQuadrX[i] = pointsQuadrX[j];
                            pointsQuadrX[j] = pointsQuadrTemp;
                        }
                    }
                }
            }

            if ((this->pointsQuadrY[0]->position.y > this->pointsQuadrY[1]->position.y) or (this->pointsQuadrY[0]->position.y > this->pointsQuadrY[2]->position.y) or (this->pointsQuadrY[1]->position.y > this->pointsQuadrY[2]->position.y)) {
                for (int i = 0; i < 3; i++) {
                    for (int j = i + 1; j < 4; j++) {
                        if (this->pointsQuadrY[j]->position.y < this->pointsQuadrY[i]->position.y) {
                            pointsQuadrTemp = pointsQuadrY[i];
                            pointsQuadrY[i] = pointsQuadrY[j];
                            pointsQuadrY[j] = pointsQuadrTemp;
                        }
                    }
                }
            }

            HitBoxXYmin.x = this->pointsQuadrX[0]->position.x; // HitBoxXYmin
            HitBoxXYmin.y = this->pointsQuadrY[0]->position.y; // HitBoxXYmin
            HitBoxXYmax.x = this->pointsQuadrX[3]->position.x; // HitBoxXYmax
            HitBoxXYmax.y = this->pointsQuadrY[3]->position.y; // HitBoxXYmax

            if (mx >= HitBoxXYmin.x && mx <= HitBoxXYmax.x && my >= HitBoxXYmin.y && my <= HitBoxXYmax.y) {
                // Mouse is inside the hitbox
                if ((this->pointsQuadrX[0]->position.x == this->pointsQuadrX[1]->position.x and this->pointsQuadrX[0]->position.y == this->pointsQuadrX[2]->position.y) or (this->pointsQuadrX[0]->position.x == this->pointsQuadrX[2]->position.x and this->pointsQuadrX[0]->position.y == this->pointsQuadrX[1]->position.y)) {
                    // First test's action
                    if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                }
                else if (this->pointsQuadrX[0]->position.x == this->pointsQuadrX[3]->position.x or this->pointsQuadrX[1]->position.x == this->pointsQuadrX[2]->position.x) {
                    // Second test's action

                    int lowBoundY_at_mx, upBoundY_at_mx;
                    double lowSlope, upSlope;

                    if (mx >= this->pointsQuadrX[0]->position.x and mx <= this->pointsQuadrX[1]->position.x) {

                        if (this->pointsQuadrX[1]->position.y <= this->Btn_G.y) {
                            lowSlope = (this->pointsQuadrX[1]->position.y - this->pointsQuadrX[0]->position.y) / (this->pointsQuadrX[1]->position.x - this->pointsQuadrX[0]->position.x);
                            lowBoundY_at_mx = this->pointsQuadrX[0]->position.y + lowSlope * (mx - this->pointsQuadrX[0]->position.x);
                            upSlope = (this->pointsQuadrX[2]->position.y - this->pointsQuadrX[0]->position.y) / (this->pointsQuadrX[2]->position.x - this->pointsQuadrX[0]->position.x);
                            upBoundY_at_mx = this->pointsQuadrX[0]->position.y + upSlope * (mx - this->pointsQuadrX[0]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                                return false;
                            }
                        }
                        else {
                            lowSlope = (this->pointsQuadrX[2]->position.y - this->pointsQuadrX[0]->position.y) / (this->pointsQuadrX[2]->position.x - this->pointsQuadrX[0]->position.x);
                            lowBoundY_at_mx = this->pointsQuadrX[0]->position.y + lowSlope * (mx - this->pointsQuadrX[0]->position.x);
                            upSlope = (this->pointsQuadrX[1]->position.y - this->pointsQuadrX[0]->position.y) / (this->pointsQuadrX[1]->position.x - this->pointsQuadrX[0]->position.x);
                            upBoundY_at_mx = this->pointsQuadrX[0]->position.y + upSlope * (mx - this->pointsQuadrX[0]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                                return false;
                            }
                        }
                    }
                    else {
                        if (this->pointsQuadrX[1]->position.y <= this->Btn_G.y) {
                            lowSlope = (this->pointsQuadrX[3]->position.y - this->pointsQuadrX[1]->position.y) / (this->pointsQuadrX[3]->position.x - this->pointsQuadrX[1]->position.x);
                            lowBoundY_at_mx = this->pointsQuadrX[1]->position.y + lowSlope * (mx - this->pointsQuadrX[1]->position.x);
                            upSlope = (this->pointsQuadrX[3]->position.y - this->pointsQuadrX[2]->position.y) / (this->pointsQuadrX[3]->position.x - this->pointsQuadrX[2]->position.x);
                            upBoundY_at_mx = this->pointsQuadrX[2]->position.y + upSlope * (mx - this->pointsQuadrX[2]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                                return false;
                            }
                        }
                        else {
                            lowSlope = (this->pointsQuadrX[3]->position.y - this->pointsQuadrX[2]->position.y) / (this->pointsQuadrX[3]->position.x - this->pointsQuadrX[2]->position.x);
                            lowBoundY_at_mx = this->pointsQuadrX[2]->position.y + lowSlope * (mx - this->pointsQuadrX[2]->position.x);
                            upSlope = (this->pointsQuadrX[3]->position.y - this->pointsQuadrX[1]->position.y) / (this->pointsQuadrX[3]->position.x - this->pointsQuadrX[1]->position.x);
                            upBoundY_at_mx = this->pointsQuadrX[1]->position.y + upSlope * (mx - this->pointsQuadrX[1]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                                return false;
                            }
                        }
                    }
                }
                else {
                    // Default action
                    int lowBoundY_at_mx, upBoundY_at_mx;
                    double lowSlope, upSlope;

                    if (mx >= this->pointsQuadrX[0]->position.x and mx <= this->pointsQuadrX[1]->position.x) {
                        if (this->pointsQuadrX[1]->position.y <= this->Btn_G.y) {
                            lowSlope = (this->pointsQuadrX[1]->position.y - this->pointsQuadrX[0]->position.y) / (this->pointsQuadrX[1]->position.x - this->pointsQuadrX[0]->position.x);
                            lowBoundY_at_mx = this->pointsQuadrX[0]->position.y + lowSlope * (mx - this->pointsQuadrX[0]->position.x);
                            upSlope = (this->pointsQuadrX[2]->position.y - this->pointsQuadrX[0]->position.y) / (this->pointsQuadrX[2]->position.x - this->pointsQuadrX[0]->position.x);
                            upBoundY_at_mx = this->pointsQuadrX[0]->position.y + upSlope * (mx - this->pointsQuadrX[0]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                                return false;
                            }
                        }
                        else {
                            lowSlope = (this->pointsQuadrX[2]->position.y - this->pointsQuadrX[0]->position.y) / (this->pointsQuadrX[2]->position.x - this->pointsQuadrX[0]->position.x);
                            lowBoundY_at_mx = this->pointsQuadrX[0]->position.y + lowSlope * (mx - this->pointsQuadrX[0]->position.x);
                            upSlope = (this->pointsQuadrX[1]->position.y - this->pointsQuadrX[0]->position.y) / (this->pointsQuadrX[1]->position.x - this->pointsQuadrX[0]->position.x);
                            upBoundY_at_mx = this->pointsQuadrX[0]->position.y + upSlope * (mx - this->pointsQuadrX[0]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                                return false;
                            }
                        }
                    }
                    else if (mx >= this->pointsQuadrX[1]->position.x and mx <= this->pointsQuadrX[2]->position.x) {
                        if (this->pointsQuadrX[1]->position.y <= this->Btn_G.y) {
                            lowSlope = (this->pointsQuadrX[3]->position.y - this->pointsQuadrX[1]->position.y) / (this->pointsQuadrX[3]->position.x - this->pointsQuadrX[1]->position.x);
                            lowBoundY_at_mx = this->pointsQuadrX[1]->position.y + lowSlope * (mx - this->pointsQuadrX[1]->position.x);
                            upSlope = (this->pointsQuadrX[2]->position.y - this->pointsQuadrX[0]->position.y) / (this->pointsQuadrX[2]->position.x - this->pointsQuadrX[0]->position.x);
                            upBoundY_at_mx = this->pointsQuadrX[0]->position.y + upSlope * (mx - this->pointsQuadrX[0]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                                return false;
                            }
                        }
                        else {
                            lowSlope = (this->pointsQuadrX[2]->position.y - this->pointsQuadrX[0]->position.y) / (this->pointsQuadrX[2]->position.x - this->pointsQuadrX[0]->position.x);
                            lowBoundY_at_mx = this->pointsQuadrX[0]->position.y + lowSlope * (mx - this->pointsQuadrX[0]->position.x);
                            upSlope = (this->pointsQuadrX[3]->position.y - this->pointsQuadrX[1]->position.y) / (this->pointsQuadrX[3]->position.x - this->pointsQuadrX[1]->position.x);
                            upBoundY_at_mx = this->pointsQuadrX[1]->position.y + upSlope * (mx - this->pointsQuadrX[1]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                                return false;
                            }
                        }
                    }
                    else {
                        if (this->pointsQuadrX[1]->position.y <= this->Btn_G.y) {
                            lowSlope = (this->pointsQuadrX[3]->position.y - this->pointsQuadrX[1]->position.y) / (this->pointsQuadrX[3]->position.x - this->pointsQuadrX[1]->position.x);
                            lowBoundY_at_mx = this->pointsQuadrX[1]->position.y + lowSlope * (mx - this->pointsQuadrX[1]->position.x);
                            upSlope = (this->pointsQuadrX[3]->position.y - this->pointsQuadrX[2]->position.y) / (this->pointsQuadrX[3]->position.x - this->pointsQuadrX[2]->position.x);
                            upBoundY_at_mx = this->pointsQuadrX[2]->position.y + upSlope * (mx - this->pointsQuadrX[2]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                                return false;
                            }
                        }
                        else {
                            lowSlope = (this->pointsQuadrX[3]->position.y - this->pointsQuadrX[2]->position.y) / (this->pointsQuadrX[3]->position.x - this->pointsQuadrX[2]->position.x);
                            lowBoundY_at_mx = this->pointsQuadrX[2]->position.y + lowSlope * (mx - this->pointsQuadrX[2]->position.x);
                            upSlope = (this->pointsQuadrX[3]->position.y - this->pointsQuadrX[1]->position.y) / (this->pointsQuadrX[3]->position.x - this->pointsQuadrX[1]->position.x);
                            upBoundY_at_mx = this->pointsQuadrX[1]->position.y + upSlope * (mx - this->pointsQuadrX[1]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->zoomFig != this->zoomFactor) { this->zoomFig = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                                return false;
                            }
                        }
                    }
                }
            }
            else {
                // Mouse is outside the hitbox
                if (this->zoomFig != 1.0) { this->zoomFig = 1.0; return false; }
                return false;
            }
        }
        else {
            return false;
        }

    }

    void renderFig() {
        // draw slider background first if present
        if (hasBackground && renderer) {
            // Track (configurable color)
            SDL_SetRenderDrawColorFloat(renderer, bg_r, bg_g, bg_b, bg_a);
            SDL_RenderFillRect(renderer, &bgRect);

            // subtle border for track
            SDL_SetRenderDrawColorFloat(renderer, 0.6f, 0.6f, 0.6f, 1.0f);
            SDL_RenderRect(renderer, &bgRect);

            // Knob in front (black by default or configurable)
            if (hasBgRect2) {
                SDL_SetRenderDrawColorFloat(renderer, slider_r, slider_g, slider_b, slider_a);
                SDL_RenderFillRect(renderer, &bgRect2);

                // optional light border to separate knob from track
                SDL_SetRenderDrawColorFloat(renderer, 0.85f, 0.85f, 0.85f, 0.6f);
                SDL_RenderRect(renderer, &bgRect2);
            }

            // Render labels at ends of bgRect for Slider types using SDL_ttf when available
            if (IsSliderType(type) && gFont) {
                SDL_Color textColor = { 0, 0, 0, 255 };
                float tw = 0.0f, th = 0.0f;
                if (type == QuadType::SliderH_PosN || type == QuadType::SliderH_FreeLimited) {
                    getSurfaceSize(getLegend_xMin_Text(), tw, th);
                    RenderText(getLegend_xMin_Text(), bgRect.x - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);
                    getSurfaceSize(getLegend_xMax_Text(), tw, th);
                    RenderText(getLegend_xMax_Text(), bgRect.x + bgRect.w - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);
                }
                else if (type == QuadType::SliderV_PosN || type == QuadType::SliderV_FreeLimited) {
                    getSurfaceSize(getLegend_yMax_Text(), tw, th);
                    RenderText(getLegend_yMax_Text(), G.x - tw / 2.0f, bgRect.y - th - 8.0f, textColor, tw, th);
                    getSurfaceSize(getLegend_yMin_Text(), tw, th);
                    RenderText(getLegend_yMin_Text(), G.x - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);
                }
                else { // 2D sliders: top-right and bottom-left
                    getSurfaceSize(getLegend_xMin_Text(), tw, th);
                    RenderText(getLegend_xMin_Text(), bgRect.x - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);
                    getSurfaceSize(getLegend_xMax_Text(), tw, th);
                    RenderText(getLegend_xMax_Text(), bgRect.x + bgRect.w - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);

                    getSurfaceSize(getLegend_yMax_Text(), tw, th);
                    RenderText(getLegend_yMax_Text(), G.x - tw / 2.0f, bgRect.y - th - 8.0f, textColor, tw, th);
                    getSurfaceSize(getLegend_yMin_Text(), tw, th);
                    RenderText(getLegend_yMin_Text(), G.x - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);
                }
            }
        }

        for (int i = 0; i < 2; i++) {
            SDL_RenderGeometry(renderer, nullptr, pointsTri[i], vertexTri, nullptr, 0);
        }

        // Render centered text for TextBox type (scaled by zoomFig)
        if ((type == QuadType::TextBox || type == QuadType::Button_StateMono) && gFont && !textboxText.empty()) {
            float centerX = (HitBoxXYmin.x + HitBoxXYmax.x) * 0.5f;
            float centerY = (HitBoxXYmin.y + HitBoxXYmax.y) * 0.5f;

            double scale = static_cast<double>(this->zoomFig);
            float tw = 0.0f, th = 0.0f;
            getSurfaceSize(textboxText, tw, th, scale);
            // If getSurfaceSize failed (0,0), fallback to RenderText measure
            if (tw <= 0.0f || th <= 0.0f) {
                RenderText(textboxText, centerX, centerY, textboxColor, tw, th, scale);
            }
            float px = centerX - tw / 2.0f;
            float py = centerY - th / 2.0f;
            RenderText(textboxText, px, py, textboxColor, tw, th, scale);
        }
    }

    // Affichage
    void display() const {
        cout << "MyObject[ID=" << id << ", Name=" << name << "]\n";
    }
};

// ============================================================================
// CLASSE SDL_GQuadrs
// ============================================================================

class SDL_GQuadrs {
    list<SDL_GQuadr> myQuards;
public:
    using iterator = list<SDL_GQuadr>::iterator;
    using const_iterator = list<SDL_GQuadr>::const_iterator;

    // Ajout avec nom uniquement -> ID = taille actuelle + 1
    void add(const string& name) {
        myQuards.emplace_back(myQuards.size() + 1, name);
    }

    // Suppression avec remplacement par le dernier et réajustement d'ID
    void remove(const string& name) {
        if (myQuards.empty()) {
            cout << "\nAucun objet à supprimer.\n";
            return;
        }
        auto it = myQuards.begin();
        while (it != myQuards.end()) {
            if (it->getName() == name) {
                auto lastIt = prev(myQuards.end());
                if (it != lastIt) {
                    int oldId = it->getId();
                    *it = *lastIt;
                    it->setId(oldId);
                }
                myQuards.erase(lastIt);
                return;
            }
            ++it;
        }
        cout << "\nObjet '" << name << "' introuvable.\n";
    }

    // Accès direct à un objet par nom
    SDL_GQuadr& operator()(const string& name) {
        for (auto& myQuard : myQuards) {
            if (myQuard.getName() == name) return myQuard;
        }
        throw out_of_range("Aucun objet trouvé avec le nom \"" + name + "\"");
    }

    // Accès direct à un objet par ID
    SDL_GQuadr& operator()(int id) {
        for (auto& myQuard : myQuards) {
            if (myQuard.getId() == id) return myQuard;
        }
        throw out_of_range("Aucun objet trouvé avec l'ID " + to_string(id));
    }

    // Taille de la collection
    size_t size() const noexcept {
        return myQuards.size();
    }

    // Itérateurs pour range-for et itération manuelle
    iterator begin() noexcept { return myQuards.begin(); }
    iterator end() noexcept { return myQuards.end(); }
    const_iterator begin() const noexcept { return myQuards.begin(); }
    const_iterator end() const noexcept { return myQuards.end(); }
    const_iterator cbegin() const noexcept { return myQuards.cbegin(); }
    const_iterator cend() const noexcept { return myQuards.cend(); }

    // Affichage de tous les objets
    void displayAll() const {
        if (myQuards.empty()) {
            cout << "Aucun objet dans la collection.\n";
            return;
        }
        for (const auto& myQuard : myQuards) myQuard.display();
    }
};

// ============================================================================
// VARIABLES GLOBALES POUR LES QUADRILATÈRES
// ============================================================================

SDL_GQuadr* mousePtrOnQuadr = nullptr;
SDL_GQuadr* mClickL_PtrOnQuadr = nullptr;
SDL_GQuadrs Quadrs;  // Suppression de l'initialisation par défaut



// === FORMANT SHIFTER TEMPS RÉEL ===
struct RealtimeFormantShifter {
    static constexpr size_t FRAME_SIZE = 512;   // Fenêtre plus courte = meilleure résolution temporelle
    static constexpr size_t HOP_SIZE = 128;     // 75% overlap

    vector<float> inputBuffer;
    vector<float> outputBuffer;
    vector<float> overlapBuffer;
    vector<float> window;

    size_t samplesInBuffer = 0;

    RealtimeFormantShifter() {
        inputBuffer.resize(FRAME_SIZE, 0.0f);
        outputBuffer.resize(FRAME_SIZE, 0.0f);
        overlapBuffer.resize(FRAME_SIZE, 0.0f);

        // Fenêtre de Hann
        window.resize(FRAME_SIZE);
        for (size_t i = 0; i < FRAME_SIZE; ++i) {
            window[i] = 0.5f * (1.0f - cos(2.0f * 3.14159265f * i / (FRAME_SIZE - 1)));
        }

        SDL_Log("=== Formant Shifter FINAL CORRIGÉ ===");
        SDL_Log("  Frame: %zu samples (%.1f ms)", FRAME_SIZE, (FRAME_SIZE * 1000.0f) / 16000.0f);
        SDL_Log("  Hop: %zu samples (75%% overlap)", HOP_SIZE);
        SDL_Log("  Méthode: Décalage spectral avec fenêtre courte + OLA correct");
    }

    void ProcessFrame(const float* input, float* output, size_t numSamples, float shiftRatio) {
        // Remplir buffer
        for (size_t i = 0; i < numSamples; ++i) {
            if (samplesInBuffer < FRAME_SIZE) {
                inputBuffer[samplesInBuffer++] = input[i];
            }
        }

        if (samplesInBuffer < FRAME_SIZE) {
            for (size_t i = 0; i < numSamples; ++i) {
                output[i] = input[i];
            }
            return;
        }

        // === FENÊTRAGE ===
        vector<float> windowedFrame(FRAME_SIZE);
        for (size_t i = 0; i < FRAME_SIZE; ++i) {
            windowedFrame[i] = inputBuffer[i] * window[i];
        }

        // === FFT ===
        vector<complex<float>> spectrum(FRAME_SIZE);
        for (size_t i = 0; i < FRAME_SIZE; ++i) {
            spectrum[i] = complex<float>(windowedFrame[i], 0.0f);
        }

        FFT_CooleyTukey(spectrum);

        // === DÉCALAGE SPECTRAL PAR REMPLACEMENT (PAS ACCUMULATION) ===
        vector<complex<float>> shiftedSpectrum(FRAME_SIZE, complex<float>(0.0f, 0.0f));

        for (size_t bin = 0; bin <= FRAME_SIZE / 2; ++bin) {
            float targetBinFloat = static_cast<float>(bin) * shiftRatio;

            if (targetBinFloat > FRAME_SIZE / 2) continue;

            size_t targetBinLow = static_cast<size_t>(floor(targetBinFloat));
            size_t targetBinHigh = min(targetBinLow + 1, FRAME_SIZE / 2);
            float frac = targetBinFloat - targetBinLow;

            complex<float> value = spectrum[bin];

            if (frac < 0.5f) {
                shiftedSpectrum[targetBinLow] = value;
            }
            else {
                if (targetBinHigh <= FRAME_SIZE / 2) {
                    shiftedSpectrum[targetBinHigh] = value;
                }
            }
        }

        shiftedSpectrum[0] = spectrum[0];

        // === SYMÉTRIE HERMITIENNE ===
        for (size_t i = 1; i < FRAME_SIZE / 2; ++i) {
            shiftedSpectrum[FRAME_SIZE - i] = conj(shiftedSpectrum[i]);
        }

        // === IFFT ===
        for (auto& c : shiftedSpectrum) c = conj(c);
        FFT_CooleyTukey(shiftedSpectrum);

        float invFFTSize = 1.0f / FRAME_SIZE;

        // === OVERLAP-ADD AVEC FACTEUR CORRECT POUR FORMANT SHIFT ===
        // Pour 75% overlap, théorie = 2/3, mais en pratique = 1/3 pour formant shift
        constexpr float olaFactor = 0.3333f;

        for (size_t i = 0; i < FRAME_SIZE; ++i) {
            float synthSample = conj(shiftedSpectrum[i]).real() * invFFTSize;

            // Appliquer fenêtre + facteur OLA
            outputBuffer[i] = overlapBuffer[i] + synthSample * window[i] * olaFactor;
        }

        // Copier sortie
        for (size_t i = 0; i < numSamples; ++i) {
            output[i] = outputBuffer[i];
        }

        // Décaler buffers
        copy(outputBuffer.begin() + HOP_SIZE, outputBuffer.end(), overlapBuffer.begin());
        fill(overlapBuffer.begin() + (FRAME_SIZE - HOP_SIZE), overlapBuffer.end(), 0.0f);

        copy(inputBuffer.begin() + HOP_SIZE, inputBuffer.end(), inputBuffer.begin());
        samplesInBuffer -= HOP_SIZE;
    }
};

// === INSTANCE GLOBALE ===
static RealtimeFormantShifter* g_formantShifter = nullptr;

// === FONCTION D'INTERFACE ===
void ApplyRealtimeFormantShift(vector<Uint8>& audioData, float shiftRatio) {
    if (audioData.empty()) return;

    if (!g_formantShifter) {
        g_formantShifter = new RealtimeFormantShifter();
    }

    int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
    size_t numSamples = audioData.size() / sizeof(int16_t);

    SDL_Log(">>> FORMANT SHIFT FINAL CORRIGÉ (ratio: %.3f = %+.1f%%) <<<",
        shiftRatio, (shiftRatio - 1.0f) * 100.0f);

    // Conversion int16  float
    vector<float> inputFloat(numSamples);
    for (size_t i = 0; i < numSamples; ++i) {
        inputFloat[i] = static_cast<float>(samples[i]);
    }

    // Traitement par blocs
    vector<float> outputFloat(numSamples, 0.0f);
    size_t processedSamples = 0;

    while (processedSamples < numSamples) {
        size_t blockSize = min(static_cast<size_t>(RealtimeFormantShifter::HOP_SIZE),
            numSamples - processedSamples);

        g_formantShifter->ProcessFrame(
            inputFloat.data() + processedSamples,
            outputFloat.data() + processedSamples,
            blockSize,
            shiftRatio
        );

        processedSamples += blockSize;
    }

    // === NORMALISATION SIMPLE SANS COMPENSATION ===
    float maxVal = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        maxVal = max(maxVal, fabs(outputFloat[i]));
    }

    // Gain simple
    float scale = (maxVal > 1.0f) ? (28000.0f / maxVal) : 0.85f;

    // Conversion float --> int16
    for (size_t i = 0; i < numSamples; ++i) {
        float val = outputFloat[i] * scale;
        val = max(-32768.0f, min(32767.0f, val));
        samples[i] = static_cast<int16_t>(val);
    }

    SDL_Log(">>> FIN (Peak: %.1f, Scale: %.2f) <<<", maxVal, scale);
}



// === PITCH SHIFTER : HANN PURE + ZÉRO RÉSONANCE ===

struct RealtimePitchShifter {
    static constexpr size_t DELAY_BUFFER_SIZE = 512;
    static constexpr size_t CROSSFADE_SIZE = 96;

    vector<float> delayLine;
    vector<float> nextSegment; // segment futur

    size_t writePos = 0;
    float readPos = 0.0f;

    // État crossfade
    size_t crossfadePhase = 0;
    bool isCrossfading = false;

    // Positions de lecture alternatives
    float readPosPrev = 0.0f;  // Position avant saut
    float readPosNext = 0.0f;  // Position après saut

    float currentPitchRatio = 1.0f;
    int cooldownFrames = 0;

    RealtimePitchShifter() {
        delayLine.resize(DELAY_BUFFER_SIZE, 0.0f);  // INITIALISATION
        readPos = DELAY_BUFFER_SIZE / 2.0f;
    }

    // === INTERPOLATION HERMITE 4-POINTS ===
    inline float ReadDelayInterpolated(float position) const {
        while (position < 0.0f) position += DELAY_BUFFER_SIZE;
        while (position >= DELAY_BUFFER_SIZE) position -= DELAY_BUFFER_SIZE;

        size_t idx = static_cast<size_t>(position);
        float frac = position - idx;

        size_t im1 = (idx + DELAY_BUFFER_SIZE - 1) % DELAY_BUFFER_SIZE;
        size_t i0 = idx;
        size_t i1 = (idx + 1) % DELAY_BUFFER_SIZE;
        size_t i2 = (idx + 2) % DELAY_BUFFER_SIZE;

        float ym1 = delayLine[im1];
        float y0 = delayLine[i0];
        float y1 = delayLine[i1];
        float y2 = delayLine[i2];

        float c0 = y0;
        float c1 = 0.5f * (y1 - ym1);
        float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

        return c0 + c1 * frac + c2 * frac * frac + c3 * frac * frac * frac;
    }

    // === Processeur sans double contribution ===
    float ProcessSample(float input, float pitchShiftSemitones) {
        // 1) Écriture
        delayLine[writePos] = input;
        writePos = (writePos + 1) % DELAY_BUFFER_SIZE;

        // 2) Mise à jour ratio
        float targetRatio = pow(2.0f, pitchShiftSemitones / 12.0f);
        currentPitchRatio = 0.995f * currentPitchRatio + 0.005f * targetRatio;

        // 3) Lecture UNIQUE (pas de duplication)
        float output;

        if (!isCrossfading) {
            // Mode normal : lecture simple
            output = ReadDelayInterpolated(readPos);

            // Avancement
            readPos += currentPitchRatio;
            if (readPos >= DELAY_BUFFER_SIZE) readPos -= DELAY_BUFFER_SIZE;

            // Détection collision
            float distance = static_cast<float>(writePos) - readPos;
            if (distance < 0.0f) distance += DELAY_BUFFER_SIZE;

            if ((distance < CROSSFADE_SIZE + 24.0f ||
                distance > DELAY_BUFFER_SIZE - CROSSFADE_SIZE - 24.0f) &&
                cooldownFrames == 0) {

                // Initialiser crossfade
                isCrossfading = true;
                crossfadePhase = 0;

                // Capturer segment actuel
                nextSegment.resize(CROSSFADE_SIZE);
                for (size_t i = 0; i < CROSSFADE_SIZE; ++i) {
                    float pos = readPos + i * currentPitchRatio;
                    if (pos >= DELAY_BUFFER_SIZE) pos -= DELAY_BUFFER_SIZE;
                    nextSegment[i] = ReadDelayInterpolated(pos);
                }

                // Repositionner au centre
                readPosPrev = readPos;
                readPosNext = static_cast<float>(writePos) - (DELAY_BUFFER_SIZE / 2.0f);
                if (readPosNext < 0.0f) readPosNext += DELAY_BUFFER_SIZE;

                cooldownFrames = 128;
            }
        }
        else {
            // Mode crossfade : interpolation entre deux segments
            if (crossfadePhase < CROSSFADE_SIZE) {
                // Fenêtre Hann
                float t = static_cast<float>(crossfadePhase) / (CROSSFADE_SIZE - 1);
                float fadeOut = 0.5f * (1.0f + cosf(3.14159265f * t));
                float fadeIn = 0.5f * (1.0f - cosf(3.14159265f * t));

                // Lire depuis les deux positions
                float oldSample = nextSegment[crossfadePhase];
                float newSample = ReadDelayInterpolated(readPosNext);

                // Crossfade SANS sur-compensation
                output = oldSample * fadeOut + newSample * fadeIn;

                // Avancement UNIQUEMENT de la nouvelle position
                readPosNext += currentPitchRatio;
                if (readPosNext >= DELAY_BUFFER_SIZE) readPosNext -= DELAY_BUFFER_SIZE;

                crossfadePhase++;
            }
            else {
                // Fin crossfade
                isCrossfading = false;
                readPos = readPosNext;
                nextSegment.clear();

                output = ReadDelayInterpolated(readPos);
                readPos += currentPitchRatio;
                if (readPos >= DELAY_BUFFER_SIZE) readPos -= DELAY_BUFFER_SIZE;
            }
        }

        if (cooldownFrames > 0) cooldownFrames--;

        return output;
    }

    void ProcessBuffer(float* buffer, size_t numSamples, float pitchShiftSemitones) {
        for (size_t i = 0; i < numSamples; ++i) {
            buffer[i] = ProcessSample(buffer[i], pitchShiftSemitones);
        }
    }
};

// === INSTANCE GLOBALE ===
static RealtimePitchShifter* g_realtimePitchShifter = nullptr;

// === FONCTION D'INTERFACE AVEC COMPENSATION INTELLIGENTE ===
void ApplyRealtimePitchShift(vector<Uint8>& audioData, float semitones) {
    if (audioData.empty() || semitones == 0.0f) return;

    if (!g_realtimePitchShifter) {
        g_realtimePitchShifter = new RealtimePitchShifter();
    }

    int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
    size_t numSamples = audioData.size() / sizeof(int16_t);

    SDL_Log(">>> PITCH SHIFT ABSOLU-FINAL : HANN PURE + ZÉRO RÉSONANCE <<<");
    SDL_Log("  Shift: %.2f ST | Samples: %zu", semitones, numSamples);

    // === CONVERSION INT16 --> FLOAT ===
    vector<float> floatBuffer(numSamples);
    for (size_t i = 0; i < numSamples; ++i) {
        floatBuffer[i] = static_cast<float>(samples[i]);
    }

    // === MESURE AVANT ===
    float inputRMS = 0.0f;
    float inputPeak = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        inputRMS += floatBuffer[i] * floatBuffer[i];
        inputPeak = max(inputPeak, fabs(floatBuffer[i]));
    }
    inputRMS = sqrt(inputRMS / numSamples);

    SDL_Log("  RMS avant: %.1f | Peak avant: %.0f", inputRMS, inputPeak);

    // === TRAITEMENT ===
    g_realtimePitchShifter->ProcessBuffer(floatBuffer.data(), numSamples, semitones);

    // === MESURE APRÈS ===
    float outputRMS = 0.0f;
    float outputPeak = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        outputRMS += floatBuffer[i] * floatBuffer[i];
        outputPeak = max(outputPeak, fabs(floatBuffer[i]));
    }
    outputRMS = sqrt(outputRMS / numSamples);

    SDL_Log("  RMS après: %.1f | Peak après: %.0f", outputRMS, outputPeak);

    // === COMPENSATION RMS INTELLIGENTE (abs1% MAX) ===
    // Plus flexible que abs0.2% pour absorber les variations légitimes
    float rmsRatio = (outputRMS > 0.01f) ? (inputRMS / outputRMS) : 1.0f;
    rmsRatio = max(0.99f, min(1.01f, rmsRatio));  // abs1% (équilibré)

    SDL_Log("  Compensation RMS: %.4f (intelligente ±1%%)", rmsRatio);

    for (size_t i = 0; i < numSamples; ++i) {
        floatBuffer[i] *= rmsRatio;
    }

    // Recalculer peak
    outputPeak = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        outputPeak = max(outputPeak, fabs(floatBuffer[i]));
    }

    // === NORMALISATION DOUCE SI NÉCESSAIRE ===
    float scale = 1.0f;
    const float maxAllowedPeak = 30000.0f;

    if (outputPeak > maxAllowedPeak) {
        scale = 28000.0f / outputPeak;
        SDL_Log("  NORMALISATION DOUCE: %.4f (peak: %.0f)", scale, outputPeak);
    }

    // === CONVERSION FLOAT --> INT16 ===
    for (size_t i = 0; i < numSamples; ++i) {
        float val = floatBuffer[i] * scale;
        val = max(-32768.0f, min(32767.0f, val));
        samples[i] = static_cast<int16_t>(val);
    }

    SDL_Log(">>> FIN (Peak final: %.0f / 32767 = %.1f%%) <<<",
        outputPeak * scale, (outputPeak * scale / 32767.0f) * 100.0f);
}



// ============================================================================
// FONCTIONS ET RESTE DU CODE
// ============================================================================

// === CALLBACK PORTAUDIO ULTRA-RAPIDE : 32 FRAMES + RMS EXPONENTIELLE SINGLE-POLE ===
static int paRecordingCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    const float* in = (const float*)inputBuffer;
    (void)outputBuffer;

    if (in == nullptr) {
        return paContinue;
    }

    static int frameCounter = 0;

    // === [1] FILTRE HP IIR 1ER ORDRE (80 Hz) - VARIABLES STATIQUES ===
    static float hp_x1 = 0.0f, hp_y1 = 0.0f;
    constexpr float hp_alpha = 0.950f;  // Cutoff ~80 Hz @ 16 kHz

    // === [2] RMS EXPONENTIELLE SINGLE-POLE ULTRA-RAPIDE ===
    // Constante de temps : 0.5 ms @ 16 kHz (8 samples)
    // Formule : alpha = exp(-1 / (tau * sampleRate))
    //         = exp(-1 / (0.0005 * 16000)) = exp(-0.125) = environ 0.8825
    static float rms_squared = 0.0f;
    constexpr float rms_alpha = 0.8825f;  // Très réactif (0.5 ms)

    // === [3] NOISE GATE ADAPTATIF EXPONENTIEL ===
    static float noise_floor = 0.0f;
    static float noise_smoothing = 0.9995f;
    static bool noise_initialized = false;
    static int calibration_counter = 0;

    // === [4] EXPANDER DOUX (ratio 1:2) ===
    constexpr float expander_threshold = 0.015f;
    constexpr float expander_ratio = 2.0f;
    constexpr float expander_attack = 0.001f;   // 1 ms
    constexpr float expander_release = 0.050f;  // 50 ms

    static float expander_envelope = 1.0f;

    // === CALIBRATION INITIALE (30 premières frames = 60 ms avec 32 frames) ===
    if (!noise_initialized && calibration_counter < 30) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            float sample = fabs(in[i]);
            noise_floor = max(noise_floor, sample);
        }

        calibration_counter++;

        if (calibration_counter == 30) {
            noise_floor *= 1.8f;
            noise_initialized = true;
            SDL_Log(">>> Noise floor calibré ULTRA-RAPIDE: %.6f (60 ms @ 32 frames) <<<", noise_floor);
        }
    }

    // === TRAITEMENT SAMPLE-BY-SAMPLE (ZÉRO ALLOCATION) ===
    vector<float> cleanedSamples(framesPerBuffer);

    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        float sample = in[i * paMicrophoneChannels];

        // === [1] FILTRE HP IIR 1ER ORDRE ===
        float hp_output = hp_alpha * (hp_y1 + sample - hp_x1);
        hp_x1 = sample;
        hp_y1 = hp_output;
        sample = hp_output;

        // === [2] RMS EXPONENTIELLE SINGLE-POLE (ULTRA-RAPIDE) ===
        // Mise à jour : rms = alpha × rms_old + (1-alpha) × sample
        rms_squared = rms_alpha * rms_squared + (1.0f - rms_alpha) * (sample * sample);
        float rms_current = sqrt(rms_squared);

        // === [3] MISE À JOUR NOISE FLOOR ADAPTATIF ===
        if (noise_initialized && rms_current < noise_floor * 0.2f) {
            noise_floor = noise_smoothing * noise_floor + (1.0f - noise_smoothing) * rms_current;
        }

        // === [4] NOISE GATE EXPONENTIEL ===
        float gate_threshold = noise_floor * 4.0f;
        float abs_sample = fabs(sample);

        if (abs_sample < gate_threshold) {
            // Courbe exponentielle douce
            float ratio = abs_sample / gate_threshold;
            float attenuation = (exp(ratio * 2.0f) - 1.0f) / (exp(2.0f) - 1.0f);
            sample *= attenuation;
        }

        // === [5] EXPANDER DOUX 1:2 ===
        if (rms_current < expander_threshold) {
            float target_gain = 1.0f - ((expander_threshold - rms_current) / expander_threshold) * (1.0f - 1.0f / expander_ratio);

            // Envelope follower
            float attack_coeff = exp(-1.0f / (expander_attack * PA_SAMPLE_RATE));
            float release_coeff = exp(-1.0f / (expander_release * PA_SAMPLE_RATE));

            if (target_gain < expander_envelope) {
                expander_envelope = attack_coeff * expander_envelope + (1.0f - attack_coeff) * target_gain;
            }
            else {
                expander_envelope = release_coeff * expander_envelope + (1.0f - release_coeff) * target_gain;
            }

            sample *= expander_envelope;
        }
        else {
            expander_envelope = 1.0f;
        }

        cleanedSamples[i] = sample;
    }

    // === REMPLIR LES BUFFERS (waveform, FFT, enregistrement) ===
    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        waveformBuffer[waveformWritePos] = cleanedSamples[i];
        waveformWritePos = (waveformWritePos + 1) % WAVEFORM_SAMPLES;
    }

    {
        lock_guard<mutex> lock(paFftMutex);
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            paFftInputBuffer.push_back(cleanedSamples[i]);
        }
        while (paFftInputBuffer.size() > PA_FFT_SIZE * 2) {
            paFftInputBuffer.erase(paFftInputBuffer.begin(),
                paFftInputBuffer.begin() + (paFftInputBuffer.size() - PA_FFT_SIZE));
        }
    }

    if (paIsRecording) {
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            float sample = cleanedSamples[i];
            sample = max(-1.0f, min(1.0f, sample));
            int16_t sample16 = static_cast<int16_t>(sample * 32767.0f);
            paAudioBuffer.push_back(sample16 & 0xFF);
            paAudioBuffer.push_back((sample16 >> 8) & 0xFF);
        }
    }

    // === LOG PÉRIODIQUE ===
    if (++frameCounter % 100 == 0) {
        float rms = sqrt(rms_squared);

        SDL_Log("PortAudio ULTRA-RAPIDE (32 frames): RMS=%.6f | Noise=%.6f | Gate=%.6f | Expander=%.3f",
            rms, noise_floor, noise_floor * 4.0f, expander_envelope);
    }

    waveformDataReady = true;
    return paContinue;
}

// === CALLBACK PORTAUDIO POUR LA LECTURE ===
static int paPlaybackCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    float* out = (float*)outputBuffer;
    (void)inputBuffer;

    if (out == nullptr) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    if (!paIsPlaying || paAudioBuffer.empty()) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    int16_t* samples = reinterpret_cast<int16_t*>(paAudioBuffer.data());
    size_t totalSamples = paAudioBuffer.size() / sizeof(int16_t);

    // === LIRE LE VOLUME DEPUIS LE SLIDER ===
    float volume = g_outputVolume.load(memory_order_acquire);

    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackPosition < totalSamples) {
            // Convertir int16 -> float32
            float sample = static_cast<float>(samples[paPlaybackPosition]) / 32767.0f;

            // === APPLIQUER LE VOLUME DU SLIDER ===
            sample *= volume;

            // Limiter
            sample = max(-1.0f, min(1.0f, sample));

            // Écrire sur tous les canaux
            for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                out[frame * paMicrophoneChannels + ch] = sample;
            }

            paPlaybackPosition += paMicrophoneChannels;
        }
        else {
            // Fin du buffer - REMPLIR LE RESTE DE SILENCE
            for (unsigned long j = frame; j < framesPerBuffer; ++j) {
                for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                    out[j * paMicrophoneChannels + ch] = 0.0f;
                }
            }
            paIsPlaying = false;
            SDL_Log(">>> Callback PortAudio: Fin de lecture détectée <<<");
            break;
        }
    }

    return paContinue;
}

// === CALLBACK PORTAUDIO POUR LA LECTURE ANONYMISÉE ===
static int paPlaybackAnonymizedCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    float* out = (float*)outputBuffer;
    (void)inputBuffer;

    if (out == nullptr) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    if (!paIsPlayingAnonymized || paProcessedBuffer.empty()) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    int16_t* samples = reinterpret_cast<int16_t*>(paProcessedBuffer.data());
    size_t totalSamples = paProcessedBuffer.size() / sizeof(int16_t);

    // === LIRE LES PARAMÈTRES DEPUIS LES SLIDERS (EN TEMPS RÉEL) ===
    float pitchShift = g_pitchShiftSemitones.load(memory_order_acquire);
    float jitter = g_jitterAmount.load(memory_order_acquire);
    float volume = g_outputVolume.load(memory_order_acquire);

    // === CRÉER INSTANCE PITCH SHIFTER SI NÉCESSAIRE ===
    if (!g_realtimePitchShifter) {
        g_realtimePitchShifter = new RealtimePitchShifter();
    }

    static mt19937 rng(random_device{}());

    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackAnonymizedPosition < totalSamples) {
            // Lire échantillon original
            float sample = static_cast<float>(samples[paPlaybackAnonymizedPosition]) / 32767.0f;

            // === TRAITEMENT DSP TEMPS RÉEL ===
            // 1) Pitch shift (valeur du slider)
            sample = g_realtimePitchShifter->ProcessSample(sample, pitchShift);

            // 2) Jitter (valeur du slider)
            uniform_real_distribution<float> dist(-jitter, jitter);
            sample *= (1.0f + dist(rng));

            // 3) Volume (valeur du slider)
            sample *= volume;

            // 4) Limiter
            sample = max(-1.0f, min(1.0f, sample));

            // Écrire sur tous les canaux
            for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                out[frame * paMicrophoneChannels + ch] = sample;
            }

            paPlaybackAnonymizedPosition += paMicrophoneChannels;
        }
        else {
            for (unsigned long j = frame; j < framesPerBuffer; ++j) {
                for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                    out[j * paMicrophoneChannels + ch] = 0.0f;
                }
            }
            paIsPlayingAnonymized = false;
            SDL_Log(">>> Callback PortAudio Anonymisé: Fin de lecture <<<");
            break;
        }
    }

    return paContinue;
}

// Callback playback formant
static int paPlaybackAnonymizedFormantCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    float* out = (float*)outputBuffer;
    (void)inputBuffer;

    if (out == nullptr) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    if (!paIsPlayingAnonymizedFormant || paProcessedFormantBuffer.empty()) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    int16_t* samples = reinterpret_cast<int16_t*>(paProcessedFormantBuffer.data());
    size_t totalSamples = paProcessedFormantBuffer.size() / sizeof(int16_t);

    // === LIRE LES PARAMÈTRES DEPUIS LES SLIDERS ===
    float formantShift = g_formantShiftRatio.load(memory_order_acquire);
    float jitter = g_jitterAmount.load(memory_order_acquire);
    float volume = g_outputVolume.load(memory_order_acquire);

    // === CRÉER INSTANCES SI NÉCESSAIRE ===
    if (!g_formantShifter) {
        g_formantShifter = new RealtimeFormantShifter();
    }

    static mt19937 rng(random_device{}());
    static vector<float> frameBuffer(framesPerBuffer);

    // Traiter par blocs (formant shifter nécessite des frames complètes)
    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackAnonymizedFormantPosition < totalSamples) {
            frameBuffer[frame] = static_cast<float>(samples[paPlaybackAnonymizedFormantPosition]);
            paPlaybackAnonymizedFormantPosition += paMicrophoneChannels;
        }
        else {
            frameBuffer[frame] = 0.0f;
        }
    }

    // === TRAITEMENT DSP PAR BLOC ===
    static vector<float> outputFrameBuffer(framesPerBuffer);
    g_formantShifter->ProcessFrame(frameBuffer.data(), outputFrameBuffer.data(),
        framesPerBuffer, formantShift);

    // === POST-TRAITEMENT ET SORTIE ===
    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackAnonymizedFormantPosition - frame * paMicrophoneChannels <= totalSamples) {
            float sample = outputFrameBuffer[frame] / 32767.0f;

            // Jitter
            uniform_real_distribution<float> dist(-jitter, jitter);
            sample *= (1.0f + dist(rng));

            // Volume
            sample *= volume;

            // Limiter
            sample = max(-1.0f, min(1.0f, sample));

            for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                out[frame * paMicrophoneChannels + ch] = sample;
            }
        }
        else {
            for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                out[frame * paMicrophoneChannels + ch] = 0.0f;
            }
            paIsPlayingAnonymizedFormant = false;
            SDL_Log(">>> Callback Formant: Fin de lecture <<<");
            break;
        }
    }

    if (paPlaybackAnonymizedFormantPosition >= totalSamples) {
        paIsPlayingAnonymizedFormant = false;
        SDL_Log(">>> Callback Formant: Fin de lecture <<<");
    }

    return paContinue;
}

// === CALLBACK PORTAUDIO POUR PITCH+FORMANT (ajouter après paPlaybackAnonymizedFormantCallback) ===
static int paPlaybackAnonymizedPitchFormantCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    float* out = (float*)outputBuffer;
    (void)inputBuffer;

    if (out == nullptr || !paIsPlayingAnonymizedPitchFormant || paProcessedPitchFormantBuffer.empty()) {
        if (out != nullptr) {
            for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
                out[i] = 0.0f;
            }
        }
        return paContinue;
    }

    int16_t* samples = reinterpret_cast<int16_t*>(paProcessedPitchFormantBuffer.data());
    size_t totalSamples = paProcessedPitchFormantBuffer.size() / sizeof(int16_t);

    // === LIRE LES 4 PARAMÈTRES ===
    float pitchShift = g_pitchShiftSemitones.load(memory_order_acquire);
    float formantShift = g_formantShiftRatio.load(memory_order_acquire);
    float jitter = g_jitterAmount.load(memory_order_acquire);
    float volume = g_outputVolume.load(memory_order_acquire);

    // === CRÉER INSTANCES ===
    if (!g_realtimePitchShifter) g_realtimePitchShifter = new RealtimePitchShifter();
    if (!g_formantShifter) g_formantShifter = new RealtimeFormantShifter();

    static mt19937 rng(random_device{}());
    static vector<float> frameBuffer(framesPerBuffer);
    static vector<float> outputFrameBuffer(framesPerBuffer);

    // === LECTURE + PITCH SHIFT ===
    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackAnonymizedPitchFormantPosition < totalSamples) {
            float sample = static_cast<float>(samples[paPlaybackAnonymizedPitchFormantPosition]);

            // 1) Pitch shift sample-par-sample
            frameBuffer[frame] = g_realtimePitchShifter->ProcessSample(sample, pitchShift);

            paPlaybackAnonymizedPitchFormantPosition += paMicrophoneChannels;
        }
        else {
            frameBuffer[frame] = 0.0f;
        }
    }

    // === FORMANT SHIFT PAR BLOC ===
    g_formantShifter->ProcessFrame(frameBuffer.data(), outputFrameBuffer.data(),
        framesPerBuffer, formantShift);

    // === POST-TRAITEMENT + SORTIE ===
    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        float sample = outputFrameBuffer[frame] / 32767.0f;

        // Jitter
        uniform_real_distribution<float> dist(-jitter, jitter);
        sample *= (1.0f + dist(rng));

        // Volume
        sample *= volume;

        // Limiter
        sample = max(-1.0f, min(1.0f, sample));

        for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
            out[frame * paMicrophoneChannels + ch] = sample;
        }
    }

    if (paPlaybackAnonymizedPitchFormantPosition >= totalSamples) {
        paIsPlayingAnonymizedPitchFormant = false;
        SDL_Log(">>> Callback Pitch+Formant: Fin de lecture <<<");
    }

    return paContinue;
}

// Fonction de démarrage playback formant
bool StartPortAudioPlaybackAnonymizedFormant()
{
    SDL_Log("=== StartPortAudioPlaybackAnonymizedFormant appelé ===");

    if (paIsPlayingAnonymizedFormant) {
        SDL_Log("Impossible de lire: déjà en lecture PortAudio formant");
        return false;
    }

    if (paAudioBuffer.empty()) {
        SDL_Log("Impossible de lire: buffer PortAudio vide");
        return false;
    }

    // === COPIE DU BUFFER ORIGINAL (SANS TRAITEMENT DSP) ===
    paProcessedFormantBuffer = paAudioBuffer;

    SDL_Log("Buffer copié: %zu octets (traitement DSP temps réel dans callback)", paProcessedFormantBuffer.size());

    // === CRÉER LE STREAM ===
    if (paPlaybackAnonymizedFormantStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        if (outputParams.device == paNoDevice) {
            SDL_Log("Aucun device de sortie trouvé");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        outputParams.channelCount = paMicrophoneChannels;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
        outputParams.hostApiSpecificStreamInfo = NULL;

        PaError err = Pa_OpenStream(&paPlaybackAnonymizedFormantStream, NULL, &outputParams,
            PA_SAMPLE_RATE, 128, paClipOff, paPlaybackAnonymizedFormantCallback, NULL);

        if (err != paNoError) {
            SDL_Log("Erreur Pa_OpenStream playback formant: %s", Pa_GetErrorText(err));
            return false;
        }

        err = Pa_StartStream(paPlaybackAnonymizedFormantStream);
        if (err != paNoError) {
            SDL_Log("Erreur Pa_StartStream playback formant: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackAnonymizedFormantStream);
            paPlaybackAnonymizedFormantStream = nullptr;
            return false;
        }
    }

    paPlaybackAnonymizedFormantPosition = 0;
    paIsPlayingAnonymizedFormant = true;

    SDL_Log(">>> Lecture FORMANT SHIFT démarrée (DSP temps réel avec sliders) <<<");
    return true;
}

void StopPortAudioPlaybackAnonymizedFormant()
{
    if (!paIsPlayingAnonymizedFormant) return;

    paIsPlayingAnonymizedFormant = false;
    paPlaybackAnonymizedFormantPosition = 0;

    SDL_Log("Lecture formant arrêtée");
}

// === FONCTIONS DE DÉMARRAGE/ARRÊT PITCH+FORMANT (ajouter après StopPortAudioPlaybackAnonymizedFormant, ligne ~850) ===
bool StartPortAudioPlaybackAnonymizedPitchFormant()
{
    SDL_Log("=== StartPortAudioPlaybackAnonymizedPitchFormant appelé ===");

    if (paIsPlayingAnonymizedPitchFormant) {
        SDL_Log("Impossible de lire: déjà en lecture PortAudio Pitch+Formant");
        return false;
    }

    if (paAudioBuffer.empty()) {
        SDL_Log("Impossible de lire: buffer PortAudio vide");
        return false;
    }

    // === COPIE DU BUFFER ORIGINAL (SANS TRAITEMENT DSP) ===
    paProcessedPitchFormantBuffer = paAudioBuffer;

    SDL_Log("Buffer copié: %zu octets (traitement DSP temps réel dans callback)", paProcessedPitchFormantBuffer.size());

    // === CRÉER LE STREAM ===
    if (paPlaybackAnonymizedPitchFormantStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        if (outputParams.device == paNoDevice) {
            SDL_Log("Aucun device de sortie trouvé");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        outputParams.channelCount = paMicrophoneChannels;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
        outputParams.hostApiSpecificStreamInfo = NULL;

        PaError err = Pa_OpenStream(&paPlaybackAnonymizedPitchFormantStream, NULL, &outputParams,
            PA_SAMPLE_RATE, 128, paClipOff, paPlaybackAnonymizedPitchFormantCallback, NULL);

        if (err != paNoError) {
            SDL_Log("Erreur Pa_OpenStream playback Pitch+Formant: %s", Pa_GetErrorText(err));
            return false;
        }

        err = Pa_StartStream(paPlaybackAnonymizedPitchFormantStream);
        if (err != paNoError) {
            SDL_Log("Erreur Pa_StartStream playback Pitch+Formant: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackAnonymizedPitchFormantStream);
            paPlaybackAnonymizedPitchFormantStream = nullptr;
            return false;
        }
    }

    paPlaybackAnonymizedPitchFormantPosition = 0;
    paIsPlayingAnonymizedPitchFormant = true;

    SDL_Log(">>> Lecture PITCH+FORMANT COMBINÉ démarrée (DSP temps réel avec sliders) <<<");
    return true;
}

void StopPortAudioPlaybackAnonymizedPitchFormant()
{
    if (!paIsPlayingAnonymizedPitchFormant) return;

    paIsPlayingAnonymizedPitchFormant = false;
    paPlaybackAnonymizedPitchFormantPosition = 0;

    SDL_Log("Lecture Pitch+Formant arrêtée");
}

// === FONCTIONS DE CONTRÔLE PORTAUDIO ===
bool StartPortAudioRecording()
{
    SDL_Log("=== StartPortAudioRecording appelé ===");

    if (paIsRecording) {
        SDL_Log("Déjà en cours d'enregistrement PortAudio");
        return false;
    }

    // === VIDER LE BUFFER À CHAQUE NOUVEAU DÉMARRAGE ===
    paAudioBuffer.clear();
    paAudioBuffer.shrink_to_fit(); // Libérer la mémoire
    SDL_Log("Buffer PortAudio vidé (nouvelle capacité: %zu)", paAudioBuffer.capacity());

    paIsRecording = true;

    SDL_Log(">>> Enregistrement PortAudio DÉMARRÉ <<<");
    return true;
}

void StopPortAudioRecording()
{
    if (!paIsRecording) return;

    paIsRecording = false;
    SDL_Log("Enregistrement PortAudio arrêté - %zu octets capturés", paAudioBuffer.size());
}

bool StartPortAudioPlayback()
{
    SDL_Log("=== StartPortAudioPlayback appelé ===");

    if (paIsPlaying) {
        SDL_Log("Impossible de lire: déjà en lecture PortAudio");
        return false;
    }

    if (paAudioBuffer.empty()) {
        SDL_Log("Impossible de lire: buffer PortAudio vide");
        return false;
    }

    SDL_Log("Buffer PortAudio contient %zu octets (%zu samples, %d canaux)",
        paAudioBuffer.size(),
        paAudioBuffer.size() / sizeof(int16_t),
        paMicrophoneChannels);

    // === CRÉER LE STREAM DE PLAYBACK ===
    if (paPlaybackStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        if (outputParams.device == paNoDevice) {
            SDL_Log("Aucun device de sortie trouvé");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        outputParams.channelCount = paMicrophoneChannels;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
        outputParams.hostApiSpecificStreamInfo = NULL;

        PaError err = Pa_OpenStream(&paPlaybackStream, NULL, &outputParams,
            PA_SAMPLE_RATE, 128, paClipOff, paPlaybackCallback, NULL);

        if (err != paNoError) {
            SDL_Log("Erreur Pa_OpenStream playback: %s", Pa_GetErrorText(err));
            return false;
        }

        err = Pa_StartStream(paPlaybackStream);
        if (err != paNoError) {
            SDL_Log("Erreur Pa_StartStream playback: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackStream);
            paPlaybackStream = nullptr;
            return false;
        }

        SDL_Log("Stream PortAudio playback créé avec succès (%d canaux)", paMicrophoneChannels);
    }

    paPlaybackPosition = 0;
    paIsPlaying = true;

    SDL_Log(">>> Lecture PortAudio DÉMARRÉE (volume contrôlé par slider) <<<");
    return true;
}

void StopPortAudioPlayback()
{
    if (!paIsPlaying) return;

    paIsPlaying = false;
    paPlaybackPosition = 0;

    SDL_Log("Lecture PortAudio arrêtée");
}

bool StartPortAudioPlaybackAnonymized()
{
    SDL_Log("=== StartPortAudioPlaybackAnonymized appelé ===");

    if (paIsPlayingAnonymized) {
        SDL_Log("Impossible de lire: déjà en lecture PortAudio anonymisée");
        return false;
    }

    if (paAudioBuffer.empty()) {
        SDL_Log("Impossible de lire: buffer PortAudio vide");
        return false;
    }

    // === COPIE DU BUFFER ORIGINAL (SANS TRAITEMENT DSP) ===
    paProcessedBuffer = paAudioBuffer;

    SDL_Log("Buffer copié: %zu octets (traitement DSP temps réel dans callback)", paProcessedBuffer.size());

    // === CRÉER LE STREAM DE PLAYBACK ===
    if (paPlaybackAnonymizedStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        if (outputParams.device == paNoDevice) {
            SDL_Log("Aucun device de sortie trouvé");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        outputParams.channelCount = paMicrophoneChannels;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
        outputParams.hostApiSpecificStreamInfo = NULL;

        PaError err = Pa_OpenStream(&paPlaybackAnonymizedStream, NULL, &outputParams,
            PA_SAMPLE_RATE, 128, paClipOff, paPlaybackAnonymizedCallback, NULL);

        if (err != paNoError) {
            SDL_Log("Erreur Pa_OpenStream playback anonymisé: %s", Pa_GetErrorText(err));
            return false;
        }

        err = Pa_StartStream(paPlaybackAnonymizedStream);
        if (err != paNoError) {
            SDL_Log("Erreur Pa_StartStream playback anonymisé: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackAnonymizedStream);
            paPlaybackAnonymizedStream = nullptr;
            return false;
        }

        SDL_Log("Stream PortAudio playback anonymisé créé avec succès");
    }

    paPlaybackAnonymizedPosition = 0;
    paIsPlayingAnonymized = true;

    SDL_Log(">>> Lecture PortAudio ANONYMISÉE DÉMARRÉE (DSP temps réel avec sliders) <<<");
    return true;
}

void StopPortAudioPlaybackAnonymized()
{
    if (!paIsPlayingAnonymized) return;

    paIsPlayingAnonymized = false;
    paPlaybackAnonymizedPosition = 0;

    SDL_Log("Lecture PortAudio anonymisée arrêtée");
}

bool InitPortAudio()
{
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        SDL_Log("Erreur Pa_Initialize: %s", Pa_GetErrorText(err));
        return false;
    }

    PaStreamParameters inputParams;
    inputParams.device = Pa_GetDefaultInputDevice();

    if (inputParams.device == paNoDevice) {
        SDL_Log("Aucun microphone trouvé");
        Pa_Terminate();
        return false;
    }

    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(inputParams.device);
    SDL_Log("=== DEVICE INFO ===");
    SDL_Log("Nom: %s", deviceInfo->name);
    SDL_Log("Canaux d'entrée max: %d", deviceInfo->maxInputChannels);
    SDL_Log("Sample rate par défaut: %.0f Hz", deviceInfo->defaultSampleRate);

    // === DÉTECTION INTELLIGENTE : TESTER D'ABORD STÉRÉO, PUIS FALLBACK MONO ===
    double sampleRate = PA_SAMPLE_RATE;
    bool stereoSuccess = false;

    // === ÉTAPE 1 : TENTER STÉRÉO SI LE HARDWARE LE SUPPORTE ===
    if (deviceInfo->maxInputChannels >= 2) {
        SDL_Log("Test d'ouverture en STÉRÉO (2 canaux)...");

        inputParams.channelCount = 2;
        inputParams.sampleFormat = paFloat32;
        //inputParams.suggestedLatency = deviceInfo->defaultLowInputLatency;
        inputParams.suggestedLatency = 0.003;   // Au lieu de defaultLowInputLatency
        inputParams.hostApiSpecificStreamInfo = NULL;

        err = Pa_OpenStream(&paStream, &inputParams, NULL, sampleRate, 320,  //STREAM_BUFFER_SIZE-->64
            paClipOff, paUnifiedCallback, NULL);

        if (err == paNoError) {
            // === LE STÉRÉO EST-IL RÉEL OU DUPLIQUÉ ? ===
            // On démarre temporairement le stream pour analyser les données
            err = Pa_StartStream(paStream);
            if (err == paNoError) {
                // Attendre quelques frames pour analyser le signal
                Pa_Sleep(200); // 200ms d'échantillonnage

                // === ANALYSE DES CANAUX : Comparer canal gauche vs canal droit ===
                // Si les canaux sont IDENTIQUES --> mono dupliqué
                // Si les canaux sont DIFFÉRENTS --> stéréo réel

                // Pour simplifier, on vérifie si paAudioBuffer contient des données différentes
                // entre les canaux pairs (gauche) et impairs (droite)

                bool channelsAreDifferent = false;

                if (paAudioBuffer.size() >= 4096) { // Au moins 2048 samples stéréo
                    int16_t* samples = reinterpret_cast<int16_t*>(paAudioBuffer.data());
                    size_t numSamples = paAudioBuffer.size() / sizeof(int16_t);

                    int differenceCount = 0;
                    int totalSamples = 0;

                    // Comparer les canaux par paires
                    for (size_t i = 0; i < numSamples - 1; i += 2) {
                        int16_t leftChannel = samples[i];
                        int16_t rightChannel = samples[i + 1];

                        // Tolérance de 5% pour le bruit
                        int16_t tolerance = abs(leftChannel) / 20;

                        if (abs(leftChannel - rightChannel) > tolerance) {
                            differenceCount++;
                        }

                        totalSamples++;
                    }

                    // Si plus de 10% des samples sont différents --> stéréo réel
                    if (totalSamples > 0 && (differenceCount * 100 / totalSamples) > 10) {
                        channelsAreDifferent = true;
                    }

                    SDL_Log("Analyse stéréo: %d/%d samples différents (%.1f%%)",
                        differenceCount, totalSamples,
                        totalSamples > 0 ? (differenceCount * 100.0f / totalSamples) : 0.0f);
                }

                // Arrêter et fermer le stream de test
                Pa_StopStream(paStream);
                Pa_CloseStream(paStream);
                paStream = nullptr;

                // Vider le buffer de test
                paAudioBuffer.clear();

                if (channelsAreDifferent) {
                    SDL_Log(">>> STÉRÉO RÉEL DÉTECTÉ (canaux indépendants) <<<");
                    stereoSuccess = true;
                    paMicrophoneChannels = 2;
                }
                else {
                    SDL_Log(">>> MONO DUPLIQUÉ DÉTECTÉ (canaux identiques) <<<");
                    SDL_Log("Fallback sur MONO...");
                    stereoSuccess = false;
                }
            }
            else {
                SDL_Log("Échec démarrage stream stéréo: %s", Pa_GetErrorText(err));
                Pa_CloseStream(paStream);
                paStream = nullptr;
                stereoSuccess = false;
            }
        }
        else {
            SDL_Log("Échec ouverture STÉRÉO: %s", Pa_GetErrorText(err));
            SDL_Log("Fallback automatique sur MONO...");
            stereoSuccess = false;
        }
    }
    else {
        SDL_Log("Hardware ne supporte qu'1 canal --> MONO uniquement");
    }

    // === CRÉER LES PARAMÈTRES DE SORTIE ===
    PaStreamParameters outputParams;
    outputParams.device = Pa_GetDefaultOutputDevice();

    if (outputParams.device == paNoDevice) {
        SDL_Log("ATTENTION: Aucun device de sortie trouvé");
        // Continuer en mode input-only (fallback)
    }

    const PaDeviceInfo* outputDeviceInfo = Pa_GetDeviceInfo(outputParams.device);
    outputParams.channelCount = paMicrophoneChannels;  // Utiliser le même nombre de canaux
    outputParams.sampleFormat = paFloat32;
    //outputParams.suggestedLatency = outputDeviceInfo->defaultLowOutputLatency;
    outputParams.suggestedLatency = 0.003;
    outputParams.hostApiSpecificStreamInfo = NULL;

    SDL_Log("Device de sortie: %s (%d canaux)",
        outputDeviceInfo->name, outputParams.channelCount);

    // === CRÉER LE STREAM AVEC ENTRÉE **ET** SORTIE ===
    // === ÉTAPE 2 : SI STÉRÉO A ÉCHOUÉ OU N'EST PAS RÉEL, UTILISER MONO ===
    if (!stereoSuccess) {
        SDL_Log("Ouverture en MONO DUPLEX (1 canal in/out)...");

        inputParams.channelCount = 1;
        inputParams.sampleFormat = paFloat32;
        inputParams.suggestedLatency = deviceInfo->defaultLowInputLatency;
        inputParams.hostApiSpecificStreamInfo = NULL;

        outputParams.channelCount = 1;  // Assurer la cohérence

        // === FIX PRINCIPAL : Passer outputParams au lieu de NULL ===
        err = Pa_OpenStream(&paStream,
            &inputParams,      // Entrée microphone
            &outputParams,     //  Sortie haut-parleurs (au lieu de NULL)
            sampleRate,
            320,
            paClipOff,
            paUnifiedCallback,
            NULL);

        if (err != paNoError) {
            SDL_Log("Erreur Pa_OpenStream MONO DUPLEX: %s", Pa_GetErrorText(err));
            Pa_Terminate();
            return false;
        }

        paMicrophoneChannels = 1;
        SDL_Log(">>> MONO DUPLEX confirmé (entrée + sortie) <<<");
    }
    else {
        // === RÉOUVRIR EN STÉRÉO DUPLEX ===
        SDL_Log("Ouverture en STÉRÉO DUPLEX (2 canaux in/out)...");

        inputParams.channelCount = 2;
        inputParams.sampleFormat = paFloat32;
        inputParams.suggestedLatency = deviceInfo->defaultLowInputLatency;
        inputParams.hostApiSpecificStreamInfo = NULL;

        outputParams.channelCount = 2;

        err = Pa_OpenStream(&paStream,
            &inputParams,
            &outputParams,     //  Sortie haut-parleurs
            sampleRate,
            320,
            paClipOff,
            paUnifiedCallback,
            NULL);

        if (err != paNoError) {
            SDL_Log("ERREUR: Impossible d'ouvrir en stéréo duplex: %s", Pa_GetErrorText(err));
            Pa_Terminate();
            return false;
        }
    }

    // === DÉMARRER LE STREAM FINAL ===
    err = Pa_StartStream(paStream);
    if (err != paNoError) {
        SDL_Log("Erreur Pa_StartStream: %s", Pa_GetErrorText(err));
        Pa_CloseStream(paStream);
        Pa_Terminate();
        return false;
    }

    SDL_Log(">>> PortAudio DUPLEX initialisé avec succès <<<");
    SDL_Log("  Device entrée: %s", deviceInfo->name);
    SDL_Log("  Device sortie: %s", outputDeviceInfo->name);
    SDL_Log("  Format: %s (%d canal/canaux)",
        paMicrophoneChannels == 2 ? "STÉRÉO RÉEL" : "MONO",
        paMicrophoneChannels);
    SDL_Log("  Sample rate: %.0f Hz", sampleRate);
    SDL_Log("  Latence: %zu échantillons (~%.1f ms)",
        STREAM_BUFFER_SIZE,
        (STREAM_BUFFER_SIZE * 1000.0f) / sampleRate);

    return true;
}

// === DANS CleanupPortAudio() ===
void CleanupPortAudio()
{
    if (paStream) {
        Pa_StopStream(paStream);
        Pa_CloseStream(paStream);
        paStream = nullptr;
    }

    if (paPlaybackStream) {
        Pa_StopStream(paPlaybackStream);
        Pa_CloseStream(paPlaybackStream);
        paPlaybackStream = nullptr;
    }

    if (paPlaybackAnonymizedStream) {
        Pa_StopStream(paPlaybackAnonymizedStream);
        Pa_CloseStream(paPlaybackAnonymizedStream);
        paPlaybackAnonymizedStream = nullptr;
    }

    if (paPlaybackAnonymizedFormantStream) {
        Pa_StopStream(paPlaybackAnonymizedFormantStream);
        Pa_CloseStream(paPlaybackAnonymizedFormantStream);
        paPlaybackAnonymizedFormantStream = nullptr;
    }

    if (paPlaybackAnonymizedPitchFormantStream) {
        Pa_StopStream(paPlaybackAnonymizedPitchFormantStream);
        Pa_CloseStream(paPlaybackAnonymizedPitchFormantStream);
        paPlaybackAnonymizedPitchFormantStream = nullptr;
    }

    Pa_Terminate();
}

// Trouver le maximum dans un tableau
inline float FindMax(const vector<float>& data) noexcept {
    if (data.empty()) return 0.0f;

    float maxVal = data[0];
    for (size_t i = 1; i < data.size(); ++i) {
        if (data[i] > maxVal) {
            maxVal = data[i];
        }
    }
    return maxVal;
}



// Normalisation audio pour éviter la saturation
void NormalizeAudio(vector<Uint8>& audioData)
{
    if (audioData.empty()) return;

    int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
    size_t numSamples = audioData.size() / sizeof(int16_t);

    SDL_Log(">>> DÉBUT NormalizeAudio <<<");
    SDL_Log("  Samples à normaliser: %zu", numSamples);

    // Trouver le pic maximal
    int16_t maxSample = 0;
    int zeroCount = 0;
    int invalidCount = 0;

    for (size_t i = 0; i < numSamples; ++i) {
        int16_t absSample = abs(samples[i]);
        if (absSample > maxSample) {
            maxSample = absSample;
        }
        if (samples[i] == 0) zeroCount++;

        // Détecter valeurs anormales
        if (absSample > 32767 || absSample < -32768) {
            invalidCount++;
        }
    }

    SDL_Log("  Peak avant normalisation: %d / 32767", maxSample);
    SDL_Log("  Zéros: %d (%.1f%%) | Invalides: %d",
        zeroCount, (zeroCount * 100.0f) / numSamples, invalidCount);

    if (maxSample == 0) {
        SDL_Log("  ATTENTION: Signal complètement silencieux !");
        return;
    }

    if (invalidCount > 0) {
        SDL_Log("  ERREUR CRITIQUE: %d valeurs hors limites détectées !", invalidCount);
    }

    // Normaliser à 90% pour laisser une marge
    float scale = (32767.0f * 0.7f) / maxSample;
    SDL_Log("  Facteur de normalisation: %.4f", scale);

    for (size_t i = 0; i < numSamples; ++i) {
        float sample = static_cast<float>(samples[i]) * scale;

        // Clamper par sécurité
        if (sample > 32767.0f) sample = 32767.0f;
        if (sample < -32768.0f) sample = -32768.0f;

        samples[i] = static_cast<int16_t>(sample);
    }

    SDL_Log(">>> FIN NormalizeAudio <<<");
}

// Filtre passe-haut pour supprimer le bruit basse fréquence AVANT pitch shift
void ApplyHighPassFilter(vector<Uint8>& audioData, float cutoffFreq = 80.0f)
{
    if (audioData.empty()) return;

    int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
    size_t numSamples = audioData.size() / sizeof(int16_t);

    SDL_Log(">>> DÉBUT ApplyHighPassFilter (%.0f Hz) <<<", cutoffFreq);

    // Convertir en float
    vector<float> inputFloat(numSamples);
    for (size_t i = 0; i < numSamples; ++i) {
        inputFloat[i] = static_cast<float>(samples[i]);
    }

    // Filtre biquad passe-haut (2nd order Butterworth)
    const float sampleRate = PA_SAMPLE_RATE;
    const float omega = 2.0f * 3.14159265f * cutoffFreq / sampleRate;
    const float cosOmega = cos(omega);
    const float sinOmega = sin(omega);
    const float alpha = sinOmega / (2.0f * 0.707f); // Q = 0.707 pour Butterworth

    // Coefficients biquad
    const float b0 = (1.0f + cosOmega) / 2.0f;
    const float b1 = -(1.0f + cosOmega);
    const float b2 = (1.0f + cosOmega) / 2.0f;
    const float a0 = 1.0f + alpha;
    const float a1 = -2.0f * cosOmega;
    const float a2 = 1.0f - alpha;

    // Normaliser
    const float b0n = b0 / a0;
    const float b1n = b1 / a0;
    const float b2n = b2 / a0;
    const float a1n = a1 / a0;
    const float a2n = a2 / a0;

    // Variables d'état
    float x1 = 0.0f, x2 = 0.0f;
    float y1 = 0.0f, y2 = 0.0f;

    for (size_t i = 0; i < numSamples; ++i) {
        float x0 = inputFloat[i];
        float y0 = b0n * x0 + b1n * x1 + b2n * x2 - a1n * y1 - a2n * y2;

        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;

        // Clamper
        if (y0 > 32767.0f) y0 = 32767.0f;
        if (y0 < -32768.0f) y0 = -32768.0f;

        samples[i] = static_cast<int16_t>(round(y0));
    }

    SDL_Log(">>> FIN ApplyHighPassFilter <<<");
}



// Ajout de micro-variations (jitter) - VERSION OPTIMISÉE SANS BRUIT SUR SILENCES
void ApplyJitter(vector<Uint8>& audioData, float amount)
{
    if (audioData.empty() || amount <= 0.0f) return;

    int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
    size_t numSamples = audioData.size() / sizeof(int16_t);

    SDL_Log(">>> DÉBUT ApplyJitter (%.3f) <<<", amount);

    // === CALCULER LE SEUIL DE SILENCE (5% du RMS global) ===
    float globalRMS = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        float sample = static_cast<float>(samples[i]);
        globalRMS += sample * sample;
    }
    globalRMS = sqrt(globalRMS / numSamples);

    float silenceThreshold = globalRMS * 0.05f; // Même seuil que le noise gate

    SDL_Log("  RMS global: %.1f | Seuil de silence: %.1f", globalRMS, silenceThreshold);

    // === GÉNÉRATEUR ALÉATOIRE ===
    static mt19937 rng(random_device{}());
    uniform_real_distribution<float> dist(-amount, amount);

    int samplesModified = 0;
    int samplesSilenced = 0;

    // === APPLIQUER LE JITTER UNIQUEMENT SUR LES SEGMENTS VOCAUX ===
    for (size_t i = 0; i < numSamples; ++i) {
        float sample = static_cast<float>(samples[i]);
        float absValue = fabs(sample);

        // **UNIQUEMENT** si l'échantillon est au-dessus du seuil de silence
        if (absValue > silenceThreshold) {
            // Jitter proportionnel à l'amplitude (plus fort sur signaux forts)
            float jitterAmount = dist(rng) * (absValue / 32767.0f);
            float jitteredSample = sample * (1.0f + jitterAmount);

            // Clamper
            if (jitteredSample > 32767.0f) jitteredSample = 32767.0f;
            if (jitteredSample < -32768.0f) jitteredSample = -32768.0f;

            samples[i] = static_cast<int16_t>(round(jitteredSample));
            samplesModified++;
        }
        else {
            // Laisser les silences INTACTS (pas de jitter sur le bruit de fond)
            samplesSilenced++;
        }
    }

    SDL_Log("  Échantillons modifiés: %d | Silences préservés: %d (%.1f%%)",
        samplesModified, samplesSilenced, (samplesSilenced * 100.0f) / numSamples);

    SDL_Log(">>> FIN ApplyJitter <<<");
}

// Fonction de bit-reversal pour FFT
void BitReversalPermutation(vector<complex<float>>& data)
{
    size_t n = data.size();
    size_t j = 0;

    for (size_t i = 0; i < n - 1; ++i) {
        if (i < j) {
            Swap(data[i], data[j]);  // Utilise notre fonction Swap
        }

        size_t k = n / 2;
        while (k <= j) {
            j -= k;
            k /= 2;
        }
        j += k;
    }
}

// FFT radix-2 Cooley-Tukey
void FFT_CooleyTukey(vector<complex<float>>& data)
{
    size_t n = data.size();

    if (n < 2 || (n & (n - 1)) != 0) {
        SDL_Log("ERREUR FFT: La taille doit être une puissance de 2");
        return;
    }

    BitReversalPermutation(data);

    size_t log2n = 0;
    size_t temp = n;
    while (temp > 1) {
        temp >>= 1;
        log2n++;
    }

    for (size_t s = 1; s <= log2n; ++s) {
        size_t m = 1 << s;
        size_t m2 = m / 2;

        complex<float> w(1.0f, 0.0f);
        complex<float> wm = polar(1.0f, static_cast<float>(-2.0 * numbers::pi / m));

        for (size_t j = 0; j < m2; ++j) {
            for (size_t k = j; k < n; k += m) {
                complex<float> t = w * data[k + m2];
                complex<float> u = data[k];

                data[k] = u + t;
                data[k + m2] = u - t;
            }
            w *= wm;
        }
    }
}

// Fenêtre de Hann
void ApplyHannWindow(vector<float>& data)
{
    size_t n = data.size();
    for (size_t i = 0; i < n; ++i) {
        float window = 0.5f * (1.0f - cos(2.0f * numbers::pi * i / (n - 1)));
        data[i] *= window;
    }
}

void ComputeFFT_PortAudio()
{
    // === 1. COPIE THREAD-SAFE DU BUFFER ===
    vector<float> localBuffer;
    {
        lock_guard<mutex> lock(paFftMutex);

        if (paFftInputBuffer.size() < PA_FFT_SIZE) {
            return;
        }

        localBuffer.assign(paFftInputBuffer.end() - PA_FFT_SIZE, paFftInputBuffer.end());
    }

    // === SUPPRESSION DES IMPULSIONS DANS LE DOMAINE TEMPOREL ===
    {
        // Calculer l'énergie moyenne et l'écart-type du signal
        float meanEnergy = 0.0f;
        for (size_t i = 0; i < localBuffer.size(); ++i) {
            meanEnergy += localBuffer[i] * localBuffer[i];
        }
        meanEnergy /= localBuffer.size();

        float stdDev = 0.0f;
        for (size_t i = 0; i < localBuffer.size(); ++i) {
            float diff = localBuffer[i] * localBuffer[i] - meanEnergy;
            stdDev += diff * diff;
        }
        stdDev = sqrt(stdDev / localBuffer.size());

        // Seuil de détection d'impulsion : 4× l'écart-type
        float impulseThreshold = meanEnergy + 4.0f * stdDev;

        static vector<float> previousBuffer(PA_FFT_SIZE, 0.0f);

        for (size_t i = 1; i < localBuffer.size() - 1; ++i) {
            float sampleEnergy = localBuffer[i] * localBuffer[i];

            // Si échantillon anormalement élevé
            if (sampleEnergy > impulseThreshold) {
                // Vérifier le contexte : les voisins sont-ils aussi élevés ?
                float leftEnergy = localBuffer[i - 1] * localBuffer[i - 1];
                float rightEnergy = localBuffer[i + 1] * localBuffer[i + 1];
                float avgNeighborEnergy = (leftEnergy + rightEnergy) / 2.0f;

                // Si les voisins sont faibles = impulsion isolée
                if (avgNeighborEnergy < impulseThreshold * 0.25f) {
                    // Remplacer par interpolation linéaire
                    localBuffer[i] = (localBuffer[i - 1] + localBuffer[i + 1]) / 2.0f;

                    static int impulseCounter = 0;
                    if (impulseCounter++ % 100 == 0) {
                        SDL_Log(">>> IMPULSION TEMPORELLE supprimée à i=%zu <<<", i);
                    }
                }
            }
        }

        previousBuffer = localBuffer;
    }

    // === 2. TRAITEMENT FFT (signal nettoyé) ===
    ApplyHannWindow(localBuffer);

    vector<complex<float>> complexData(PA_FFT_SIZE);
    for (size_t i = 0; i < PA_FFT_SIZE; ++i) {
        complexData[i] = complex<float>(localBuffer[i], 0.0f);
    }

    FFT_CooleyTukey(complexData);

    size_t usableBins = PA_FFT_SIZE / 2;
    float binWidth = PA_SAMPLE_RATE / PA_FFT_SIZE;

    // === 3. CALCUL DES MAGNITUDES BRUTES (80-4000 Hz) ===
    vector<float> rawMagnitudes(PA_NUM_BARS, 0.0f);

    for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
        float freqStart = 80.0f * pow(4000.0f / 80.0f, static_cast<float>(bar) / PA_NUM_BARS);
        float freqEnd = 80.0f * pow(4000.0f / 80.0f, static_cast<float>(bar + 1) / PA_NUM_BARS);

        size_t binStart = static_cast<size_t>(freqStart / binWidth);
        size_t binEnd = static_cast<size_t>(freqEnd / binWidth);
        binStart = Min(binStart, usableBins - 1);
        binEnd = Min(binEnd, usableBins);

        float sum = 0.0f;
        size_t count = 0;

        for (size_t bin = binStart; bin < binEnd; ++bin) {
            sum += abs(complexData[bin]);
            count++;
        }

        rawMagnitudes[bar] = (count > 0) ? (sum / count) : 0.0f;
    }

    // === 4. PHASE DE CALIBRATION RENFORCÉE ===
    if (paIsCalibrating) {
        for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
            paAdaptiveNoiseFloor[bar] = Max(paAdaptiveNoiseFloor[bar], rawMagnitudes[bar]);
        }

        paCalibrationFrames++;

        if (paCalibrationFrames % 10 == 0) {
            int progress = (paCalibrationFrames * 100) / PA_CALIBRATION_FRAMES;
            SDL_Log("PortAudio calibration du bruit: %d%% (%d/%d frames)",
                progress, paCalibrationFrames, PA_CALIBRATION_FRAMES);
        }

        if (paCalibrationFrames >= PA_CALIBRATION_FRAMES) {
            paIsCalibrating = false;

            for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
                float freqStart = 80.0f * pow(4000.0f / 80.0f, static_cast<float>(bar) / PA_NUM_BARS);
                float freqEnd = 80.0f * pow(4000.0f / 80.0f, static_cast<float>(bar + 1) / PA_NUM_BARS);
                float barCenterFreq = (freqStart + freqEnd) / 2.0f;

                if (barCenterFreq < 200.0f) {
                    paAdaptiveNoiseFloor[bar] *= 1.6f;
                }
                else if (barCenterFreq < 1000.0f) {
                    paAdaptiveNoiseFloor[bar] *= 1.4f;
                }
                else {
                    paAdaptiveNoiseFloor[bar] *= 1.3f;
                }
            }

            float avgNoise = 0.0f;
            float maxNoise = 0.0f;
            for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
                avgNoise += paAdaptiveNoiseFloor[bar];
                maxNoise = Max(maxNoise, paAdaptiveNoiseFloor[bar]);
            }
            avgNoise /= PA_NUM_BARS;

            SDL_Log("=== PortAudio CALIBRATION TERMINÉE (AVEC BOOST) ===");
            SDL_Log("Bruit moyen ajusté: %.4f | Bruit max: %.4f", avgNoise, maxNoise);
        }

        for (size_t i = 0; i < PA_NUM_BARS; ++i) {
            paFftMagnitudes[i] = rawMagnitudes[i];
        }

        paFftDataReady = true;
        return;
    }

    // === 5. LISSAGE TEMPOREL SIMPLE ===
    static vector<float> spectralHistory(PA_NUM_BARS, 0.0f);

    for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
        // Lissage exponentiel doux
        spectralHistory[bar] = spectralHistory[bar] * 0.85f + rawMagnitudes[bar] * 0.15f;
    }

    // === 6. SOUSTRACTION DU BRUIT CALIBRÉ ===
    for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
        float cleanMag = Max(0.0f, spectralHistory[bar] - paAdaptiveNoiseFloor[bar]);
        paFftMagnitudes[bar] = cleanMag;
    }

    // === 7. NORMALISATION ===
    float maxMagFinal = FindMax(paFftMagnitudes);

    if (maxMagFinal > 0.00001f) {
        for (size_t i = 0; i < PA_NUM_BARS; ++i) {
            paFftMagnitudes[i] = Min(1.0f, paFftMagnitudes[i] / maxMagFinal);
        }
    }

    paFftDataReady = true;

    // === LOG OCCASIONNEL ===
    static int paLogCounter = 0;
    if (paLogCounter % 120 == 0) {
        float totalEnergy = 0.0f;
        for (size_t i = 0; i < PA_NUM_BARS; ++i) {
            totalEnergy += paFftMagnitudes[i];
        }
        SDL_Log("PA FFT | Énergie: %.2f | Max: %.2f", totalEnergy, maxMagFinal);
    }
    paLogCounter++;
}

// Lissage pour PortAudio FFT
void SmoothFFTDisplay_PortAudio(float smoothingFactor)
{
    for (size_t i = 0; i < PA_NUM_BARS; ++i) {
        paFftSmoothed[i] = paFftSmoothed[i] * (1.0f - smoothingFactor) + paFftMagnitudes[i] * smoothingFactor;
    }
}



static string FormatDouble(double value, int precision) noexcept
{
    // format supporte la précision dynamique "{:.{}f}"
    try {
        return format("{:.{}f}", value, precision);
    }
    catch (...) {
        // en cas d'exception, fallback
    }
    // Fallback portable sans format
    ostringstream ss;
    ss.setf(ios::fixed);
    ss << setprecision(precision) << value;
    return ss.str();
}

static string ValueToText(double value, int precision) noexcept
{
    return FormatDouble(value, precision);
}

// Ajoute l'unité lisible en suffixe selon QuadUnit
static string FormatValueWithUnit(double value, QuadUnit unit, int precision) noexcept
{
    string s = ValueToText(value, precision);
    switch (unit) {
    case QuadUnit::Pts:     return s + " pts";
    case QuadUnit::Hz:      return s + " Hz";
    case QuadUnit::dB:      return s + " dB";
    case QuadUnit::percent: return s + " %";
    case QuadUnit::cents:   return s + " cents";
    case QuadUnit::ms:      return s + " ms";
    case QuadUnit::degrees: return s + "°";
    default:                return s;
    }
}



void getSurfaceSize(const string& text, float& surfW, float& surfH, double scale) {
    surfW = surfH = 0.0f;

    int w = 0, h = 0;
    TTF_GetStringSize(gFont, text.c_str(), text.size(), &w, &h);
    surfW = static_cast<float>(w) * scale;
    surfH = static_cast<float>(h) * scale;
}

// Utility: render text (UTF-8) using SDL_ttf. returns whether rendered and outputs width/height.
static bool RenderText(const string& text, float x, float y, SDL_Color color, float& outW, float& outH, double scale) {
    outW = outH = 0.0f;
    if (!gFont || !renderer) return false;
    SDL_Surface* surf = TTF_RenderText_Blended(gFont, text.c_str(), text.size(), color);
    if (!surf) return false;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (!tex) {
        SDL_DestroySurface(surf);
        return false;
    }
    // taille de base
    float baseW = static_cast<float>(surf->w);
    float baseH = static_cast<float>(surf->h);
    SDL_DestroySurface(surf);
    outW = baseW * scale;
    outH = baseH * scale;
    SDL_FRect dst = { x, y, outW, outH };
    SDL_RenderTexture(renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    return true;
}



void RenderWaveform(float x, float y, float width, float height)
{
    SDL_FRect background = { x, y, width, height };
    SDL_SetRenderDrawColorFloat(renderer, 0.15f, 0.15f, 0.2f, 0.9f);
    SDL_RenderFillRect(renderer, &background);

    SDL_SetRenderDrawColorFloat(renderer, 0.3f, 0.3f, 0.4f, 1.0f);
    SDL_RenderRect(renderer, &background);

    // Vérifier qu'il y a des données récentes
    if (!waveformDataReady) {
        // Afficher un message si pas de données
        if (gFont) {
            SDL_Color textColor = { 200, 100, 100, 255 };
            float tw = 0.0f, th = 0.0f;
            RenderText("No audio input detected", x + 10, y + height / 2.0f, textColor, tw, th, 0.6);
        }
        return;
    }

    // Ligne centrale (zéro)
    SDL_SetRenderDrawColorFloat(renderer, 0.4f, 0.4f, 0.5f, 0.6f);
    SDL_RenderLine(renderer, x, y + height / 2.0f, x + width, y + height / 2.0f);

    // Dessiner la forme d'onde
    vector<SDL_FPoint> wavePoints;
    wavePoints.reserve(WAVEFORM_SAMPLES);

    // Capturer la position de lecture atomiquement
    size_t readPos = waveformWritePos;
    float xStep = width / static_cast<float>(WAVEFORM_SAMPLES);

    // Calculer RMS en temps réel pour détecter l'activité
    float currentRMS = 0.0f;

    for (size_t i = 0; i < WAVEFORM_SAMPLES; ++i) {
        float sample = waveformBuffer[readPos];

        // Clamp pour éviter les débordements
        sample = Max(-1.0f, Min(1.0f, sample));

        float px = x + i * xStep;
        float py = y + height / 2.0f - (sample * height * 0.45f);

        wavePoints.push_back({ px, py });

        // Calcul RMS
        currentRMS += sample * sample;

        readPos = (readPos + 1) % WAVEFORM_SAMPLES;
    }

    currentRMS = sqrt(currentRMS / WAVEFORM_SAMPLES);

    // Dessiner la courbe avec un gradient vert
    if (wavePoints.size() >= 2) {
        for (size_t i = 1; i < wavePoints.size(); ++i) {
            float intensity = 0.5f + 0.5f * fabs(waveformBuffer[(waveformWritePos + i) % WAVEFORM_SAMPLES]);

            SDL_SetRenderDrawColorFloat(renderer,
                0.2f * intensity,
                0.8f + 0.2f * intensity,
                0.3f * intensity,
                1.0f);

            SDL_RenderLine(renderer,
                wavePoints[i - 1].x, wavePoints[i - 1].y,
                wavePoints[i].x, wavePoints[i].y);
        }
    }

    // Calcul du Peak pour affichage
    float peak = 0.0f;
    for (size_t i = 0; i < WAVEFORM_SAMPLES; ++i) {
        peak = Max(peak, fabs(waveformBuffer[i]));
    }

    // Afficher les stats
    if (gFont) {
        SDL_Color textColor = { 200, 255, 200, 255 };
        float tw = 0.0f, th = 0.0f;

        string label = "Audio Waveform (Oscilloscope)";
        RenderText(label, x + 10, y + 5, textColor, tw, th, 0.6);

        // RMS et Peak avec indication d'activité
        SDL_Color statsColor = (currentRMS > 0.01f) ? SDL_Color{ 100, 255, 100, 255 } : SDL_Color{ 200, 200, 200, 255 };
        string stats = "RMS: " + FormatDouble(currentRMS * 100.0, 2) + "% | Peak: " + FormatDouble(peak * 100.0, 1) + "%";
        RenderText(stats, x + 10, y + height - 25, statsColor, tw, th, 0.5);
    }
}

// === FONCTION DE RENDU FFT PORTAUDIO ===
void RenderFFTSpectrum_PortAudio(float x, float y, float width, float height)
{
    SDL_FRect background = { x, y, width, height };
    SDL_SetRenderDrawColorFloat(renderer, 0.1f, 0.1f, 0.15f, 0.8f);
    SDL_RenderFillRect(renderer, &background);

    SDL_SetRenderDrawColorFloat(renderer, 0.3f, 0.3f, 0.4f, 1.0f);
    SDL_RenderRect(renderer, &background);

    // Créer un tableau de points pour la courbe
    vector<SDL_FPoint> curvePoints;
    curvePoints.reserve(PA_NUM_BARS + 2);

    float barWidth = width / PA_NUM_BARS;

    // Point de départ au bas à gauche
    curvePoints.push_back({ x, y + height });

    // Générer les points de la courbe
    for (size_t i = 0; i < PA_NUM_BARS; ++i) {
        float magnitude = paFftSmoothed[i];
        float barHeight = magnitude * height * 0.9f;

        float pointX = x + (i + 0.5f) * barWidth;
        float pointY = y + height - barHeight;

        curvePoints.push_back({ pointX, pointY });
    }

    // Point de fin au bas à droite
    curvePoints.push_back({ x + width, y + height });

    // === DESSINER LE REMPLISSAGE SOUS LA COURBE ===
    if (curvePoints.size() >= 3) {
        vector<SDL_Vertex> fillVertices;
        fillVertices.reserve(curvePoints.size() * 2);

        for (size_t i = 0; i < curvePoints.size(); ++i) {
            float hue = static_cast<float>(i) / curvePoints.size();
            float intensity = (curvePoints[i].y < y + height) ?
                1.0f - (curvePoints[i].y - y) / height : 0.0f;

            float r = Max(0.0f, Min(1.0f, (hue - 0.5f) * 2.0f + intensity * 0.3f));
            float g = Max(0.0f, Min(1.0f, 1.0f - fabs(hue - 0.5f) * 2.0f + intensity * 0.2f));
            float b = Max(0.0f, Min(1.0f, (0.5f - hue) * 2.0f + intensity * 0.1f));

            SDL_Vertex topVertex;
            topVertex.position = curvePoints[i];
            topVertex.color = { r, g, b, 0.6f };

            SDL_Vertex bottomVertex;
            bottomVertex.position = { curvePoints[i].x, y + height };
            bottomVertex.color = { r * 0.3f, g * 0.3f, b * 0.3f, 0.2f };

            fillVertices.push_back(topVertex);
            fillVertices.push_back(bottomVertex);
        }

        for (size_t i = 0; i < fillVertices.size() - 2; i += 2) {
            SDL_Vertex triangle[3] = {
                fillVertices[i],
                fillVertices[i + 1],
                fillVertices[i + 2]
            };
            SDL_RenderGeometry(renderer, nullptr, triangle, 3, nullptr, 0);

            if (i + 3 < fillVertices.size()) {
                SDL_Vertex triangle2[3] = {
                    fillVertices[i + 1],
                    fillVertices[i + 2],
                    fillVertices[i + 3]
                };
                SDL_RenderGeometry(renderer, nullptr, triangle2, 3, nullptr, 0);
            }
        }
    }

    // === DESSINER LA COURBE ===
    if (curvePoints.size() >= 2) {
        for (size_t i = 1; i < curvePoints.size(); ++i) {
            float hue = static_cast<float>(i) / curvePoints.size();

            float r = Max(0.0f, Min(1.0f, (hue - 0.5f) * 2.0f + 0.5f));
            float g = Max(0.0f, Min(1.0f, 1.0f - fabs(hue - 0.5f) * 2.0f + 0.3f));
            float b = Max(0.0f, Min(1.0f, (0.5f - hue) * 2.0f + 0.2f));

            SDL_SetRenderDrawColorFloat(renderer, r, g, b, 1.0f);

            for (int offset = -1; offset <= 1; ++offset) {
                SDL_RenderLine(renderer,
                    curvePoints[i - 1].x, curvePoints[i - 1].y + offset,
                    curvePoints[i].x, curvePoints[i].y + offset);
            }
        }
    }

    // === LABEL ===
    if (gFont) {
        SDL_Color textColor = { 200, 200, 255, 255 };
        float tw = 0.0f, th = 0.0f;

        string label;
        if (paIsCalibrating) {
            int progress = (paCalibrationFrames * 100) / PA_CALIBRATION_FRAMES;
            label = "PortAudio - Calibration du bruit: " + to_string(progress) + "%";
        }
        else {
            label = "PortAudio Vocal Spectrum (80 Hz - 4 kHz @ 16 kHz) - Bruit calibré";
        }

        RenderText(label, x + 10, y + 5, textColor, tw, th, 0.6);
    }
}

static double RandNb_0_to_1()
{
    static mt19937_64 rng(random_device{}()); // seed non déterministe
    static uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng);
}



// Fonction pour sauvegarder un buffer audio au format WAV (MONO ou STÉRÉO automatique)
bool SaveWAV_Adaptive(const string& filename, const vector<Uint8>& audioData, int numChannels, float sampleRate = PA_SAMPLE_RATE)
{
    if (audioData.empty()) {
        SDL_Log("ERREUR SaveWAV_Adaptive: Buffer audio vide");
        return false;
    }

    SDL_Log("=== DÉBUT SaveWAV_Adaptive ===");
    SDL_Log("  Fichier: %s", filename.c_str());
    SDL_Log("  Taille buffer: %zu octets", audioData.size());
    SDL_Log("  Format: %s (canaux: %d)", numChannels == 2 ? "STÉRÉO" : "MONO", numChannels);
    SDL_Log("  Sample rate: %.0f Hz", sampleRate);

    // === SDL3 retourne const char* et gère la mémoire en interne ===
    const char* basePath = SDL_GetBasePath();
    if (!basePath) {
        SDL_Log("ERREUR: Impossible d'obtenir SDL_GetBasePath()");
        return false;
    }

    // Convertir IMMÉDIATEMENT en string (copie la chaîne)
    string projectPath(basePath);

    // Remonter de 2 niveaux : x64/Debug/ -> x64/ -> racine du projet
    size_t lastSlash = projectPath.find_last_of("\\/", projectPath.length() - 2);
    if (lastSlash != string::npos) {
        projectPath = projectPath.substr(0, lastSlash + 1);
    }

    // Supprimer le dossier de configuration (x64 ou Win32)
    lastSlash = projectPath.find_last_of("\\/", projectPath.length() - 2);
    if (lastSlash != string::npos) {
        projectPath = projectPath.substr(0, lastSlash + 1);
    }

    SDL_Log("  Chemin du projet détecté: %s", projectPath.c_str());

    // Créer le dossier "recordings" dans le projet
    string folderPath = projectPath + "recordings";

#ifdef _WIN32
    int result = _mkdir(folderPath.c_str());
#else
    int result = mkdir(folderPath.c_str(), 0755);
#endif

    if (result == 0) {
        SDL_Log("  Dossier 'recordings' créé: %s", folderPath.c_str());
    }
    else {
        SDL_Log("  Dossier 'recordings' existant ou créé: %s", folderPath.c_str());
    }

    // Construire le chemin complet
    string fullPath = folderPath + "/" + filename;

    // === Utiliser fopen_s au lieu de fopen ===
    FILE* file = nullptr;
    errno_t err = fopen_s(&file, fullPath.c_str(), "wb");
    if (err != 0 || !file) {
        SDL_Log("ERREUR: Impossible de créer le fichier %s (code erreur: %d)", fullPath.c_str(), err);
        return false;
    }

    // Remplir l'en-tête WAV
    WAVHeader header;

    // Chunk RIFF
    memcpy(header.riffID, "RIFF", 4);
    header.fileSize = static_cast<uint32_t>(audioData.size() + sizeof(WAVHeader) - 8);
    memcpy(header.riffType, "WAVE", 4);

    // Chunk fmt
    memcpy(header.fmtID, "fmt ", 4);
    header.fmtSize = 16;
    header.audioFormat = 1;
    header.numChannels = numChannels;
    header.sampleRate = static_cast<uint32_t>(sampleRate);
    header.bitsPerSample = 16;
    header.byteRate = header.sampleRate * header.numChannels * header.bitsPerSample / 8;
    header.blockAlign = header.numChannels * header.bitsPerSample / 8;

    // Chunk data
    memcpy(header.dataID, "data", 4);
    header.dataSize = static_cast<uint32_t>(audioData.size());

    // Écrire l'en-tête
    size_t written = fwrite(&header, 1, sizeof(WAVHeader), file);
    if (written != sizeof(WAVHeader)) {
        SDL_Log("ERREUR: Échec écriture en-tête WAV");
        fclose(file);
        return false;
    }

    // Écrire les données audio
    written = fwrite(audioData.data(), 1, audioData.size(), file);
    if (written != audioData.size()) {
        SDL_Log("ERREUR: Échec écriture données audio");
        fclose(file);
        return false;
    }

    fclose(file);

    // Vérifications finales
    size_t totalFileSize = sizeof(WAVHeader) + audioData.size();
    size_t totalSamples = audioData.size() / sizeof(int16_t);
    float durationSeconds = static_cast<float>(totalSamples / numChannels) / sampleRate;

    SDL_Log(">>> FICHIER WAV SAUVEGARDÉ AVEC SUCCÈS <<<");
    SDL_Log("  Chemin: %s", fullPath.c_str());
    SDL_Log("  Taille totale: %zu octets (%.2f KB)", totalFileSize, totalFileSize / 1024.0f);
    SDL_Log("  Samples totaux: %zu (%zu par canal)", totalSamples, totalSamples / numChannels);
    SDL_Log("  Durée: %.2f secondes", durationSeconds);
    SDL_Log("  Format: PCM 16-bit %s @ %.0f Hz",
        numChannels == 2 ? "STÉRÉO" : "MONO", sampleRate);
    SDL_Log("=== FIN SaveWAV_Adaptive ===");

    return true;
}

// Fonction pour générer un nom de fichier unique avec timestamp
string GenerateWAVFilename(const string& prefix)
{
    time_t now = time(nullptr);

    // === Utiliser localtime_s au lieu de localtime ===
    tm timeinfo;
    errno_t err = localtime_s(&timeinfo, &now);
    if (err != 0) {
        SDL_Log("ERREUR: Échec de localtime_s");
        return prefix + "_error.wav";
    }

    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &timeinfo);

    return prefix + "_" + string(buffer) + ".wav";
}



// ============================================================================
// PITCH SHIFTER OPTIMISÉ POUR LATENCE 20MS (DÉFINITION AVANT UTILISATION)
// ============================================================================

class StreamPitchShifter {
private:
    static constexpr size_t DELAY_SIZE = 160;    // 10ms @ 16kHz
    static constexpr size_t XFADE_SIZE = 48;     // 3ms crossfade

    array<float, DELAY_SIZE> delayLine;
    array<float, XFADE_SIZE> xfadeBuffer;

    size_t writePos = 0;
    float readPos = 0.0f;
    size_t xfadePhase = 0;
    bool isCrossfading = false;
    float currentRatio = 1.0f;

public:
    StreamPitchShifter() {
        delayLine.fill(0.0f);
        xfadeBuffer.fill(0.0f);
        readPos = DELAY_SIZE / 2.0f;
    }

    inline float ProcessSample(float input, float semitones) {
        // Écriture dans le delay
        delayLine[writePos] = input;
        writePos = (writePos + 1) % DELAY_SIZE;

        // Calcul du ratio cible
        float targetRatio = pow(2.0f, semitones / 12.0f);
        currentRatio = 0.99f * currentRatio + 0.01f * targetRatio;

        float output;

        if (!isCrossfading) {
            // Lecture normale avec interpolation linéaire
            size_t idx = static_cast<size_t>(readPos);
            float frac = readPos - idx;

            float s0 = delayLine[idx % DELAY_SIZE];
            float s1 = delayLine[(idx + 1) % DELAY_SIZE];
            output = s0 + frac * (s1 - s0);

            // Avancement de la tête de lecture
            readPos += currentRatio;
            if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;

            // Détection de collision avec tête d'écriture
            float distance = static_cast<float>(writePos) - readPos;
            if (distance < 0.0f) distance += DELAY_SIZE;

            if (distance < XFADE_SIZE + 10.0f || distance > DELAY_SIZE - XFADE_SIZE - 10.0f) {
                // Démarrer crossfade
                isCrossfading = true;
                xfadePhase = 0;

                // Capturer le segment actuel
                for (size_t i = 0; i < XFADE_SIZE; ++i) {
                    float pos = readPos + i * currentRatio;
                    if (pos >= DELAY_SIZE) pos -= DELAY_SIZE;
                    size_t idx = static_cast<size_t>(pos);
                    xfadeBuffer[i] = delayLine[idx % DELAY_SIZE];
                }

                // Repositionner au centre
                readPos = static_cast<float>(writePos) - (DELAY_SIZE / 2.0f);
                if (readPos < 0.0f) readPos += DELAY_SIZE;
            }
        }
        else {
            // Mode crossfade avec fenêtre de Hann
            if (xfadePhase < XFADE_SIZE) {
                float t = static_cast<float>(xfadePhase) / (XFADE_SIZE - 1);
                float fadeOut = 0.5f * (1.0f + cosf(3.14159265f * t));
                float fadeIn = 1.0f - fadeOut;

                size_t idx = static_cast<size_t>(readPos);
                float newSample = delayLine[idx % DELAY_SIZE];

                output = xfadeBuffer[xfadePhase] * fadeOut + newSample * fadeIn;

                readPos += currentRatio;
                if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;

                xfadePhase++;
            }
            else {
                isCrossfading = false;

                size_t idx = static_cast<size_t>(readPos);
                output = delayLine[idx % DELAY_SIZE];

                readPos += currentRatio;
                if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;
            }
        }

        return output;
    }

    void ProcessBuffer(float* buffer, size_t numSamples, float semitones) {
        for (size_t i = 0; i < numSamples; ++i) {
            buffer[i] = ProcessSample(buffer[i], semitones);
        }
    }
};

// ============================================================================
// FORMANT SHIFTER OPTIMISÉ POUR LATENCE 20MS (DÉFINITION AVANT UTILISATION)
// ============================================================================

class StreamFormantShifter {
private:
    static constexpr size_t DELAY_SIZE = 256;    //512, Buffer circulaire (32ms @ 16kHz)
    static constexpr size_t XFADE_SIZE = 96;     // Crossfade 6ms

    array<float, DELAY_SIZE> delayLine;
    array<float, XFADE_SIZE> xfadeBuffer;

    size_t writePos = 0;
    float readPos = 0.0f;
    size_t xfadePhase = 0;
    bool isCrossfading = false;
    float currentRatio = 1.0f;

public:
    StreamFormantShifter() {
        delayLine.fill(0.0f);
        xfadeBuffer.fill(0.0f);
        readPos = DELAY_SIZE / 2.0f;

        SDL_Log("=== StreamFormantShifter MODE SAMPLE-PAR-SAMPLE ===");
        SDL_Log("  Delay: %zu samples (%.1f ms)", DELAY_SIZE, (DELAY_SIZE * 1000.0f) / 16000.0f);
        SDL_Log("  Architecture: IDENTIQUE au Pitch Shifter");
    }

    // === INTERPOLATION HERMITE (COPIE DU PITCH SHIFTER) ===
    inline float ReadDelayInterpolated(float position) const {
        while (position < 0.0f) position += DELAY_SIZE;
        while (position >= DELAY_SIZE) position -= DELAY_SIZE;

        size_t idx = static_cast<size_t>(position);
        float frac = position - idx;

        size_t im1 = (idx + DELAY_SIZE - 1) % DELAY_SIZE;
        size_t i0 = idx;
        size_t i1 = (idx + 1) % DELAY_SIZE;
        size_t i2 = (idx + 2) % DELAY_SIZE;

        float ym1 = delayLine[im1];
        float y0 = delayLine[i0];
        float y1 = delayLine[i1];
        float y2 = delayLine[i2];

        float c0 = y0;
        float c1 = 0.5f * (y1 - ym1);
        float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

        return c0 + c1 * frac + c2 * frac * frac + c3 * frac * frac * frac;
    }

    // === PROCESSEUR SAMPLE-PAR-SAMPLE (ARCHITECTURE IDENTIQUE AU PITCH) ===
    float ProcessSample(float input, float shiftRatio) {
        // 1) Écriture
        delayLine[writePos] = input;
        writePos = (writePos + 1) % DELAY_SIZE;

        // 2) Mise à jour ratio
        currentRatio = 0.995f * currentRatio + 0.005f * shiftRatio;

        // 3) Lecture
        float output;

        if (!isCrossfading) {
            // Lecture normale
            output = ReadDelayInterpolated(readPos);

            // Avancement
            readPos += currentRatio;
            if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;

            // Détection collision
            float distance = static_cast<float>(writePos) - readPos;
            if (distance < 0.0f) distance += DELAY_SIZE;

            if ((distance < XFADE_SIZE + 24.0f ||
                distance > DELAY_SIZE - XFADE_SIZE - 24.0f)) {

                // Démarrer crossfade
                isCrossfading = true;
                xfadePhase = 0;

                // Capturer segment actuel
                for (size_t i = 0; i < XFADE_SIZE; ++i) {
                    float pos = readPos + i * currentRatio;
                    if (pos >= DELAY_SIZE) pos -= DELAY_SIZE;
                    xfadeBuffer[i] = ReadDelayInterpolated(pos);
                }

                // Repositionner au centre
                readPos = static_cast<float>(writePos) - (DELAY_SIZE / 2.0f);
                if (readPos < 0.0f) readPos += DELAY_SIZE;
            }
        }
        else {
            // Mode crossfade
            if (xfadePhase < XFADE_SIZE) {
                float t = static_cast<float>(xfadePhase) / (XFADE_SIZE - 1);
                float fadeOut = 0.5f * (1.0f + cosf(3.14159265f * t));
                float fadeIn = 1.0f - fadeOut;

                float oldSample = xfadeBuffer[xfadePhase];
                float newSample = ReadDelayInterpolated(readPos);

                output = oldSample * fadeOut + newSample * fadeIn;

                readPos += currentRatio;
                if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;

                xfadePhase++;
            }
            else {
                isCrossfading = false;
                output = ReadDelayInterpolated(readPos);
                readPos += currentRatio;
                if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;
            }
        }

        return output;
    }

    // === MAINTENIR LA COMPATIBILITÉ AVEC LE MODE PLAYBACK ===
    void ProcessFrame(const float* input, float* output, size_t numSamples, float shiftRatio) {
        for (size_t i = 0; i < numSamples; ++i) {
            output[i] = ProcessSample(input[i], shiftRatio);
        }
    }
};

// ============================================================================
// MAINTENANT ON PEUT DÉCLARER LES VARIABLES GLOBALES
// ============================================================================

// === ÉTATS GLOBAUX THREAD-SAFE ===
atomic<bool> g_streamModeActive{ false };  // Flag mode stream

// Instances des processeurs (créées à la demande)
static StreamPitchShifter* g_streamPitchShifter = nullptr;
static StreamFormantShifter* g_streamFormantShifter = nullptr;

// === BUFFERS PRÉ-ALLOUÉS (aucune allocation dans callback) ===
struct StreamBuffers {
    array<float, STREAM_BUFFER_SIZE> tempPitch;
    array<float, STREAM_BUFFER_SIZE> tempFormant;
    array<float, STREAM_BUFFER_SIZE> output;

    StreamBuffers() {
        tempPitch.fill(0.0f);
        tempFormant.fill(0.0f);
        output.fill(0.0f);
    }
};

static StreamBuffers* g_streamBuffers = nullptr;

// Stream PortAudio dédié au mode stream
static PaStream* paStreamDuplex = nullptr;

// ============================================================================
// CALLBACK PORTAUDIO UNIFIÉ AVEC MESURE DE LATENCE
// ============================================================================

static int paUnifiedCallback(
    const void* inputBuffer,
    void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    const float* in = static_cast<const float*>(inputBuffer);
    float* out = static_cast<float*>(outputBuffer);

    if (in == nullptr) {
        if (out != nullptr) {
            memset(out, 0, framesPerBuffer * paMicrophoneChannels * sizeof(float));
        }
        return paContinue;
    }

    // === TRAITEMENT COMMUN (NETTOYAGE AUDIO) ===
    static float hp_x1 = 0.0f, hp_y1 = 0.0f;
    constexpr float hp_alpha = 0.950f;

    static float rms_squared = 0.0f;
    constexpr float rms_alpha = 0.8825f;

    static float noise_floor = 0.0f;
    static float noise_smoothing = 0.9995f;
    static bool noise_initialized = false;
    static int calibration_counter = 0;

    constexpr float expander_threshold = 0.015f;
    constexpr float expander_ratio = 2.0f;
    constexpr float expander_attack = 0.001f;
    constexpr float expander_release = 0.050f;
    static float expander_envelope = 1.0f;

    // Calibration initiale
    if (!noise_initialized && calibration_counter < 30) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            float sample = fabs(in[i]);
            noise_floor = max(noise_floor, sample);
        }

        calibration_counter++;

        if (calibration_counter == 30) {
            noise_floor *= 1.8f;
            noise_initialized = true;
        }
    }

    // Buffer temporaire pour échantillons nettoyés
    vector<float> cleanedSamples(framesPerBuffer);

    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        float sample = in[i * paMicrophoneChannels];

        // Filtre HP
        float hp_output = hp_alpha * (hp_y1 + sample - hp_x1);
        hp_x1 = sample;
        hp_y1 = hp_output;
        sample = hp_output;

        // RMS
        rms_squared = rms_alpha * rms_squared + (1.0f - rms_alpha) * (sample * sample);
        float rms_current = sqrt(rms_squared);

        // Noise floor adaptatif
        if (noise_initialized && rms_current < noise_floor * 0.2f) {
            noise_floor = noise_smoothing * noise_floor + (1.0f - noise_smoothing) * rms_current;
        }

        // Noise gate
        float gate_threshold = noise_floor * 4.0f;
        float abs_sample = fabs(sample);

        if (abs_sample < gate_threshold) {
            float ratio = abs_sample / gate_threshold;
            float attenuation = (exp(ratio * 2.0f) - 1.0f) / (exp(2.0f) - 1.0f);
            sample *= attenuation;
        }

        // Expander
        if (rms_current < expander_threshold) {
            float target_gain = 1.0f - ((expander_threshold - rms_current) / expander_threshold) * (1.0f - 1.0f / expander_ratio);

            float attack_coeff = exp(-1.0f / (expander_attack * PA_SAMPLE_RATE));
            float release_coeff = exp(-1.0f / (expander_release * PA_SAMPLE_RATE));

            if (target_gain < expander_envelope) {
                expander_envelope = attack_coeff * expander_envelope + (1.0f - attack_coeff) * target_gain;
            }
            else {
                expander_envelope = release_coeff * expander_envelope + (1.0f - release_coeff) * target_gain;
            }

            sample *= expander_envelope;
        }
        else {
            expander_envelope = 1.0f;
        }

        cleanedSamples[i] = sample;
    }

    // === ROUTING SELON LE MODE ===
    if (g_streamModeActive.load(memory_order_acquire)) {
        // ----------------------------------------------------------
        // MODE STREAM TEMPS RÉEL AVEC MESURE DE LATENCE
        // ----------------------------------------------------------

        if (!g_streamBuffers || !g_streamPitchShifter || !g_streamFormantShifter) {
            if (out != nullptr) {
                memset(out, 0, framesPerBuffer * paMicrophoneChannels * sizeof(float));
            }
            return paContinue;
        }

        // === TIMESTAMP T1 : AUDIO ARRIVE DU MICRO ===
        auto t1_capture = chrono::high_resolution_clock::now();

        // Copier dans buffer temporaire
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            g_streamBuffers->tempPitch[i] = cleanedSamples[i];
        }

        // === TIMESTAMP T2 : PRÊT POUR DSP (après filtre HP 80Hz déjà appliqué) ===
        auto t2_readyDSP = chrono::high_resolution_clock::now();

        // === TRAITEMENT DSP ===
        g_streamPitchShifter->ProcessBuffer(
            g_streamBuffers->tempPitch.data(),
            framesPerBuffer,
            g_pitchShiftSemitones.load(memory_order_acquire)
        );

        g_streamFormantShifter->ProcessFrame(
            g_streamBuffers->tempPitch.data(),
            g_streamBuffers->tempFormant.data(),
            framesPerBuffer,
            g_formantShiftRatio.load(memory_order_acquire)
        );

        // Appliquer jitter
        float jitter = g_jitterAmount.load(memory_order_acquire);
        static mt19937 rngLatency(random_device{}());

        if (jitter > 0.0f) {
            uniform_real_distribution<float> distLatency(-jitter, jitter);
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                g_streamBuffers->tempFormant[i] *= (1.0f + distLatency(rngLatency));
            }
        }

        // Appliquer le volume de sortie
        float volume = g_outputVolume.load(memory_order_acquire);
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            g_streamBuffers->tempFormant[i] *= volume;
        }

        // === TIMESTAMP T3 : PRÊT À ÊTRE ENVOYÉ ===
        auto t3_readyOutput = chrono::high_resolution_clock::now();

        // Copier vers le buffer de sortie
        if (out != nullptr) {
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                float sample = g_streamBuffers->tempFormant[i];

                for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                    out[i * paMicrophoneChannels + ch] = sample;
                }
            }
        }

        // === TIMESTAMP T4 : ENVOYÉ AU DRIVER (juste avant return) ===
        auto t4_sent = chrono::high_resolution_clock::now();

        // Enregistrer les timestamps pour ce frame
        if (g_latencyMetrics) {
            g_latencyMetrics->t1_microphoneCapture = t1_capture;
            g_latencyMetrics->t2_readyForDSP = t2_readyDSP;
            g_latencyMetrics->t3_readyForOutput = t3_readyOutput;
            g_latencyMetrics->t4_sentToSpeaker = t4_sent;
            g_latencyMetrics->recordSample();
        }

    }
    else {
        // ----------------------------------------------------------
        // MODE ENREGISTREMENT
        // ----------------------------------------------------------

        // Remplir waveform
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            waveformBuffer[waveformWritePos] = cleanedSamples[i];
            waveformWritePos = (waveformWritePos + 1) % WAVEFORM_SAMPLES;
        }

        // Remplir FFT
        {
            lock_guard<mutex> lock(paFftMutex);
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                paFftInputBuffer.push_back(cleanedSamples[i]);
            }
            while (paFftInputBuffer.size() > PA_FFT_SIZE * 2) {
                paFftInputBuffer.erase(paFftInputBuffer.begin(),
                    paFftInputBuffer.begin() + (paFftInputBuffer.size() - PA_FFT_SIZE));
            }
        }

        // Enregistrement
        if (paIsRecording) {
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                float sample = cleanedSamples[i];
                sample = max(-1.0f, min(1.0f, sample));
                int16_t sample16 = static_cast<int16_t>(sample * 32767.0f);
                paAudioBuffer.push_back(sample16 & 0xFF);
                paAudioBuffer.push_back((sample16 >> 8) & 0xFF);
            }
        }

        // Pas de sortie en mode enregistrement
        if (out != nullptr) {
            memset(out, 0, framesPerBuffer * paMicrophoneChannels * sizeof(float));
        }
    }

    waveformDataReady = true;
    return paContinue;
}

// ============================================================================
// FONCTIONS DE CONTRÔLE MODE STREAM
// ============================================================================

bool InitStreamMode() {
    SDL_Log("=== Initialisation Mode Stream (latence 20ms) ===");

    // Allouer les processeurs si nécessaire
    if (!g_streamPitchShifter) {
        g_streamPitchShifter = new StreamPitchShifter();
        SDL_Log("  StreamPitchShifter créé (delay: 10ms)");
    }

    if (!g_streamFormantShifter) {
        g_streamFormantShifter = new StreamFormantShifter();
        SDL_Log("  StreamFormantShifter créé (window: 16ms)");
    }

    // === Allouer dynamiquement StreamBuffers ===
    if (!g_streamBuffers) {
        g_streamBuffers = new StreamBuffers();
        SDL_Log("  Buffers stream alloués (320 échantillons)");
    }

    SDL_Log(">>> Mode stream initialisé avec succès <<<");
    SDL_Log("  Latence théorique: ~20ms");
    SDL_Log("  Qualité: Optimale (fenêtres 256 échantillons)");

    return true;
}

void ActivateStreamMode() {
    if (!g_streamPitchShifter || !g_streamFormantShifter || !g_streamBuffers) {
        if (!InitStreamMode()) {
            SDL_Log("ERREUR: Échec initialisation mode stream");
            return;
        }
    }

    g_streamModeActive.store(true, memory_order_release);
    SDL_Log(">>> MODE STREAM ACTIVÉ <<<");
}

void DeactivateStreamMode() {
    g_streamModeActive.store(false, memory_order_release);
    SDL_Log(">>> MODE STREAM DÉSACTIVÉ <<<");
}

void CleanupStreamMode() {
    if (g_streamPitchShifter) {
        delete g_streamPitchShifter;
        g_streamPitchShifter = nullptr;
    }

    if (g_streamFormantShifter) {
        delete g_streamFormantShifter;
        g_streamFormantShifter = nullptr;
    }

    if (g_streamBuffers) {
        delete g_streamBuffers;
        g_streamBuffers = nullptr;
    }
}

// ============================================================================
// GESTION DU BOUTON MODE
// ============================================================================

void OnModeButtonClick() {
    if (Quadrs("AudioMode").getText() == "Mode Actif : Enregistrement") {
        // Basculer vers mode stream
        Quadrs("AudioMode").setText("Mode Actif : Stream");
        Quadrs("AudioMode").setQuadrColorRGB(0.3f, 1.0f, 0.3f);

        // Activer le flag atomique
        g_streamModeActive.store(true, memory_order_release);

        // Initialiser le stream si pas déjà fait
        if (!g_streamPitchShifter) {
            InitStreamMode();
        }

        SDL_Log(">>> MODE STREAM ACTIVÉ <<<");

    }
    else {
        // Retour au mode enregistrement
        Quadrs("AudioMode").setText("Mode Actif : Enregistrement");
        Quadrs("AudioMode").setQuadrColorRGB(0.8f, 0.8f, 0.8f);

        g_streamModeActive.store(false, memory_order_release);

        SDL_Log(">>> MODE ENREGISTREMENT ACTIVÉ <<<");
    }
}



/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    // === INITIALISER LES VARIABLES GLOBALES QUI DÉPENDENT DE SDL ===
    pxFormatRGBA64 = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA64);
    color_grey = SDL_MapRGBA(pxFormatRGBA64, NULL, grey_r, grey_g, grey_b, grey_a);

    // === INITIALISER LES VECTEURS AVEC LEURS TAILLES ===
    waveformBuffer.resize(WAVEFORM_SAMPLES, 0.0f);
    paFftMagnitudes.resize(PA_NUM_BARS, 0.0f);
    paFftSmoothed.resize(PA_NUM_BARS, 0.0f);
    paAdaptiveNoiseFloor.resize(PA_NUM_BARS, 0.0f);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // === INITIALISER PORTAUDIO ===
    if (!InitPortAudio()) {
        SDL_Log("ATTENTION: Échec de l'initialisation PortAudio");
    }

    // === INITIALISER LE SYSTÈME DE MESURE DE LATENCE ===
    g_latencyMetrics = new LatencyMetrics();
    SDL_Log("=== Système de mesure de latence initialisé ===");

    if (!SDL_CreateWindowAndRenderer("Anonymix Scoobs", 1595, 1040, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // Charger un logo BMP et l'utiliser comme icône de fenêtre
    SDL_Surface* logoSurface = SDL_LoadBMP("logo.bmp");
    if (!logoSurface) {
        SDL_Log("Impossible de charger le logo.bmp: %s", SDL_GetError());
    }
    else {
        SDL_SetWindowIcon(window, logoSurface);
        SDL_DestroySurface(logoSurface);
        SDL_Log("Icône de fenêtre définie à partir de logo.bmp");
    }

    // Ne plus utiliser de texture/rectangle pour le logo dans le rendu
    gLogoTexture = nullptr;
    gLogoRect = { 0.f, 0.f, 0.f, 0.f }; SDL_DestroySurface(logoSurface);

    // Initialize SDL_ttf
    if (TTF_Init() != 0) {
        SDL_Log("TTF_Init failed: %s", SDL_GetError());
        gFont = nullptr;
    }
    else {
        // essayer une police système ; adapte le chemin si nécessaire
        gFont = TTF_OpenFont("C:/Windows/Fonts/Arial.ttf", 14);
        if (!gFont) gFont = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14);
        if (!gFont) SDL_Log("Warning: could not open a font. Text labels will be skipped.");
    }

    gFont = TTF_OpenFont("C:/Windows/Fonts/Arial.ttf", 32);

    SDL_GetMouseState(&mx, &my);

    mousePtrOnQuadr = nullptr;

    Quadrs.add("Quadr1");
    Quadrs("Quadr1").setType(QuadType::Static_DynaZoom);
    Quadrs("Quadr1").setHalfDiag1(50);
    Quadrs("Quadr1").setHalfDiag2(50);
    Quadrs("Quadr1").setGx(200);
    Quadrs("Quadr1").setGy(240);
    Quadrs("Quadr1").setQuadrColorRGB(0.0, 0.0, 0.0);

// === CONSTANTES DE LAYOUT (calculées pour éviter collisions) ===
    constexpr float MARGIN_H = 30.0f;   // Marge horizontale entre boutons
    constexpr float MARGIN_V = 25.0f;   // Marge verticale entre lignes
    constexpr float ZOOM_SAFE = 1.20f;  // Facteur de sécurité (1.15 * 1.04)

    // ========================================================================
    // ZONE 1 : CONTRÔLES PRINCIPAUX
    // ========================================================================

    // BOUTON 1.1 : Mode Toggle
    Quadrs.add("AudioMode");
    Quadrs("AudioMode").setType(QuadType::Button_StateMono);
    Quadrs("AudioMode").setButtonFunction(QuadButtonEvent::AudioMode_Toggle);
    Quadrs("AudioMode").setQuadrColorRGB(0.8f, 0.8f, 0.8f);
    Quadrs("AudioMode").setRectangleShape(210, 40);
    Quadrs("AudioMode").setGx(1140);
    Quadrs("AudioMode").setGy(65);
    Quadrs("AudioMode").setText("Mode Actif : Enregistrement");

    // BOUTON 1.2 : Start Recording
    Quadrs.add("PA_Record");
    Quadrs("PA_Record").setType(QuadType::Button_StateMono);
    Quadrs("PA_Record").setButtonFunction(QuadButtonEvent::PA_AudioRecord_Start);
    Quadrs("PA_Record").setQuadrColorRGB(1.0, 0.55, 0.55);
    Quadrs("PA_Record").setRectangleShape(100, 48);
    Quadrs("PA_Record").setGx(790);
    Quadrs("PA_Record").setGy(190);
    Quadrs("PA_Record").setText("Start Rec");

    // BOUTON 1.3 : Play Original
    Quadrs.add("PA_Play");
    Quadrs("PA_Play").setType(QuadType::Button_StateMono);
    Quadrs("PA_Play").setButtonFunction(QuadButtonEvent::PA_AudioPlay_Start);
    Quadrs("PA_Play").setQuadrColorRGB(1.0, 1.0, 0.55);
    Quadrs("PA_Play").setRectangleShape(90, 35);
    Quadrs("PA_Play").setGx(765);
    Quadrs("PA_Play").setGy(310);
    Quadrs("PA_Play").setText("Play OG");

    // ========================================================================
    // ZONE 2 : LECTURE ANONYMISÉE
    // ========================================================================

    // BOUTON 2.1 : Play Pitch+Formant
    Quadrs.add("PA_PlayAnonPitchFormant");
    Quadrs("PA_PlayAnonPitchFormant").setType(QuadType::Button_StateMono);
    Quadrs("PA_PlayAnonPitchFormant").setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedPitchFormant_Start);
    Quadrs("PA_PlayAnonPitchFormant").setQuadrColorRGB(1.0, 0.55, 1.0);
    Quadrs("PA_PlayAnonPitchFormant").setRectangleShape(90, 35);
    Quadrs("PA_PlayAnonPitchFormant").setGx(985);
    Quadrs("PA_PlayAnonPitchFormant").setGy(310);
    Quadrs("PA_PlayAnonPitchFormant").setText("Play P+F");

    // BOUTON 2.2 : Play Pitch
    Quadrs.add("PA_PlayAnonPitch");
    Quadrs("PA_PlayAnonPitch").setType(QuadType::Button_StateMono);
    Quadrs("PA_PlayAnonPitch").setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedPitch_Start);
    Quadrs("PA_PlayAnonPitch").setQuadrColorRGB(0.55, 1.0, 0.55);
    Quadrs("PA_PlayAnonPitch").setRectangleShape(90, 35);
    Quadrs("PA_PlayAnonPitch").setGx(1205);
    Quadrs("PA_PlayAnonPitch").setGy(310);
    Quadrs("PA_PlayAnonPitch").setText("Play Pitch");

    // BOUTON 2.3 : Play Formant
    Quadrs.add("PA_PlayAnonFormant");
    Quadrs("PA_PlayAnonFormant").setType(QuadType::Button_StateMono);
    Quadrs("PA_PlayAnonFormant").setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedFormant_Start);
    Quadrs("PA_PlayAnonFormant").setQuadrColorRGB(0.55, 0.85, 1.0);
    Quadrs("PA_PlayAnonFormant").setRectangleShape(105, 35);
    Quadrs("PA_PlayAnonFormant").setGx(1440);
    Quadrs("PA_PlayAnonFormant").setGy(310);
    Quadrs("PA_PlayAnonFormant").setText("Play Formant");

    // ========================================================================
    // ZONE 3 : SAUVEGARDE
    // ========================================================================

    // BOUTON 3.1 : Save Original
    Quadrs.add("PA_SaveOriginal");
    Quadrs("PA_SaveOriginal").setType(QuadType::Button_StateMono);
    Quadrs("PA_SaveOriginal").setButtonFunction(QuadButtonEvent::PA_SaveOriginal);
    Quadrs("PA_SaveOriginal").setQuadrColorRGB(0.55, 0.85, 1.0);
    Quadrs("PA_SaveOriginal").setRectangleShape(90, 35);
    Quadrs("PA_SaveOriginal").setGx(765);
    Quadrs("PA_SaveOriginal").setGy(410);
    Quadrs("PA_SaveOriginal").setText("Save OG");

    // BOUTON 3.2 : Save Pitch+Formant
    Quadrs.add("PA_SaveAnonPitchFormant");
    Quadrs("PA_SaveAnonPitchFormant").setType(QuadType::Button_StateMono);
    Quadrs("PA_SaveAnonPitchFormant").setButtonFunction(QuadButtonEvent::PA_SaveAnonymizedPitchFormant);
    Quadrs("PA_SaveAnonPitchFormant").setQuadrColorRGB(1.0, 0.85, 0.55);
    Quadrs("PA_SaveAnonPitchFormant").setRectangleShape(90, 35);
    Quadrs("PA_SaveAnonPitchFormant").setGx(985);
    Quadrs("PA_SaveAnonPitchFormant").setGy(410);
    Quadrs("PA_SaveAnonPitchFormant").setText("Save P+F");

    // BOUTON 3.3 : Save Pitch
    Quadrs.add("PA_SaveAnonPitch");
    Quadrs("PA_SaveAnonPitch").setType(QuadType::Button_StateMono);
    Quadrs("PA_SaveAnonPitch").setButtonFunction(QuadButtonEvent::PA_SaveAnonymizedPitch);
    Quadrs("PA_SaveAnonPitch").setQuadrColorRGB(0.85, 0.55, 1.0);
    Quadrs("PA_SaveAnonPitch").setRectangleShape(90, 35);
    Quadrs("PA_SaveAnonPitch").setGx(1205);
    Quadrs("PA_SaveAnonPitch").setGy(410);
    Quadrs("PA_SaveAnonPitch").setText("Save Pitch");

    // BOUTON 3.4 : Save Formant
    Quadrs.add("PA_SaveAnonFormant");
    Quadrs("PA_SaveAnonFormant").setType(QuadType::Button_StateMono);
    Quadrs("PA_SaveAnonFormant").setButtonFunction(QuadButtonEvent::PA_SaveAnonymizedFormant);
    Quadrs("PA_SaveAnonFormant").setQuadrColorRGB(1.0, 0.85, 0.55);
    Quadrs("PA_SaveAnonFormant").setRectangleShape(105, 35);
    Quadrs("PA_SaveAnonFormant").setGx(1440);
    Quadrs("PA_SaveAnonFormant").setGy(410);
    Quadrs("PA_SaveAnonFormant").setText("Save Formant");

    // === SLIDER 1 : PITCH SHIFT (Semitones) ===
    Quadrs.add("Slider_PitchShift");
    Quadrs("Slider_PitchShift").setType(QuadType::SliderV_FreeLimited);
    Quadrs("Slider_PitchShift").setBgColor(0.3f, 0.3f, 0.55f);
    Quadrs("Slider_PitchShift").setHalfDiag1(55);
    Quadrs("Slider_PitchShift").setHalfDiag2(40);
    Quadrs("Slider_PitchShift").setGx(800);
    Quadrs("Slider_PitchShift").setGy(740);
    Quadrs("Slider_PitchShift").setTriangleColorRGB(0, 0.0, 0.65, 0.0);
    Quadrs("Slider_PitchShift").setTriangleColorRGB(1, 0.0, 1.0, 0.0);
    Quadrs("Slider_PitchShift").setPointColorRGB(0, 1, 0.0, 0.15, 0.0);
    Quadrs("Slider_PitchShift").setPointColorRGB(1, 1, 0.75, 1.0, 0.75);

    Quadrs("Slider_PitchShift").setLegendY_Unit(QuadUnit::Pts);
    Quadrs("Slider_PitchShift").setLegend_yMin_Val(-12.0);  // -12 semitones
    Quadrs("Slider_PitchShift").setLegend_yMax_Val(12.0);   // +12 semitones
    Quadrs("Slider_PitchShift").setLegend_yMin_Text("-12 ST");
    Quadrs("Slider_PitchShift").setLegend_yMax_Text("+12 ST");

    // Position initiale à +3 semitones
    double initialPosY = Quadrs("Slider_PitchShift").getSlider_yMin() +
        (12.0 - 3.0) / 24.0 * (Quadrs("Slider_PitchShift").getSlider_yMax() -
            Quadrs("Slider_PitchShift").getSlider_yMin());
    Quadrs("Slider_PitchShift").setBtn_Gy(initialPosY);

    // === SLIDER 2 : FORMANT SHIFT (Ratio) ===
    Quadrs.add("Slider_FormantShift");
    Quadrs("Slider_FormantShift").setType(QuadType::SliderV_FreeLimited);
    Quadrs("Slider_FormantShift").setBgColor(0.55f, 0.3f, 0.3f);
    Quadrs("Slider_FormantShift").setHalfDiag1(55);
    Quadrs("Slider_FormantShift").setHalfDiag2(40);
    Quadrs("Slider_FormantShift").setGx(1000);
    Quadrs("Slider_FormantShift").setGy(740);
    Quadrs("Slider_FormantShift").setTriangleColorRGB(0, 0.65, 0.0, 0.0);
    Quadrs("Slider_FormantShift").setTriangleColorRGB(1, 1.0, 0.0, 0.0);
    Quadrs("Slider_FormantShift").setPointColorRGB(0, 1, 0.15, 0.0, 0.0);
    Quadrs("Slider_FormantShift").setPointColorRGB(1, 1, 1.0, 0.75, 0.75);

    Quadrs("Slider_FormantShift").setLegendY_Unit(QuadUnit::Pts);
    Quadrs("Slider_FormantShift").setLegend_yMin_Val(0.5);   // ×0.5
    Quadrs("Slider_FormantShift").setLegend_yMax_Val(2.0);   // ×2.0
    Quadrs("Slider_FormantShift").setLegend_yMin_Text("×0.5");
    Quadrs("Slider_FormantShift").setLegend_yMax_Text("×2.0");

    // Position initiale à ×1.18
    double initialPosY_Formant = Quadrs("Slider_FormantShift").getSlider_yMin() +
        (2.0 - 1.18) / 1.5 * (Quadrs("Slider_FormantShift").getSlider_yMax() -
            Quadrs("Slider_FormantShift").getSlider_yMin());
    Quadrs("Slider_FormantShift").setBtn_Gy(initialPosY_Formant);

    // FIX PRINCIPAL : FORCER LA MISE À JOUR DE legend_yVal
    Quadrs("Slider_FormantShift").setSlider_yVal(initialPosY_Formant);

    SDL_Log(">>> Slider Formant initialisé : Pos=%.1f | Valeur=%.3f <<<",
        initialPosY_Formant,
        Quadrs("Slider_FormantShift").getLegend_yVal());

    // === SLIDER 3 : JITTER AMOUNT ===
    Quadrs.add("Slider_Jitter");
    Quadrs("Slider_Jitter").setType(QuadType::SliderV_FreeLimited);
    Quadrs("Slider_Jitter").setBgColor(0.3f, 0.55f, 0.3f);
    Quadrs("Slider_Jitter").setHalfDiag1(55);
    Quadrs("Slider_Jitter").setHalfDiag2(40);
    Quadrs("Slider_Jitter").setGx(1200);
    Quadrs("Slider_Jitter").setGy(740);
    Quadrs("Slider_Jitter").setTriangleColorRGB(0, 0.0, 0.65, 0.0);
    Quadrs("Slider_Jitter").setTriangleColorRGB(1, 0.0, 1.0, 0.0);

    Quadrs("Slider_Jitter").setLegendY_Unit(QuadUnit::Pts);
    Quadrs("Slider_Jitter").setLegend_yMin_Val(0.0);     // Pas de jitter
    Quadrs("Slider_Jitter").setLegend_yMax_Val(0.02);    // Maximum 2%
    Quadrs("Slider_Jitter").setLegend_yMin_Text("0.000");
    Quadrs("Slider_Jitter").setLegend_yMax_Text("0.020");

    // Position initiale à 0.005
    initialPosY = Quadrs("Slider_Jitter").getSlider_yMin() +
        (0.02 - 0.005) / 0.02 * (Quadrs("Slider_Jitter").getSlider_yMax() -
            Quadrs("Slider_Jitter").getSlider_yMin());
    Quadrs("Slider_Jitter").setBtn_Gy(initialPosY);

    // === SLIDER 4 : OUTPUT VOLUME ===
    Quadrs.add("Slider_Volume");
    Quadrs("Slider_Volume").setType(QuadType::SliderV_FreeLimited);
    Quadrs("Slider_Volume").setBgColor(0.55f, 0.55f, 0.3f);
    Quadrs("Slider_Volume").setHalfDiag1(55);
    Quadrs("Slider_Volume").setHalfDiag2(40);
    Quadrs("Slider_Volume").setGx(1400);
    Quadrs("Slider_Volume").setGy(740);
    Quadrs("Slider_Volume").setTriangleColorRGB(0, 0.65, 0.65, 0.0);
    Quadrs("Slider_Volume").setTriangleColorRGB(1, 1.0, 1.0, 0.0);

    Quadrs("Slider_Volume").setLegendY_Unit(QuadUnit::percent);
    Quadrs("Slider_Volume").setLegend_yMin_Val(0.0);     // Silence
    Quadrs("Slider_Volume").setLegend_yMax_Val(100.0);   // Maximum
    Quadrs("Slider_Volume").setLegend_yMin_Text("0%");
    Quadrs("Slider_Volume").setLegend_yMax_Text("100%");

    // Position initiale à 50%
    initialPosY = Quadrs("Slider_Volume").getSlider_yMin() +
        (100.0 - 50.0) / 100.0 * (Quadrs("Slider_Volume").getSlider_yMax() -
            Quadrs("Slider_Volume").getSlider_yMin());
    Quadrs("Slider_Volume").setBtn_Gy(initialPosY);

    Quadrs.add("Quadr_Info");
    Quadrs("Quadr_Info").setType(QuadType::TextBox);
    Quadrs("Quadr_Info").setQuadrColorRGB(0.55, 0.55, 1.0);
    Quadrs("Quadr_Info").setRectangleShape(300, 55);
    Quadrs("Quadr_Info").setGx(1230);
    Quadrs("Quadr_Info").setGy(190);

    // === INITIALISER LE BUFFER FFT INPUT (une seule fois) ===
    paFftInputBuffer.resize(PA_FFT_SIZE, 0.0f);

    // Calculer les FFT initiales
    ComputeFFT_PortAudio();
    SmoothFFTDisplay_PortAudio(0.25f);

    paFftDataReady = true;

    return SDL_APP_CONTINUE;
}

/* This function runs when a new event (mouse input, keypresses, etc) occurs. */
SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }

    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event->button.button == SDL_BUTTON_LEFT) {
            
            SDL_Log(">>> MOUSE DOWN à (%.0f, %.0f) - mousePtrOnQuadr = %s <<<",
                mx, my,
                mousePtrOnQuadr ? mousePtrOnQuadr->getName().c_str() : "nullptr");
            
            if (mousePtrOnQuadr == nullptr) {
                mClickL_PtrOnQuadr = nullptr;
            }
            else {
                mClickL_PtrOnQuadr = mousePtrOnQuadr;
                mClickL_PtrOnQuadr->set_isMouseLeftButtonDown(true);

                mouseClick_OFFSET.x = mx - mClickL_PtrOnQuadr->getBtn_Gx();
                mouseClick_OFFSET.y = my - mClickL_PtrOnQuadr->getBtn_Gy();

                SDL_Log(">>> Offset calculé: (%.2f, %.2f) <<<",
                    mouseClick_OFFSET.x, mouseClick_OFFSET.y);

                mClickL_PtrOnQuadr->quadrColorRGB_ClickUpdate();
            }
        }
    }
    else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event->button.button == SDL_BUTTON_LEFT) {
            SDL_Log(">>> CLIC SOURIS DÉTECTÉ à position (%.0f, %.0f) <<<", mx, my);
            SDL_Log(">>> mClickL_PtrOnQuadr = %s <<<",
                mClickL_PtrOnQuadr ? mClickL_PtrOnQuadr->getName().c_str() : "nullptr");

            // === Traiter le clic AVANT de réinitialiser le pointeur ===
            if (mClickL_PtrOnQuadr != nullptr) {
                SDL_Log(">>> Quadr cliqué: %s (Type: %d) <<<",
                    mClickL_PtrOnQuadr->getName().c_str(),
                    static_cast<int>(mClickL_PtrOnQuadr->getType()));

                // === Vérifier que le relâchement est sur le MÊME bouton ===
                bool isStillOverButton = (mousePtrOnQuadr == mClickL_PtrOnQuadr);

                if (isStillOverButton && mClickL_PtrOnQuadr->getType() == QuadType::Button_StateMono) {
                    SDL_Log(">>> Fonction bouton: %d <<<",
                        static_cast<int>(mClickL_PtrOnQuadr->getButtonFunction()));

                    switch (mClickL_PtrOnQuadr->getButtonFunction()) {

                    case QuadButtonEvent::PA_AudioRecord_Start:
                        SDL_Log(">>> Clic sur PA_AudioRecord_Start <<<");
                        mClickL_PtrOnQuadr->setButtonFunction(QuadButtonEvent::PA_AudioRecord_Stop);
                        mClickL_PtrOnQuadr->setText("Stop Rec");
                        mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.3, 0.3);
                        StartPortAudioRecording();
                        break;

                    case QuadButtonEvent::PA_AudioRecord_Stop:
                        SDL_Log(">>> Clic sur PA_AudioRecord_Stop <<<");
                        mClickL_PtrOnQuadr->setButtonFunction(QuadButtonEvent::PA_AudioRecord_Start);
                        mClickL_PtrOnQuadr->setText("Start Rec");
                        mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.55, 0.55);
                        StopPortAudioRecording();
                        break;

                    case QuadButtonEvent::PA_AudioPlay_Start:
                        SDL_Log(">>> Clic sur PA_AudioPlay_Start <<<");
                        mClickL_PtrOnQuadr->setButtonFunction(QuadButtonEvent::PA_AudioPlay_Stop);
                        mClickL_PtrOnQuadr->setText("Stop Audio");
                        mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.8, 0.0);
                        StartPortAudioPlayback();
                        break;

                    case QuadButtonEvent::PA_AudioPlay_Stop:
                        SDL_Log(">>> Clic sur PA_AudioPlay_Stop <<<");
                        mClickL_PtrOnQuadr->setButtonFunction(QuadButtonEvent::PA_AudioPlay_Start);
                        mClickL_PtrOnQuadr->setText("Play OG");
                        mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 1.0, 0.55);
                        StopPortAudioPlayback();
                        break;

                    case QuadButtonEvent::PA_AudioPlayAnonymizedPitch_Start:
                        SDL_Log(">>> Clic sur PA_AudioPlayAnonymized_Start <<<");
                        mClickL_PtrOnQuadr->setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedPitch_Stop);
                        mClickL_PtrOnQuadr->setText("Stop Pitch");
                        mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.5, 0.0);
                        StartPortAudioPlaybackAnonymized();
                        break;

                    case QuadButtonEvent::PA_AudioPlayAnonymizedPitch_Stop:
                        SDL_Log(">>> Clic sur PA_AudioPlayAnonymized_Stop <<<");
                        mClickL_PtrOnQuadr->setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedPitch_Start);
                        mClickL_PtrOnQuadr->setText("Play Pitch");
                        mClickL_PtrOnQuadr->setQuadrColorRGB(0.55, 1.0, 0.55);
                        StopPortAudioPlaybackAnonymized();
                        break;

                        // === DANS SDL_AppEvent, section switch ===

                    case QuadButtonEvent::PA_AudioPlayAnonymizedFormant_Start:
                        SDL_Log(">>> Clic sur PA_AudioPlayAnonymizedFormant_Start <<<");
                        mClickL_PtrOnQuadr->setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedFormant_Stop);
                        mClickL_PtrOnQuadr->setText("Stop Formant");
                        mClickL_PtrOnQuadr->setQuadrColorRGB(0.0, 0.5, 1.0);
                        StartPortAudioPlaybackAnonymizedFormant();
                        break;

                    case QuadButtonEvent::PA_AudioPlayAnonymizedFormant_Stop:
                        SDL_Log(">>> Clic sur PA_AudioPlayAnonymizedFormant_Stop <<<");
                        mClickL_PtrOnQuadr->setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedFormant_Start);
                        mClickL_PtrOnQuadr->setText("Play Formant");
                        mClickL_PtrOnQuadr->setQuadrColorRGB(0.55, 0.85, 1.0);
                        StopPortAudioPlaybackAnonymizedFormant();
                        break;

                    case QuadButtonEvent::PA_SaveAnonymizedFormant:
                        SDL_Log(">>> Clic sur PA_SaveAnonymizedFormant <<<");
                        if (paAudioBuffer.empty()) {
                            SDL_Log("ERREUR: Aucun audio à sauvegarder (buffer vide)");
                            mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.3, 0.3);
                        }
                        else {
                            vector<Uint8> tempBuffer = paAudioBuffer;

                            // === LIRE LES PARAMÈTRES DEPUIS LES SLIDERS ===
                            float formantShift = g_formantShiftRatio.load(memory_order_acquire);
                            float jitter = g_jitterAmount.load(memory_order_acquire);
                            float volume = g_outputVolume.load(memory_order_acquire);

                            SDL_Log("=== TRAITEMENT DSP FORMANT (PARAMÈTRES SLIDERS) ===");
                            SDL_Log("  Formant: %.2fx | Jitter: %.4f | Volume: %.0f%%",
                                formantShift, jitter, volume * 100.0f);

                            ApplyHighPassFilter(tempBuffer, 80.0f);
                            ApplyRealtimeFormantShift(tempBuffer, formantShift);
                            ApplyJitter(tempBuffer, jitter);
                            NormalizeAudio(tempBuffer);

                            int16_t* samples = reinterpret_cast<int16_t*>(tempBuffer.data());
                            size_t numSamples = tempBuffer.size() / sizeof(int16_t);

                            for (size_t i = 0; i < numSamples; ++i) {
                                float reduced = static_cast<float>(samples[i]) * volume;
                                reduced = Max(-32768.0f, Min(32767.0f, reduced));
                                samples[i] = static_cast<int16_t>(round(reduced));
                            }

                            string formatSuffix = (paMicrophoneChannels == 2) ? "stereo" : "mono";
                            string filename = GenerateWAVFilename("audio_anonymized_formant_lpc_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, paMicrophoneChannels, PA_SAMPLE_RATE)) {
                                mClickL_PtrOnQuadr->setQuadrColorRGB(0.3, 1.0, 0.3);
                            }
                            else {
                                mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.3, 0.3);
                            }
                        }
                        break;

                    case QuadButtonEvent::PA_SaveOriginal:
                        SDL_Log(">>> Clic sur PA_SaveOriginal <<<");
                        if (paAudioBuffer.empty()) {
                            SDL_Log("ERREUR: Aucun audio à sauvegarder (buffer vide)");
                            mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.3, 0.3);
                        }
                        else {
                            // === COPIER LE BUFFER ET APPLIQUER LE VOLUME ===
                            vector<Uint8> tempBuffer = paAudioBuffer;

                            // Lire le volume du slider
                            float volume = g_outputVolume.load(memory_order_acquire);

                            SDL_Log("=== SAUVEGARDE AUDIO ORIGINAL (VOLUME: %.0f%%) ===", volume * 100.0f);

                            // Appliquer le volume
                            int16_t* samples = reinterpret_cast<int16_t*>(tempBuffer.data());
                            size_t numSamples = tempBuffer.size() / sizeof(int16_t);

                            for (size_t i = 0; i < numSamples; ++i) {
                                float reduced = static_cast<float>(samples[i]) * volume;
                                reduced = Max(-32768.0f, Min(32767.0f, reduced));
                                samples[i] = static_cast<int16_t>(reduced);
                            }

                            string formatSuffix = (paMicrophoneChannels == 2) ? "stereo" : "mono";
                            string filename = GenerateWAVFilename("audio_original_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, paMicrophoneChannels, PA_SAMPLE_RATE)) {
                                mClickL_PtrOnQuadr->setQuadrColorRGB(0.3, 1.0, 0.3);
                            }
                            else {
                                mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.3, 0.3);
                            }
                        }
                        break;

                    case QuadButtonEvent::PA_SaveAnonymizedPitch:
                        SDL_Log(">>> Clic sur PA_SaveAnonymized <<<");
                        if (paAudioBuffer.empty()) {
                            SDL_Log("ERREUR: Aucun audio à sauvegarder (buffer vide)");
                            mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.3, 0.3);
                        }
                        else {
                            vector<Uint8> tempBuffer = paAudioBuffer;

                            // === LIRE LES PARAMÈTRES DEPUIS LES SLIDERS ===
                            float pitchShift = g_pitchShiftSemitones.load(memory_order_acquire);
                            float jitter = g_jitterAmount.load(memory_order_acquire);
                            float volume = g_outputVolume.load(memory_order_acquire);

                            SDL_Log("=== TRAITEMENT DSP Pitch (PARAMÈTRES SLIDERS) ===");
                            SDL_Log("  Pitch: %.2f ST | Jitter: %.4f | Volume: %.0f%%",
                                pitchShift, jitter, volume * 100.0f);

                            ApplyHighPassFilter(tempBuffer, 80.0f);
                            ApplyRealtimePitchShift(tempBuffer, pitchShift);
                            ApplyJitter(tempBuffer, jitter);
                            NormalizeAudio(tempBuffer);

                            int16_t* samples = reinterpret_cast<int16_t*>(tempBuffer.data());
                            size_t numSamples = tempBuffer.size() / sizeof(int16_t);

                            for (size_t i = 0; i < numSamples; ++i) {
                                float reduced = static_cast<float>(samples[i]) * volume;
                                reduced = Max(-32768.0f, Min(32767.0f, reduced));
                                samples[i] = static_cast<int16_t>(round(reduced));
                            }

                            string formatSuffix = (paMicrophoneChannels == 2) ? "stereo" : "mono";
                            string filename = GenerateWAVFilename("audio_anonymized_pitch_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, paMicrophoneChannels, PA_SAMPLE_RATE)) {
                                mClickL_PtrOnQuadr->setQuadrColorRGB(0.3, 1.0, 0.3);
                            }
                            else {
                                mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.3, 0.3);
                            }
                        }
                        break;

                    case QuadButtonEvent::PA_AudioPlayAnonymizedPitchFormant_Start:
                        SDL_Log(">>> Clic sur PA_AudioPlayAnonymizedPitchFormant_Start <<<");
                        mClickL_PtrOnQuadr->setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedPitchFormant_Stop);
                        mClickL_PtrOnQuadr->setText("Stop P+F");
                        mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.0, 0.5);
                        StartPortAudioPlaybackAnonymizedPitchFormant();
                        break;

                    case QuadButtonEvent::PA_AudioPlayAnonymizedPitchFormant_Stop:
                        SDL_Log(">>> Clic sur PA_AudioPlayAnonymizedPitchFormant_Stop <<<");
                        mClickL_PtrOnQuadr->setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedPitchFormant_Start);
                        mClickL_PtrOnQuadr->setText("Play P+F");
                        mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.55, 1.0);
                        StopPortAudioPlaybackAnonymizedPitchFormant();
                        break;

                    case QuadButtonEvent::PA_SaveAnonymizedPitchFormant:
                        SDL_Log(">>> Clic sur PA_SaveAnonymizedPitchFormant <<<");
                        if (paAudioBuffer.empty()) {
                            SDL_Log("ERREUR: Aucun audio à sauvegarder (buffer vide)");
                            mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.3, 0.3);
                        }
                        else {
                            vector<Uint8> tempBuffer = paAudioBuffer;

                            // === LIRE LES PARAMÈTRES DEPUIS LES SLIDERS ===
                            float pitchShift = g_pitchShiftSemitones.load(memory_order_acquire);
                            float formantShift = g_formantShiftRatio.load(memory_order_acquire);
                            float jitter = g_jitterAmount.load(memory_order_acquire);
                            float volume = g_outputVolume.load(memory_order_acquire);

                            SDL_Log("=== TRAITEMENT DSP PITCH+FORMANT (PARAMÈTRES SLIDERS) ===");
                            SDL_Log("  Pitch: %.2f ST | Formant: %.2fx | Jitter: %.4f | Volume: %.0f%%",
                                pitchShift, formantShift, jitter, volume * 100.0f);

                            ApplyHighPassFilter(tempBuffer, 80.0f);
                            ApplyRealtimePitchShift(tempBuffer, pitchShift);
                            ApplyRealtimeFormantShift(tempBuffer, formantShift);
                            ApplyJitter(tempBuffer, jitter);
                            NormalizeAudio(tempBuffer);

                            int16_t* samples = reinterpret_cast<int16_t*>(tempBuffer.data());
                            size_t numSamples = tempBuffer.size() / sizeof(int16_t);

                            for (size_t i = 0; i < numSamples; ++i) {
                                float reduced = static_cast<float>(samples[i]) * volume;
                                samples[i] = static_cast<int16_t>(Max(-32768.0f, Min(32767.0f, reduced)));
                            }

                            string formatSuffix = (paMicrophoneChannels == 2) ? "stereo" : "mono";
                            string filename = GenerateWAVFilename("audio_anonymized_pitch_formant_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, paMicrophoneChannels, PA_SAMPLE_RATE)) {
                                mClickL_PtrOnQuadr->setQuadrColorRGB(0.3, 1.0, 0.3);
                            }
                            else {
                                mClickL_PtrOnQuadr->setQuadrColorRGB(1.0, 0.3, 0.3);
                            }
                        }
                        break;

                    case QuadButtonEvent::AudioMode_Toggle:
                        SDL_Log(">>> Clic sur AudioMode_Toggle <<<");
                        if (mClickL_PtrOnQuadr->getText() == "Mode Actif : Enregistrement") {
                            // Passer en mode stream
                            mClickL_PtrOnQuadr->setText("Mode Actif : Stream");
                            mClickL_PtrOnQuadr->setQuadrColorRGB(0.3f, 1.0f, 0.3f);
                            ActivateStreamMode();
                        }
                        else {
                            // Retour au mode enregistrement
                            mClickL_PtrOnQuadr->setText("Mode Actif : Enregistrement");
                            mClickL_PtrOnQuadr->setQuadrColorRGB(0.8f, 0.8f, 0.8f);
                            DeactivateStreamMode();
                        }
                        break;

                    default:
                        SDL_Log(">>> ATTENTION: Événement bouton non géré: %d <<<",
                            static_cast<int>(mClickL_PtrOnQuadr->getButtonFunction()));
                        break;
                    }
                }
                else if (!isStillOverButton) {
                    SDL_Log(">>> Clic annulé : souris hors du bouton lors du relâchement <<<");
                }

                // === Toujours réinitialiser l'état après traitement ===
                mClickL_PtrOnQuadr->set_isMouseLeftButtonDown(false);
                mClickL_PtrOnQuadr->quadrColorRGB_ClickUpdate();
            }
            else {
                SDL_Log(">>> AUCUN QUADR SOUS LA SOURIS <<<");
            }

            // === Réinitialiser le pointeur APRÈS le traitement ===
            mClickL_PtrOnQuadr = nullptr;
        }
    }

    return SDL_APP_CONTINUE;
}

/* This function runs once per frame, and is the heart of the program. */
SDL_AppResult SDL_AppIterate(void* appstate)
{
    const double now = ((double)SDL_GetTicks()) / 1000.0;  /* convert from milliseconds to seconds. */

    // === Détection de fin PortAudio avec flag de transition ===
    static bool paWasPlaying = false;

    if (paIsPlaying) {
        paWasPlaying = true;
    }
    else if (paWasPlaying) {
        // Transition de playing --> stopped détectée
        paWasPlaying = false;

        try {
            Quadrs("PA_Play").setButtonFunction(QuadButtonEvent::PA_AudioPlay_Start);
            Quadrs("PA_Play").setText("Play OG");
            Quadrs("PA_Play").setQuadrColorRGB(1.0, 1.0, 0.55);
            SDL_Log(">>> Lecture PortAudio terminée automatiquement <<<");
        }
        catch (...) {
            SDL_Log("Erreur réinitialisation bouton PA_Play");
        }
    }

    // === Détection de fin PortAudio Anonymisé ===
    static bool paWasPlayingAnonymized = false;

    if (paIsPlayingAnonymized) {
        paWasPlayingAnonymized = true;
    }
    else if (paWasPlayingAnonymized) {
        paWasPlayingAnonymized = false;

        try {
            Quadrs("PA_PlayAnonPitch").setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedPitch_Start);
            Quadrs("PA_PlayAnonPitch").setText("Play Pitch");
            Quadrs("PA_PlayAnonPitch").setQuadrColorRGB(0.55, 1.0, 0.55);
            SDL_Log(">>> Lecture PortAudio Anonymisée terminée automatiquement <<<");
        }
        catch (...) {
            SDL_Log("Erreur réinitialisation bouton PA_PlayAnonPitch");
        }
    }



    // === Détection de fin PortAudio Formant Anonymisé ===
    static bool paWasPlayingAnonymizedFormant = false;

    if (paIsPlayingAnonymizedFormant) {
        paWasPlayingAnonymizedFormant = true;
    }
    else if (paWasPlayingAnonymizedFormant) {
        paWasPlayingAnonymizedFormant = false;

        try {
            Quadrs("PA_PlayAnonFormant").setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedFormant_Start);
            Quadrs("PA_PlayAnonFormant").setText("Play Formant");
            Quadrs("PA_PlayAnonFormant").setQuadrColorRGB(0.55, 0.85, 1.0);
            SDL_Log(">>> Lecture PortAudio Formant Anonymisée terminée automatiquement <<<");
        }
        catch (...) {
            SDL_Log("Erreur réinitialisation bouton PA_PlayAnonFormant");
        }
    }

    // === Détection de fin PortAudio Pitch+Formant ===
    static bool paWasPlayingAnonymizedPitchFormant = false;

    if (paIsPlayingAnonymizedPitchFormant) {
        paWasPlayingAnonymizedPitchFormant = true;
    }
    else if (paWasPlayingAnonymizedPitchFormant) {
        paWasPlayingAnonymizedPitchFormant = false;

        try {
            Quadrs("PA_PlayAnonPitchFormant").setButtonFunction(QuadButtonEvent::PA_AudioPlayAnonymizedPitchFormant_Start);
            Quadrs("PA_PlayAnonPitchFormant").setText("Play P+F");
            Quadrs("PA_PlayAnonPitchFormant").setQuadrColorRGB(1.0, 0.55, 1.0);
            SDL_Log(">>> Lecture PortAudio Pitch+Formant terminée automatiquement <<<");
        }
        catch (...) {
            SDL_Log("Erreur réinitialisation bouton PA_PlayAnonPitchFormant");
        }
    }

    // === MISE À JOUR DES PARAMÈTRES DSP DEPUIS LES SLIDERS ===
    try {
        // Pitch Shift : -12 à +12 semitones (échelle inversée Y)
        double pitchY = Quadrs("Slider_PitchShift").getLegend_yVal();
        g_pitchShiftSemitones.store(static_cast<float>(pitchY), memory_order_release);

        // Formant Shift : 0.5 à 2.0 (échelle inversée Y)
        double formantY = Quadrs("Slider_FormantShift").getLegend_yVal();
        float formantValue = static_cast<float>(formantY);
        g_formantShiftRatio.store(formantValue, memory_order_release);

        // LOG DE DEBUG (à supprimer après fix)
        static int logCounter = 0;
        if (++logCounter % 60 == 0) {  // Toutes les secondes (60 FPS)
            SDL_Log(">>> Slider Formant : legend_yVal=%.3f | atomique=%.3f <<<",
                formantY, formantValue);
        }

        // Jitter : 0.0 à 0.02 (échelle inversée Y)
        double jitterY = Quadrs("Slider_Jitter").getLegend_yVal();
        g_jitterAmount.store(static_cast<float>(jitterY), memory_order_release);

        // Volume : 0 à 100% (échelle inversée Y)
        double volumeY = Quadrs("Slider_Volume").getLegend_yVal();
        g_outputVolume.store(static_cast<float>(volumeY / 100.0), memory_order_release);

    }
    catch (...) {
        SDL_Log("Erreur lecture valeurs sliders DSP");
    }

    // === CALCULER LA FFT ===
    static size_t sampleCountPA = 0;

    // Incrémenter le compteur (approximation basée sur les frames)
    sampleCountPA += 256; // framesPerBuffer dans le callback

    if (sampleCountPA >= FFT_OVERLAP) {
        sampleCountPA = 0;

        // Calculer la FFT PortAudio (spectre brut)
        ComputeFFT_PortAudio();
        SmoothFFTDisplay_PortAudio(0.25f);
    }

    // ========================================================================
    // === RAPPORT DE LATENCE TOUTES LES SECONDES ===
    // ========================================================================
    static Uint64 lastLatencyReport = 0;
    Uint64 currentTicks = SDL_GetTicks();

    if (currentTicks - lastLatencyReport >= 1000) {  // Toutes les 1000ms
        lastLatencyReport = currentTicks;

        if (g_latencyMetrics && g_streamModeActive.load(memory_order_acquire)) {
            g_latencyMetrics->printSecondReport();
        }
    }
    // ========================================================================

    /* clear the window to the draw color. */
    SDL_RenderClear(renderer);

    SDL_GetMouseState(&mx, &my);

    sevenBitsValue = int((toggle1 == true)) + 4 * int((toggle2 == true)) + 32 * int((toggle3 == true)) + 32 * int((toggle4 == true)) + 16 * int((toggle5 == true)) + 32 * int((toggle6 == true)) + 64 * int((toggle7 == true));

    tenBitsValue = int((toggle1 == true)) + 4 * int((toggle2 == true)) + 8 * int((toggle3 == true)) + 8 * int((toggle4 == true)) + 16 * int((toggle5 == true)) + 32 * int((toggle6 == true)) + 64 * int((toggle7 == true)) + 128 * int((toggle8 == true)) + 256 * int((toggle9 == true)) + 512 * int((toggle10 == true));

    toggle1 = not toggle1;

    toggle2 = not ((toggle2 == false) xor (toggle1 == false));

    toggle3 = not ((toggle3 == false) xor (((toggle2 == false) and (toggle1 == false))));

    toggle4 = not ((toggle4 == false) xor (((toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle5 = not ((toggle5 == false) xor (((toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle6 = not ((toggle6 == false) xor (((toggle5 == false) and (toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle7 = not ((toggle7 == false) xor (((toggle6 == false) and (toggle5 == false) and (toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle8 = not ((toggle8 == false) xor (((toggle7 == false) and (toggle6 == false) and (toggle5 == false) and (toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle9 = not ((toggle9 == false) xor (((toggle8 == false) and (toggle7 == false) and (toggle6 == false) and (toggle5 == false) and (toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle10 = not ((toggle10 == false) xor (((toggle9 == false) and (toggle8 == false) and (toggle7 == false) and (toggle6 == false) and (toggle5 == false) and (toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    /* choose the color for the frame we will draw. The sine wave trick makes it fade between colors smoothly. */
    red = (float)(0.5 + 0.5 * SDL_sin(now));
    green = (float)(0.5 + 0.5 * SDL_sin(now + SDL_PI_D * 2 / 3));
    blue = (float)(0.5 + 0.5 * SDL_sin(now + SDL_PI_D * 4 / 3));
    SDL_SetRenderDrawColorFloat(renderer, red, green, blue, SDL_ALPHA_OPAQUE_FLOAT);  /* new color, full alpha. */

    SDL_RenderFillRect(renderer, &rect1);

    //SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

    red = (float)(0.5 + 0.5 * SDL_sin(now + SDL_PI_D));
    green = (float)(0.5 + 0.5 * SDL_sin(now + SDL_PI_D + SDL_PI_D * 2 / 3));
    blue = (float)(0.5 + 0.5 * SDL_sin(now + SDL_PI_D + SDL_PI_D * 4 / 3));
    SDL_SetRenderDrawColorFloat(renderer, red, green, blue, SDL_ALPHA_OPAQUE_FLOAT);  /* new color, full alpha. */

    SDL_RenderFillRect(renderer, &rect2);

    //SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

    //surface = SDL_RotateSurface(surface, float(now)); : to use this function, wait the release of SDL 3.4.0 because 3.3.2 doesn't work

    Quadrs("Quadr1").setGx(280 + 90 * sin((SDL_PI_D * tenBitsValue) / 512));

    SDL_SetRenderDrawColorFloat(renderer, (red - (red - (int(red) % 2))/2), (red - (red - (int(red) % 2)) / 2), (red - (red - (int(red) % 2)) / 2), SDL_ALPHA_OPAQUE_FLOAT);  /* new color, full alpha. */

    rect3.x = Quadrs("Quadr1").getGx() - int(rect3.w/2) + int(160 * SDL_sin(now + SDL_PI_D/128 * sevenBitsValue));
    rect3.y = 240 - int(rect3.w / 2) + int(160 * SDL_sin(now + SDL_PI_D/128 * sevenBitsValue + SDL_PI_D/2));

    SDL_RenderFillRect(renderer, &rect3);

    rect4.x = Quadrs("Quadr1").getGx() - int(rect4.w / 2) + int(160 * SDL_sin(now + SDL_PI_D + SDL_PI_D / 128 * sevenBitsValue));
    rect4.y = 240 - int(rect4.w / 2) + int(160 * SDL_sin(now + SDL_PI_D + SDL_PI_D / 128 * sevenBitsValue + SDL_PI_D / 2));

    SDL_RenderFillRect(renderer, &rect4);

    Quadrs("Quadr1").setAngleFig(fmod(-(SDL_PI_D / 128 * sevenBitsValue), 2 * SDL_PI_D));

    //Update and render the GQuadrs

	bool test = false;

    mousePtrOnQuadr = nullptr;

    for (auto& quad : Quadrs) {
        
        if (mClickL_PtrOnQuadr != nullptr && &quad == mClickL_PtrOnQuadr) {
            if (mClickL_PtrOnQuadr->get_isMouseLeftButtonDown()) {

                if (mx - mouseClick_OFFSET.x < mClickL_PtrOnQuadr->getSlider_xMin()) {
                    mClickL_PtrOnQuadr->setBtn_Gx(mClickL_PtrOnQuadr->getSlider_xMin());
                }
                else if (mx - mouseClick_OFFSET.x > mClickL_PtrOnQuadr->getSlider_xMax()) {
                    mClickL_PtrOnQuadr->setBtn_Gx(mClickL_PtrOnQuadr->getSlider_xMax());
                }
                else {
                    if (mClickL_PtrOnQuadr->getType() != QuadType::SliderH_PosN && mClickL_PtrOnQuadr->getType() != QuadType::SliderH_FreeLimited && mClickL_PtrOnQuadr->getType() != QuadType::Slider2D_FreeLimited && mClickL_PtrOnQuadr->getType() != QuadType::Slider2D_PosN) {
                        mClickL_PtrOnQuadr->setBtn_Gx(mClickL_PtrOnQuadr->getSlider_xMin());
                    }
                    else {
                        if (mClickL_PtrOnQuadr->getType() != QuadType::SliderH_FreeLimited && mClickL_PtrOnQuadr->getType() != QuadType::Slider2D_FreeLimited) {
                            mClickL_PtrOnQuadr->setBtn_Gx(int(mClickL_PtrOnQuadr->getSlider_xMin() + int(((mx - mouseClick_OFFSET.x - mClickL_PtrOnQuadr->getSlider_xMin()) / mClickL_PtrOnQuadr->getRangeSizeX()) + 0.5f) * mClickL_PtrOnQuadr->getRangeSizeX()));
                        }
                        else {
                            mClickL_PtrOnQuadr->setBtn_Gx(mx - mouseClick_OFFSET.x);
                        }
                    }
                }

                if (my - mouseClick_OFFSET.y < mClickL_PtrOnQuadr->getSlider_yMin()) {
                    mClickL_PtrOnQuadr->setBtn_Gy(mClickL_PtrOnQuadr->getSlider_yMin());
                }
                else if (my - mouseClick_OFFSET.y > mClickL_PtrOnQuadr->getSlider_yMax()) {
                    mClickL_PtrOnQuadr->setBtn_Gy(mClickL_PtrOnQuadr->getSlider_yMax());
                }
                else {
                    if (mClickL_PtrOnQuadr->getType() != QuadType::SliderV_PosN && mClickL_PtrOnQuadr->getType() != QuadType::SliderV_FreeLimited && mClickL_PtrOnQuadr->getType() != QuadType::Slider2D_FreeLimited && mClickL_PtrOnQuadr->getType() != QuadType::Slider2D_PosN) {
                        mClickL_PtrOnQuadr->setBtn_Gy(mClickL_PtrOnQuadr->getSlider_yMin());
                    }
                    else {
                        if (mClickL_PtrOnQuadr->getType() != QuadType::SliderV_FreeLimited && mClickL_PtrOnQuadr->getType() != QuadType::Slider2D_FreeLimited) {
                            mClickL_PtrOnQuadr->setBtn_Gy(mClickL_PtrOnQuadr->getSlider_yMin() + int(((my - mouseClick_OFFSET.y - mClickL_PtrOnQuadr->getSlider_yMin()) / mClickL_PtrOnQuadr->getRangeSizeY()) + 0.5f) * mClickL_PtrOnQuadr->getRangeSizeY());
                        }
                        else {
                            mClickL_PtrOnQuadr->setBtn_Gy(my - mouseClick_OFFSET.y);
                        }
                    }
                }

                mClickL_PtrOnQuadr->setSlider_xVal(mClickL_PtrOnQuadr->getBtn_Gx());
                mClickL_PtrOnQuadr->setSlider_yVal(mClickL_PtrOnQuadr->getBtn_Gy());

                if (mClickL_PtrOnQuadr->getType() == QuadType::SliderH_PosN || mClickL_PtrOnQuadr->getType() == QuadType::SliderH_FreeLimited || mClickL_PtrOnQuadr->getType() == QuadType::Slider2D_FreeLimited || mClickL_PtrOnQuadr->getType() == QuadType::Slider2D_PosN) {
                    mClickL_PtrOnQuadr->setLegend_xText(FormatValueWithUnit(mClickL_PtrOnQuadr->getLegend_xVal(), mClickL_PtrOnQuadr->getLegendX_Unit()));
                }
                else{
                    mClickL_PtrOnQuadr->setLegend_xText("");
                }

                if (mClickL_PtrOnQuadr->getType() == QuadType::SliderV_PosN || mClickL_PtrOnQuadr->getType() == QuadType::SliderV_FreeLimited || mClickL_PtrOnQuadr->getType() == QuadType::Slider2D_FreeLimited || mClickL_PtrOnQuadr->getType() == QuadType::Slider2D_PosN) {
                    mClickL_PtrOnQuadr->setLegend_yText(FormatValueWithUnit(mClickL_PtrOnQuadr->getLegend_yVal(), mClickL_PtrOnQuadr->getLegendY_Unit()));
                }
                else {
					mClickL_PtrOnQuadr->setLegend_yText("");
                }

                Quadrs("Quadr_Info").setText(mClickL_PtrOnQuadr->getLegend_xText() + (mClickL_PtrOnQuadr->getType() == QuadType::Slider2D_FreeLimited || mClickL_PtrOnQuadr->getType() == QuadType::Slider2D_PosN ? " ; " : "") + mClickL_PtrOnQuadr->getLegend_yText());
            }
            else {
                if (mousePtrOnQuadr == nullptr) {
                    Quadrs("Quadr_Info").setText("");
                }
                else {
                    Quadrs("Quadr_Info").setText(mousePtrOnQuadr->getLegend_xText() + (mousePtrOnQuadr->getType() == QuadType::Slider2D_FreeLimited || mousePtrOnQuadr->getType() == QuadType::Slider2D_PosN ? " ; " : "") + mousePtrOnQuadr->getLegend_yText());
                }
            }
        }

        if (quad.updatePos()) {
            
            // === Vérifier si on doit ignorer ce bouton ===
            bool shouldIgnoreButton = false;

            try {
                // Récupérer le mode audio actuel
                string audioModeText = Quadrs("AudioMode").getText();

                // Si mode Stream ET bouton audio (sauf AudioMode lui-même)
                if (audioModeText == "Mode Actif : Stream" &&
                    quad.getType() == QuadType::Button_StateMono) {

                    string quadName = quad.getName();

                    // Liste des noms à NE PAS bloquer
                    if (quadName != "AudioMode" &&
                        quadName != "Mode audio" &&
                        quadName != "Mode Actif : Stream" &&
                        quadName != "Mode Actif : Enregistrement") {

                        // Bloquer tous les autres boutons audio
                        shouldIgnoreButton = true;
                        SDL_Log("Bouton '%s' ignoré (Mode Stream actif)", quadName.c_str());
                    }
                }
            }
            catch (...) {
                SDL_Log("Erreur lors de la vérification du mode audio");
            }

            // === APPLIQUER LA LOGIQUE ===
            if (!shouldIgnoreButton) {
                mousePtrOnQuadr = &quad;
                test = true;

                if (mousePtrOnQuadr->getType() == QuadType::SliderH_PosN || mousePtrOnQuadr->getType() == QuadType::SliderH_FreeLimited) {
                    Quadrs("Quadr_Info").setText(mousePtrOnQuadr->getLegend_xText());
                }
                else if (mousePtrOnQuadr->getType() == QuadType::SliderV_PosN || mousePtrOnQuadr->getType() == QuadType::SliderV_FreeLimited) {
                    Quadrs("Quadr_Info").setText(mousePtrOnQuadr->getLegend_yText());
                }
                else if (mousePtrOnQuadr->getType() == QuadType::Slider2D_FreeLimited || mousePtrOnQuadr->getType() == QuadType::Slider2D_PosN) {
                    Quadrs("Quadr_Info").setText(mousePtrOnQuadr->getLegend_xText() + " ; " + mousePtrOnQuadr->getLegend_yText());
                }
                else {
                    mousePtrOnQuadr->setLegend_yText("");
                }
            }
            else {
                if (quad.getZoomFig() != 1.0f) {
                    quad.setZoomFig(1.0f);
                }
            }

        }
        quad.renderFig();
    }

    if (not test) {
		mousePtrOnQuadr = nullptr;
        Quadrs("Quadr_Info").setText("");
    }

    // === AFFICHAGE DES SPECTRES ===
    RenderFFTSpectrum_PortAudio(50.0f, 550.0f, 600.0f, 200.0f); // PortAudio FFT (20-20000 Hz)
    RenderWaveform(50.0f, 800.0f, 600.0f, 200.0f);             // Oscilloscope

    /* put the newly-cleared rendering on the screen. */
    SDL_RenderPresent(renderer);

    return SDL_APP_CONTINUE;  /* carry on with the program! */
}

/* This function runs once at shutdown. */
void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    
    if (gLogoTexture) {
        SDL_DestroyTexture(gLogoTexture);
        gLogoTexture = nullptr;
    }
    
    // Nettoyer les métriques de latence
    if (g_latencyMetrics) {
        delete g_latencyMetrics;
        g_latencyMetrics = nullptr;
    }
    
    // Nettoyer le mode stream
    CleanupStreamMode();

    // Nettoyer PortAudio
    CleanupPortAudio();
    
    if (gFont) {
        TTF_CloseFont(gFont);
        gFont = nullptr;
    }
    TTF_Quit();
    
    /* SDL will clean up the window/renderer for us. */
}