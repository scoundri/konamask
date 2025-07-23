#include "TextToSpeech.h"
#include <cstring>
#include <portaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdlib>   // for system()

pa_simple *pa = nullptr;


extern "C" int SynthCallbackC(short* wav, int numsamples, espeak_EVENT* events) {
    return TextToSpeech::SynthCallback(wav, numsamples, events);
}

int TextToSpeech::Initialize() {

    std::cout << "\n>─────────────────────[INITIALIZING TEXT-TO-SPEECH]─────────────────────<\n" << std::endl;

    // ensure loopback are loaded
    system("pactl load-module module-null-sink sink_name=VirtualSink sink_properties=device.description=Virtual_Sink");
    system("pactl load-module module-remap-source source_name=VirtualMic master=VirtualSink.monitor source_properties=device.description=Virtual_Mic");

    // create virtual microphone
    pa_sample_spec ss;
    ss.format   = PA_SAMPLE_S16LE;          // 16‑bit PCM
    ss.rate     = 22050;                    // must match espeak_Initialize
    ss.channels = 1;                        // mono

    int pa_error;
    pa = pa_simple_new(
        nullptr,                             // server (NULL = default)
        "konamask Virtual Micropone",          // client name
        PA_STREAM_PLAYBACK,
        "VirtualSink",                     // sink name
        "konamask routed TTS voice",    // stream description
        &ss,                                     // sample format
        nullptr, nullptr,
        &pa_error
    );
    if (!pa) {
        std::cerr << "[ERROR] pa_simple_new() failed: " << pa_strerror(pa_error) << "\n";
        return 1;
    }


    samplerate = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, // audio device (retrieve raw samples instead of playing them)
                                   ss.rate,             // buffer length - KEEP ss.rate
                                   NULL,                     // path to espeak-ng-data (NULL - $ESPEAK_DATA_PATH)
                                   0);                    // options bits
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
    espeak_SetSynthCallback(SynthCallbackC);

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


void TextToSpeech::Shutdown() {
    espeak_Synchronize(); // ensure all speech is played before exiting
    espeak_Terminate();
    //pa_simple_drain(pa, &pa_error); TODO: make &pa_error accessible to TextToSpeech
    pa_simple_free(pa);
}

// callback (espeak will call it with raw PCM)
int TextToSpeech::SynthCallback(short* wav, int numsamples, espeak_EVENT* events) {
        if (wav && numsamples > 0) {
        int error;
        // write the PCM into pulseaudio sink
        if (pa_simple_write(pa, wav, numsamples * sizeof(short), &error) < 0) {
            std::cerr << "pa_simple_write failed: " << pa_strerror(error) << "\n";
            return 1;  // abort synthesis
        }
    }
    return 0;
    return 0;  // continue synthesis
}