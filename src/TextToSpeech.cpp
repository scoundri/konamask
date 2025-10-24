#include "TextToSpeech.h"
#include "Logger.h"
#include <cstring>
#include <portaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdlib>   // for system() & voice-fail shutdown
#include <string>

pa_simple *pa = nullptr;


extern "C" int SynthCallbackC(short* wav, int numsamples, espeak_EVENT* events) {
    return TextToSpeech::SynthCallback(wav, numsamples, events);
}

int TextToSpeech::Initialize() {
Settings& cfg = Settings::GetInstance();
    std::cout << "\n>─────────────────────[INITIALIZING TEXT-TO-SPEECH]─────────────────────<\n" << std::endl;
    Logger::GetInstance().log("\n>---------------------[INITIALIZING TEXT-TO-SPEECH]---------------------<\n\n");
    // ensure loopback are loaded
    system("pactl load-module module-null-sink sink_name=VirtualSink sink_properties=device.description=konamask");
    system("pactl load-module module-remap-source source_name=konamask master=VirtualSink.monitor source_properties=device.description=konamask");

    // create virtual microphone
    pa_sample_spec ss;
    ss.format   = PA_SAMPLE_S16LE;                                                // 16‑bit PCM
    ss.rate     = cfg.get<int>("pa_sample_spec_rate", 22050);   // must match espeak_Initialize
    std::cout << "[INFO] PulseAudio sample rate has been set to  \"" << cfg.get<int>("pa_sample_spec_rate", 22050) << "\"." << std::endl;
    Logger::GetInstance().log("[INFO] PulseAudio sample rate has been set to  \"");
    Logger::GetInstance().log(cfg.get<std::string>("pa_sample_spec_rate", "22050"));
    Logger::GetInstance().log("\".\n");
    ss.channels = 1;

    int pa_error;
    pa = pa_simple_new(
        nullptr,                             // server (NULL = default)
        "konamask-virtual-microphone",          // client name
        PA_STREAM_PLAYBACK,
        "VirtualSink",                          // sink name
        "konamask routed TTS voice",    // stream description
        &ss,                                     // sample format
        nullptr, nullptr,
        &pa_error
    );
    if (!pa) {
        std::cerr << "[ERROR] pa_simple_new() failed: " << pa_strerror(pa_error) << "\nShutting down!";
        Logger::GetInstance().log("[ERROR] pa_simple_new() failed: ");
        Logger::GetInstance().log(pa_strerror(pa_error));
        Logger::GetInstance().log("\n[INFO] Shutting down!\n");
        std::exit(EXIT_FAILURE);
    }
    std::cout << "[INFO] Successfully created a virtual input!" << std::endl;
    Logger::GetInstance().log("[INFO] Successfully created a virtual input!\n");

    samplerate = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, // audio device (retrieve raw samples instead of playing them)
                                   ss.rate,             // buffer length - KEEP SAME AS 'ss.rate'!
                                   NULL,                     // path to espeak-ng-data (NULL - $ESPEAK_DATA_PATH)
                                   0);                    // options bits
    if (samplerate <= 0) {
        std::cout << "[ERROR] Failed to initialize eSpeak NG, shutting down!" << std::endl;
        Logger::GetInstance().log("[ERROR] Failed to initialize eSpeak NG, shutting down!n\n");
        std::exit(EXIT_FAILURE);
    }
    std::cout << "[INFO] eSpeak NG initialized @ " << samplerate << " Hz" << std::endl;
    Logger::GetInstance().log("[INFO] eSpeak NG initialized @ ");
    Logger::GetInstance().log(std::to_string(samplerate));
    Logger::GetInstance().log(" Hz\n");
    // set the voice by name (must match a folder/voice file)
    if (espeak_SetVoiceByName(cfg.get<std::string>("speech_vociebank", "en-us").c_str()) != EE_OK) {
        std::cout << "[ERROR] Voicebank \"" << cfg.get<std::string>("speech_vociebank", "en-us") << "\" could not be found, using default!" << std::endl;
        Logger::GetInstance().log("[ERROR] Voicebank \"" );
        Logger::GetInstance().log(cfg.get<std::string>("speech_vociebank", "en-us"));
        Logger::GetInstance().log("\" could not be found, using default!\n");
        if (espeak_SetVoiceByName("en-us") == EE_OK) {
            std::cout << "[INFO] Default voicebank found!" << std::endl;
            Logger::GetInstance().log("[INFO] Default voicebank found!\n");
        } else {
            std::cout << "[ERROR] Default voicebank not found, shutting down!" << std::endl;
            Logger::GetInstance().log("[ERROR] Default voicebank not found, shutting down!\n");
            std::exit(EXIT_FAILURE);
        }
    } 
    std::cout << "[INFO] Successfully loaded voicebank!" << std::endl;
    Logger::GetInstance().log("[INFO] Successfully loaded voicebank!");
    espeak_SetParameter(espeakRATE, cfg.get<int>("speech_rate", 150), 0);   // default ~150 wpm
    std::cout << "[INFO] Speech rate has been set to \"" << cfg.get<int>("speech_rate", 150) << "\"." << std::endl;
    Logger::GetInstance().log("[INFO] Speech rate has been set to \"");
    Logger::GetInstance().log(cfg.get<std::string>("speech_rate", "150"));
    Logger::GetInstance().log("\".\n");
    espeak_SetParameter(espeakPITCH, cfg.get<int>("speech_pitch", 50), 0);   // default 50
    std::cout << "[INFO] Speech pitch has been set to \"" << cfg.get<int>("speech_pitch", 50) << "\"." << std::endl;
    Logger::GetInstance().log("[INFO] Speech pitch has been set to \"");
    Logger::GetInstance().log(cfg.get<std::string>("speech_pitch", "50"));
    Logger::GetInstance().log("\".\n");
    espeak_SetParameter(espeakVOLUME, cfg.get<int>("speech_volume",100), 0); // default 100% volume
    std::cout << "[INFO] Speech volume has been set to \"" << cfg.get<int>("speech_volume",100) << "\"." << std::endl;
    Logger::GetInstance().log("[INFO] Speech volume has been set to \"");
    Logger::GetInstance().log(cfg.get<std::string>("speech_volume","100"));
    Logger::GetInstance().log("\".\n");
    std::cout << "[INFO] Voice parameters have been set!" << std::endl;
    Logger::GetInstance().log("[INFO] Voice parameters have been set!\n");

    espeak_SetSynthCallback(SynthCallbackC);

    std::cout << "\n>────────────────[INITIALIZED TEXT-TO-SPEECH SUCCESSULLY]───────────────<\n" << std::endl;
    Logger::GetInstance().log("\n>----------------[INITIALIZED TEXT-TO-SPEECH SUCCESSULLY]---------------<\n\n");
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
        Logger::GetInstance().log("[ERROR] espeak_Synth error: \n[ERROR] ");
        Logger::GetInstance().log(std::to_string(speakErr));
        Logger::GetInstance().log("\n");
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
            std::cerr << "[ERROR] pa_simple_write failed: " << pa_strerror(error) << std::endl;
            Logger::GetInstance().log("[ERROR] pa_simple_write failed: ");
            Logger::GetInstance().log(pa_strerror(error));
            Logger::GetInstance().log("\n");
            return 1;  // abort synthesis
        }
    }
    return 0;
}