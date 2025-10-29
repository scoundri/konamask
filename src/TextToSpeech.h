#pragma once
#include "Settings.h"
#include <espeak-ng/espeak_ng.h>
#include <espeak-ng/speak_lib.h>
#include <iostream>

class TextToSpeech {
public:
    int Initialize();
    static void Verbalize(const char* TEXT);
    void Shutdown();
    static int SynthCallback(short* wav, int numsamples, espeak_EVENT* events);
    static void render();
private:
    unsigned int samplerate;
};