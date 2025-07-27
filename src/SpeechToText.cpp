#include "SpeechToText.h"
#include "TextToSpeech.h"
#include <cstddef>
#include <cstdint>
#include <iostream>

int SpeechToText::Initialize() {
Settings& cfg = Settings::GetInstance();

    std::cout << "\n>─────────────────────[INITIALIZING SPEECH-TO-TEXT]─────────────────────<\n" << std::endl;
    // initialize vosk api
    vosk_set_log_level(0);
    model = vosk_model_new(cfg.VOSK_MODEL_PATH);
    if (!model) {
        std::cerr << "[ERROR] Failed to load Vosk-API model from \"" << cfg.VOSK_MODEL_PATH << "\"!" << std::endl;
        return 1;
    }
    std::cout << "[INFO] Successfully loaded the Vosk-API model from \"" << cfg.VOSK_MODEL_PATH << "\"!" << std::endl;

    // initialize portaudio
    if (Pa_Initialize() != paNoError) {
        std::cout << "[ERROR] PortAudio Initialization error!" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    std::cout << "[INFO] Successfully initialized PortAudio!" << std::endl;

    PaDeviceIndex dev = Pa_GetDefaultInputDevice();
    const PaDeviceInfo *devInfo = Pa_GetDeviceInfo(dev);
    sampleRate = devInfo->defaultSampleRate;

    // small buffer -> low latency
    framesPerBuffer =  int(sampleRate * cfg.BUFFER_FACTOR);

    std::cout << "[INFO] Using input device: " << devInfo->name << " @" << sampleRate << "Hz" << std::endl;

    recognizer = vosk_recognizer_new(model, sampleRate);
    vosk_recognizer_set_max_alternatives(recognizer, 0);
    vosk_recognizer_set_words(recognizer, true);

    PaStreamParameters inputParams;
    inputParams.device = dev;
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency = devInfo->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    if (Pa_OpenStream(&stream, &inputParams, nullptr, sampleRate,
                      framesPerBuffer, paNoFlag, nullptr, nullptr) != paNoError) {
        std::cerr << "[INFO] Failed to open stream!" << std::endl;
        return 1;
    }
    Pa_StartStream(stream);
    std::cout << "[INFO] Successfully oppened the PortAudio stream!" << std::endl;

    //clean-up
    cfg.VOSK_MODEL_PATH = NULL;
    cfg.BUFFER_FACTOR = NULL;
    std::cout << "[INFO] Cleared unnecessary memory!" << std::endl;

    std::cout << "\n>────────────────[INITIALIZED SPEECH-TO-TEXT SUCCESSULLY]───────────────<\n" << std::endl;
    Run();
    return 0;
}

int SpeechToText::Run() {

    std::cout << "[INFO] Listening...\n";

    std::vector<int16_t> buffer(framesPerBuffer);
    bool inSpeech = false;
    bool silenceTimerRunning = false;
    auto silenceStart = std::chrono::steady_clock::now();
    
    // listen and process
    while (true) {
        PaError err = Pa_ReadStream(stream, buffer.data(), framesPerBuffer);
        if (err && err != paInputOverflowed) break;

        // voice activity detection
        int16_t maxAmp = 0;
        for (auto &s : buffer) maxAmp = std::max<int16_t>(maxAmp, std::abs(s));
        if (maxAmp > SILENCE_THRESHOLD) {
            if (!inSpeech) {
                inSpeech = true;
                vosk_recognizer_reset(recognizer);
            }
            silenceTimerRunning = false;
        } else if (inSpeech) {
            if (!silenceTimerRunning) {
                silenceTimerRunning = true;
                silenceStart = std::chrono::steady_clock::now();
            }
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - silenceStart).count();
            if (elapsed >= SILENCE_TIMEOUT_MS) {
                // finalize utterance
                j_result = nlohmann::json::parse(vosk_recognizer_final_result(recognizer));
                // debug output
                //std::cout << "\n──────────── STT DEBUG ────────────" << std::endl;
                //const char* result = vosk_recognizer_final_result(recognizer);
                //std::cout << result << std::endl;
                
                if (j_result["text"].get<std::string>().c_str() != "") {
                    TextToSpeech::Verbalize(j_result["text"].get<std::string>().c_str());
                    std::cout << "[INFO] Dispatched content successfully!" << std::endl;
                }
                inSpeech = false;
                silenceTimerRunning = false;
            }
        }

        // always feed output
        vosk_recognizer_accept_waveform(recognizer,
            reinterpret_cast<const char *>(buffer.data()),
            framesPerBuffer * sizeof(int16_t));
    }


    // cleanup
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    vosk_recognizer_free(recognizer);
    vosk_model_free(model);

    return 0;
}
