#include "TextToSpeech.h"
#include <cstring>
#include <espeak-ng/speak_lib.h>



int TextToSpeech::Initialize() {

    std::cout << "\n>─────────────────────[INITIALIZING TEXT-TO-SPEECH]─────────────────────<\n" << std::endl;

    samplerate = espeak_Initialize(AUDIO_OUTPUT_PLAYBACK, // play to default audio device
                                   0,                  // buffer length (use default)
                                   NULL,                    // path to espeak-ng-data (NULL - $ESPEAK_DATA_PATH)
                                   0);                   // options bits
    if (samplerate <= 0) {
        std::cerr << "[ERROR] Failed to initialize eSpeak NG!" << std::endl;
        return 1;
    }
    std::cout << "[INFO] eSpeak NG initialized @ " << samplerate << " Hz" << std::endl;

    // set the voice by name (must match a folder/voice file)
    if (espeak_SetVoiceByName(voice_name) != EE_OK) {
        std::cerr << "[ERROR] Voice \"" << voice_name << "\" not found, using default!" << std::endl;
        return 1;
    }
    std::cout << "[INFO] Successfully loaded voice \"" << voice_name << "\"!" << std::endl;

    espeak_SetParameter(espeakRATE, 150, 0);   // default ~175 wpm
    espeak_SetParameter(espeakPITCH, 50, 0);   // default 50
    espeak_SetParameter(espeakVOLUME, 100, 0); // default 100% volume
    std::cout << "[INFO] Voice parameters set!" << std::endl;

    // register an event callback to track synthesis events, TODO: add valid conversion to t_espeak_callback
    //espeak_SetSynthCallback(SynthCallback(NULL,NULL,NULL));

    std::cout << "\n>────────────────[INITIALIZED TEXT-TO-SPEECH SUCCESSULLY]───────────────<\n" << std::endl;
    return 0;
}

void TextToSpeech::Verbalize(const char* TEXT) {
    espeak_ERROR speakErr = espeak_Synth(
        TEXT,
        strlen(TEXT) + 1,     // include terminating NUL
        0,                   // start position in text
        POS_CHARACTER,  // position type
        0,               // end position (no limit)
        espeakCHARS_AUTO,       // let eSpeak decide encoding
        NULL,       // unique identifier (unused)
        NULL                // user data pointer
    );
    if (speakErr != EE_OK) {
        std::cerr << "[ERROR] espeak_Synth error: \n" << speakErr << std::endl;
    }
}

void Shutdown() {
    espeak_Synchronize(); // ensure all speech is played before exiting
    espeak_Terminate();
}

// callback to receive synthesis events (timing, phonemes...)
int TextToSpeech::SynthCallback(short* wav, int numsamples, espeak_EVENT* events) {
    return 0;  // continue synthesis
}