#include "TextToSpeech.h"
#include <cstring>
#include <portaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdlib>   // for system() & voice-fail shutdown

pa_simple *pa = nullptr;


extern "C" int SynthCallbackC(short* wav, int numsamples, espeak_EVENT* events) {
    return TextToSpeech::SynthCallback(wav, numsamples, events);
}

int TextToSpeech::Initialize() {
Settings cfg;

    std::cout << "\n>─────────────────────[INITIALIZING TEXT-TO-SPEECH]─────────────────────<\n" << std::endl;

    // ensure loopback are loaded
    system("pactl load-module module-null-sink sink_name=VirtualSink sink_properties=device.description=konamask");
    system("pactl load-module module-remap-source source_name=konamask master=VirtualSink.monitor source_properties=device.description=konamask");

    // create virtual microphone
    pa_sample_spec ss;
    ss.format   = PA_SAMPLE_S16LE;                  // 16‑bit PCM
    ss.rate     = cfg.PA_SAMPLE_SPEC_RATE;          // must match espeak_Initialize
    ss.channels = 1;

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
        std::cerr << "[ERROR] pa_simple_new() failed: " << pa_strerror(pa_error) << "\nShutting down!";
        std::exit(EXIT_FAILURE);
    }


    samplerate = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, // audio device (retrieve raw samples instead of playing them)
                                   cfg.PA_SAMPLE_SPEC_RATE,             // buffer length - KEEP ss.rate
                                   NULL,                     // path to espeak-ng-data (NULL - $ESPEAK_DATA_PATH)
                                   0);                    // options bits
    if (samplerate <= 0) {
        std::cout << "[ERROR] Failed to initialize eSpeak NG, shutting down!" << std::endl;        
        std::exit(EXIT_FAILURE);
    }
    std::cout << "[INFO] eSpeak NG initialized @ " << samplerate << " Hz" << std::endl;

    // set the voice by name (must match a folder/voice file)
    if (espeak_SetVoiceByName(cfg.SPEECH_VOICEBANK) != EE_OK) {
        std::cout << "[ERROR] Voicebank \"" << cfg.SPEECH_VOICEBANK << "\" could not be found, using default!" << std::endl;
        if (espeak_SetVoiceByName("en-us") == EE_OK) {
            std::cout << "[INFO] Default voicebank found!" << std::endl;
        } else {
            std::cout << "[ERROR] Default voicebank not found, shutting down!" << std::endl;
            std::exit(EXIT_FAILURE);
        }
    } 
    std::cout << "[INFO] Successfully loaded voicebank!" << std::endl;
    espeak_SetParameter(espeakRATE, cfg.SPEECH_RATE, 0);   // default ~175 wpm
    espeak_SetParameter(espeakPITCH, cfg.SPEECH_PITCH, 0);   // default 50
    espeak_SetParameter(espeakVOLUME, cfg.SPEECH_VOLUME, 0); // default 100% volume
    std::cout << "[INFO] Voice parameters have been set!" << std::endl;

    espeak_SetSynthCallback(SynthCallbackC);

    // clean-up
    cfg.PA_SAMPLE_SPEC_RATE = NULL;
    cfg.SPEECH_RATE = NULL;
    cfg.SPEECH_PITCH = NULL;
    cfg.SPEECH_VOLUME = NULL;
    cfg.SPEECH_VOICEBANK = NULL;
    std::cout << "[INFO] Cleared unnecessary memory!" << std::endl;

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
    system("bash -c 'if [[ -f /tmp/virt_mods ]]; then for id in $(< /tmp/virt_mods); do pactl unload-module \"$id\" || echo \"Warning: failed to unload $id\"; done; rm /tmp/virt_mods; fi'");
    //pa_simple_drain(pa, &pa_error); TODO: make &pa_error accessible to TextToSpeech
    pa_simple_free(pa);
}

// callback (espeak will call it with raw PCM)
int TextToSpeech::SynthCallback(short* wav, int numsamples, espeak_EVENT* events) {
        if (wav && numsamples > 0) {
        int error;
        // write the PCM into pulseaudio sink
        if (pa_simple_write(pa, wav, numsamples * sizeof(short), &error) < 0) {
            std::cerr << "[ERROR] pa_simple_write failed: " << pa_strerror(error) << "\n";
            return 1;  // abort synthesis
        }
    }
    return 0;
}