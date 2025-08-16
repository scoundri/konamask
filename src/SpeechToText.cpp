#include "SpeechToText.h"
#include "Logger.h"
#include <cstdint>
#include <iostream>

int SpeechToText::Initialize() {

    std::cout << "\n>─────────────────────[INITIALIZING SPEECH-TO-TEXT]─────────────────────<\n" << std::endl;
    Logger::GetInstance().log("\n>─────────────────────[INITIALIZING SPEECH-TO-TEXT]─────────────────────<\n\n");
    // initialize vosk api
    vosk_set_log_level(0);

    
    model = vosk_model_new(cfg.get<std::string>("voskapi_model_path", "./model").c_str());
    if (!model) {
        std::cerr << "[ERROR] Failed to load Vosk-API model from \"" << cfg.get<std::string>("voskapi_model_path", "./model") << "\"!" << std::endl;
        Logger::GetInstance().log("[ERROR] Failed to load Vosk-API model from \"");
        Logger::GetInstance().log(cfg.get<std::string>("voskapi_model_path", "./model"));
        Logger::GetInstance().log("\"!\n");
        return 1;
    }
    std::cout << "[INFO] Successfully loaded the Vosk-API model from \"" << cfg.get<std::string>("voskapi_model_path", "./model") << "\"!" << std::endl;
    Logger::GetInstance().log("[INFO] Successfully loaded the Vosk-API model from \"");
    Logger::GetInstance().log(cfg.get<std::string>("voskapi_model_path", "./model"));
    Logger::GetInstance().log("\"!\n");
    // initialize portaudio
    if (Pa_Initialize() != paNoError) {
        std::cout << "[ERROR] PortAudio Initialization error!" << std::endl;
        Logger::GetInstance().log("[ERROR] PortAudio Initialization error!\n");
        std::exit(EXIT_FAILURE);
    }
    std::cout << "[INFO] Successfully initialized PortAudio!" << std::endl;
    Logger::GetInstance().log("[INFO] Successfully initialized PortAudio!\n");

    PaDeviceIndex dev = Pa_GetDefaultInputDevice();
    const PaDeviceInfo *devInfo = Pa_GetDeviceInfo(dev);
    sampleRate = devInfo->defaultSampleRate;

    // small buffer -> low latency
    framesPerBuffer =  int(sampleRate * cfg.get<double>("buffer_factor", 0.05));
    std::cout << "[INFO] Buffer factor has been set to  \"" << cfg.get<double>("buffer_factor", 0.05) << "\"." << std::endl;
    Logger::GetInstance().log("[INFO] Buffer factor has been set to  \"");
    Logger::GetInstance().log(cfg.get<std::string>("buffer_factor", "0.05"));
    Logger::GetInstance().log("\".\n");
    std::cout << "[INFO] Using input device: " << devInfo->name << " @" << sampleRate << "Hz" << std::endl;
    Logger::GetInstance().log("[INFO] Using input device: ");
    Logger::GetInstance().log(devInfo->name);
    Logger::GetInstance().log(" @");
    Logger::GetInstance().log(std::to_string(sampleRate));
    Logger::GetInstance().log("Hz\n");

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
        Logger::GetInstance().log("[INFO] Failed to open stream!\n");
        return 1;
    }
    Pa_StartStream(stream);
    std::cout << "[INFO] Successfully oppened the PortAudio stream!" << std::endl;
    Logger::GetInstance().log("[INFO] Successfully oppened the PortAudio stream!\n");

    std::cout << "\n>────────────────[INITIALIZED SPEECH-TO-TEXT SUCCESSULLY]───────────────<\n" << std::endl;
    Logger::GetInstance().log("\n>────────────────[INITIALIZED SPEECH-TO-TEXT SUCCESSULLY]───────────────<\n\n");
    Run();
    return 0;
}

int SpeechToText::Run() {

    std::cout << "[INFO] Listening..." << std::endl;
    Logger::GetInstance().log("[INFO] Listening...\n");

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
                
                if (j_result["text"].get<std::string>().compare("")) { // when done with other stuff, test with j_result["text"].get<std::string>().compare("")
                    std::cout << "[INFO] Dispatching \"" << j_result["text"].get<std::string>() << "\"..." << std::endl;
                    TextToSpeech::Verbalize(j_result["text"].get<std::string>().c_str());
                    std::cout << "[INFO] Dispatched content successfully!" << std::endl;
                    Logger::GetInstance().log("[INFO] Dispatched \"");
                    Logger::GetInstance().log(j_result["text"].get<std::string>());
                    Logger::GetInstance().log("\" successfully!\n");

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
