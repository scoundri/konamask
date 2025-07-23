#include <iostream>
#include <vosk_api.h>
#include <portaudio.h>
#include <vector>
#include <cmath>
#include <chrono>

class SpeechToText {
public:
    int Initialize();
private:
    // silence detection settings
    #define SILENCE_THRESHOLD     200    // amplitude threshold
    #define SILENCE_TIMEOUT_MS   1000    // required silence duration to finalize

    VoskModel *model;
    VoskRecognizer *recognizer;
    PaStream *stream;
    int framesPerBuffer;
    double sampleRate;
    int Run();
};