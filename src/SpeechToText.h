#pragma once
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <thread>
#include <vosk_api.h>
#include <portaudio.h>
#include <vector>
#include <atomic>
#include <cmath>
#include <chrono>
#include <nlohmann/json.hpp>
#include "TextToSpeech.h"

#ifdef _WIN32
// avoid windows min/max collisions with <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

enum class ProcessingMode {
    FinalOnSilence,
    IncrementalPartial
};

class SpeechToText {
public:
    ~SpeechToText();
    int Initialize();
    void render();
    void Shutdown();

    void Start();
    void Stop();
    void SwitchMode(ProcessingMode m) { currentMode.store(m); controlCv.notify_all(); };
    void RequestRestartInput();
    bool ReopenInputStream();
    bool SwitchToDevice(PaDeviceIndex newDev);
private:
    Settings& cfg = Settings::GetInstance();    
    void PauseListening();
    void ResumeListening();
    void RequestExit();
    void WorkerLoop();
    void RecreateRecognizerLocked(double newSampleRate);
    void ProcessBuffer_Final(const std::vector<int16_t>& buffer);
    void ProcessBuffer_Partial(const std::vector<int16_t>& buffer);

    // silence detection settings
    #define SILENCE_THRESHOLD cfg.get<int>("silence_threshold", 200)       // amplitude threshold
    #define SILENCE_TIMEOUT_MS cfg.get<int>("silence_threshold", 1000)     // required silence duration to finalize

    std::atomic<bool> stopRequested{false};
    PaDeviceIndex selectedDevice = paNoDevice;

    VoskModel *model;
    VoskRecognizer *recognizer;
    PaStream *stream;
    nlohmann::json j_result;
    int framesPerBuffer;
    double sampleRate;

    int visFftSize = 2048;
    int visRingSeconds = 2;

    std::atomic<PaDeviceIndex> pendingDevice{paNoDevice};
    std::thread workerThread;
    std::atomic<bool> workerRunning{false};
    std::atomic<bool> restartRequested{false};
    std::atomic<bool> exitRequested{false};
    std::atomic<bool> paused{false};
    std::condition_variable controlCv;
    std::condition_variable exitCv;
    std::mutex streamMutex;
    std::atomic<ProcessingMode> currentMode{ProcessingMode::FinalOnSilence};

    std::mutex tts_mtx;
};

class InputVisualizer {
public:
    InputVisualizer();
    ~InputVisualizer();

    bool Initialize(int sampleRate, int fftSize = 2048, int ringBufferSeconds = 2);
    void PushSamples(const int16_t* samples, size_t count);
    void Process();
    void render(Settings *cfg);

    void setSmoothing(float alpha) { smoothingAlpha = alpha; }
    void setGain(float g) { gain = g; }

    int getSampleRate() const { return sr; }
    int getFFTSize() const { return N; }
    std::atomic<bool> initialized;
    std::atomic<bool> enabled;
private:
    std::vector<int16_t> ring;
    size_t ringMask = 0;
    std::atomic<uint64_t> writeIndex;
    std::atomic<uint64_t> readIndex;

    int sr = 0; // fft state
    int N = 0;  // fft size
    int halfN = 0;
    std::vector<double> tw_re, tw_im;
    std::vector<double> win;
    std::vector<double> fft_re, fft_im; // temporary

    // output buffers for display
    std::vector<float> spectrum; // magnitude normalized
    std::vector<float> waveform; // last waveform
    std::vector<float> spectrumSmoothed;
    std::vector<float> peakHold;
    std::chrono::steady_clock::time_point lastPeakDecay;

    // ui parameters
    float gain = 1.0f;             // applied to incoming signal for display
    float smoothingAlpha = 0.6f;
    float peakDecayPerSec = 12.0f;  // dB per sec
    float minDb = -90.0f, maxDb = 0.0f;

    // temporary storage
    std::vector<double> tmp_re, tmp_im;

    void prepareTwiddles();
    void inplaceFFT(std::vector<double>& re, std::vector<double>& im);
    inline void writeRing(const int16_t* src, size_t n);
    inline size_t availableSamples() const;
    inline void readFromRing(int16_t* dst, size_t n);

    // utility
    static inline double hannWindow(double x, int N) {
        return 0.5 * (1.0 - std::cos(2.0 * M_PI * x / (N - 1)));
    }
};

extern InputVisualizer visualizer;
extern SpeechToText stt;