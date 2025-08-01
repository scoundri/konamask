#pragma once
#include "SimpleIni.h"
#include <string>
#include <unordered_map>

class Settings {
public:
    int Initialize();

    // ------- values -------
    // SpeechToText class:
    const char* VOSK_MODEL_PATH = "model";
    double BUFFER_FACTOR = 0.05; // 50ms
    // TextToSpeech class:
    unsigned short SPEECH_RATE = 150;
    short SPEECH_PITCH = 50;
    unsigned short SPEECH_VOLUME = 100;
    const char* SPEECH_VOICEBANK = "en-us";
    
    int PA_SAMPLE_SPEC_RATE = 22050;

protected:
    CSimpleIniA ini;
};