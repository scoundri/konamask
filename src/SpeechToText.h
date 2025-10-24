#pragma once
#include <iostream>
#include <nlohmann/json_fwd.hpp>
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

class SpeechToText {
public:
    int Initialize();
private:
    Settings& cfg = Settings::GetInstance();

    // silence detection settings
    #define SILENCE_THRESHOLD cfg.get<int>("silence_threshold", 200)       // amplitude threshold
    #define SILENCE_TIMEOUT_MS cfg.get<int>("silence_threshold", 1000)     // required silence duration to finalize

    VoskModel *model;
    VoskRecognizer *recognizer;
    PaStream *stream;
    nlohmann::json j_result;
    int framesPerBuffer;
    double sampleRate;
    int Run();
};

class InputVisualizer {
public:
    InputVisualizer();
    ~InputVisualizer();

    bool Initialize(int sampleRate, int fftSize = 2048, int ringBufferSeconds = 2);
    void PushSamples(const int16_t* samples, size_t count);
    void Process();
    void render();

    void setSmoothing(float alpha) { smoothingAlpha = alpha; }
    void setGain(float g) { gain = g; }

private:
    std::vector<int16_t> ring;
    size_t ringMask;
    std::atomic<uint64_t> writeIndex; // absolute sample index
    std::atomic<uint64_t> readIndex;  // absolute sample index

    
    int sr; // fft state
    int N;  // fft size
    int halfN;
    std::vector<double> tw_re, tw_im;   // twiddle factors
    std::vector<double> win;            // window function
    std::vector<double> fft_re, fft_im; // temp arrays

    // output buffers for display
    std::vector<float> spectrum; // magnitude-dB or normalized
    std::vector<float> waveform; // last waveform
    std::vector<float> spectrumSmoothed;
    std::vector<float> peakHold;
    std::chrono::steady_clock::time_point lastPeakDecay;

    // ui parameters
    float gain;             // applied to incoming signal for display
    float smoothingAlpha;
    float peakDecayPerSec;  // dB per sec
    float minDb, maxDb;

    // temporary storage
    std::vector<double> tmp_re, tmp_im;

    // helper methods
    void prepareTwiddles();
    void computeFFT();
    void inplaceFFT(std::vector<double>& re, std::vector<double>& im);
    inline void writeRing(const int16_t* src, size_t n);
    inline size_t availableSamples() const;
    inline void readFromRing(int16_t* dst, size_t n);

    // utility
    static inline double hannWindow(double x, int N) {
        return 0.5 * (1.0 - std::cos(2.0 * M_PI * x / (N - 1)));
    }
};