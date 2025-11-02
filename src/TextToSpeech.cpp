#include "TextToSpeech.h"
#include "Logger.h"
#include <algorithm>
#include <fstream>
#include <cstring>
#include <portaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdlib>   // for system() & voice-fail shutdown
#include <string>
#include <imgui.h>
#include <mutex>
#include <vector>

pa_simple *pa = nullptr;
static constexpr const char* VIRT_MODS_PATH = "/tmp/virt_mods";

static inline void trim_inplace(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){ return !std::isspace(ch); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end());
}

static int pactl_load_module_get_id(const std::string &args) {
    std::string cmd = "pactl load-module " + args + " 2>/dev/null";
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp) return -1;
    char buf[256];
    std::string out;
    while (fgets(buf, sizeof(buf), fp)) out += buf;
    int rc = pclose(fp);
    // pclose return value not used; parse out
    trim_inplace(out);
    if (out.empty()) return -1;
    try {
        int id = std::stoi(out);
        return id;
    } catch (...) {
        return -1;
    }
}

static bool pactl_unload_module(int module_id) {
    if (module_id <= 0) return false;
    std::string cmd = "pactl unload-module " + std::to_string(module_id) + " 2>/dev/null";
    int rc = std::system(cmd.c_str());
    // system() returns -1 on failure to start shell; otherwise status<<8
    if (rc == -1) return false;
    int exit_status = WEXITSTATUS(rc);
    return exit_status == 0;
}

static void persist_module_id(int module_id) {
    std::ofstream f(VIRT_MODS_PATH, std::ios::app);
    if (!f.is_open()) return;
    f << module_id << "\n";
    f.close();
}

static std::vector<int> read_persisted_module_ids() {
    std::vector<int> out;
    std::ifstream f(VIRT_MODS_PATH);
    if (!f.is_open()) return out;
    std::string line;
    while (std::getline(f, line)) {
        trim_inplace(line);
        if (line.empty()) continue;
        try {
            out.push_back(std::stoi(line));
        } catch (...) { continue; }
    }
    f.close();
    return out;
}

static void remove_persisted_module_file() {
    ::unlink(VIRT_MODS_PATH); // ignore errors
}

extern "C" int SynthCallbackC(short* wav, int numsamples, espeak_EVENT* events) {
    return TextToSpeech::SynthCallback(wav, numsamples, events);
}

int TextToSpeech::Initialize() {
Settings& cfg = Settings::GetInstance();
    std::cout << "\n>─────────────────────[INITIALIZING TEXT-TO-SPEECH]─────────────────────<\n" << std::endl;
    Logger::GetInstance().log("\n>---------------------[INITIALIZING TEXT-TO-SPEECH]---------------------<\n\n");
    // ensure loopback are loaded
    // system("pactl load-module module-null-sink sink_name=VirtualSink sink_properties=device.description=konamask");
    // system("pactl load-module module-remap-source source_name=konamask master=VirtualSink.monitor source_properties=device.description=konamask");

    // load null sink
    int id1 = pactl_load_module_get_id("module-null-sink sink_name=VirtualSink sink_properties=device.description=konamask");
    if (id1 <= 0) {
        std::cerr << "[ERROR] Failed to load module-null-sink!" << std::endl;
    } else {
        persist_module_id(id1);
        std::cout << "[INFO] loaded null-sink module id=" << id1 << std::endl;
    }
    
    // load remap source (virtual microphone), points at VirtualSink.monitor
    int id2 = pactl_load_module_get_id("module-remap-source source_name=konamask master=VirtualSink.monitor source_properties=device.description=konamask");
    if (id2 <= 0) {
        std::cerr << "[ERROR] Failed to load module-remap-source!" << std::endl;
    } else {
        persist_module_id(id2);
        std::cout << "[INFO] Loaded remap-source module id=" << id2 << std::endl;
    }

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
    if (pa) {
        int pa_err = 0;
        if (pa_simple_drain(pa, &pa_err) < 0) {
            std::cerr << "[ERROR] pa_simple_drain failed: " << pa_strerror(pa_err) << std::endl;
        }
        pa_simple_free(pa);
        pa = nullptr;
    }

    auto ids = read_persisted_module_ids();
    for (int id : ids) {
        if (id <= 0) continue;
        bool ok = pactl_unload_module(id);
        if (!ok) {
            std::cerr << "[ERROR] Failed to unload pulse module id=" << id << std::endl;
        } else {
            std::cout << "[INFO] unloaded pulse module id=" << id << std::endl;
        }
    }

    remove_persisted_module_file();

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

void TextToSpeech::render() {
    static char tts_buf[4096] = {0};
    static std::mutex tts_text_mtx;
    static std::string tts_input;

    // ImGui::Begin("Text-to-Speech", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    // rate/pitch/volume controls
    int rate = Settings::GetInstance().get<int>("speech_rate", 150);
    if (ImGui::SliderInt("rate", &rate, 80, 400)) {
        Settings::GetInstance().set<int>("speech_rate", rate);
        espeak_SetParameter(espeakRATE, rate, 0);
    }
    int pitch = Settings::GetInstance().get<int>("speech_pitch", 50);
    if (ImGui::SliderInt("pitch", &pitch, 0, 99)) {
        Settings::GetInstance().set<int>("speech_pitch", pitch);
        espeak_SetParameter(espeakPITCH, pitch, 0);
    }
    int volume = Settings::GetInstance().get<int>("speech_volume", 100);
    if (ImGui::SliderInt("volume", &volume, 0, 200)) {
        Settings::GetInstance().set<int>("speech_volume", volume);
        espeak_SetParameter(espeakVOLUME, volume, 0);
    }

    // voice selection
    static char voice_buf[128] = {0};
    std::string curVoice = Settings::GetInstance().get<std::string>("speech_vociebank", "en-us");
    if (voice_buf[0] == '\0') strncpy(voice_buf, curVoice.c_str(), sizeof(voice_buf)-1);
    if (ImGui::InputText("voicebank", voice_buf, sizeof(voice_buf))) {
        // apply when changed
        if (espeak_SetVoiceByName(voice_buf) == EE_OK) {
            Settings::GetInstance().set<std::string>("speech_vociebank", std::string(voice_buf));
        }
    }

    // text input & speak button
    ImGui::Spacing();
    ImGui::Text("Manual voice output");
    ImGui::InputTextMultiline("##text", tts_buf, sizeof(tts_buf), ImVec2(ImGui::GetColumnWidth(), ImGui::GetFrameHeight()-132));

    ImGui::SetCursorPosX(ImGui::GetColumnWidth()-100);
    if (ImGui::Button("Clear")) {
        tts_buf[0] = '\0';
    }
    ImGui::SameLine();
    if (ImGui::Button("Speak")) {
        std::lock_guard<std::mutex> lk(tts_text_mtx);
        if (tts_buf[0] != '\0') TextToSpeech::Verbalize(tts_buf);
    }

    // ImGui::End();
}
