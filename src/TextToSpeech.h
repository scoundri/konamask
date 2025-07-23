#pragma once
#include <espeak-ng/espeak_ng.h>
#include <iostream>

class TextToSpeech {
public:
    int Initialize();
    static void Verbalize(const char* TEXT);
private:
    const char* voice_name = "en-us"; // TODO: add voicebank customizability
    unsigned int samplerate;
    int SynthCallback(short* wav, int numsamples, espeak_EVENT* events);
};