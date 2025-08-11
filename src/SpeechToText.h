#pragma once
#include <iostream>
#include <nlohmann/json_fwd.hpp>
#include <vosk_api.h>
#include <portaudio.h>
#include <vector>
#include <cmath>
#include <chrono>
#include <nlohmann/json.hpp>
#include "TextToSpeech.h"

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