#pragma once
#include <espeak-ng/espeak_ng.h>
#include <espeak-ng/speak_lib.h>
#include <iostream>

class TextToSpeech {
public:
    int Initialize();
    static void Verbalize(const char* TEXT);
    void Shutdown();
    static int SynthCallback(short* wav, int numsamples, espeak_EVENT* events);
private:
    const char* voice_name = "en-us"; // TODO: add voicebank customizability
    unsigned int samplerate;
};