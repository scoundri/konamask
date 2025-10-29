#include "SpeechToText.h"
#include "TextToSpeech.h"
#include <cstdint>
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <cmath>
#include <vosk_api.h>
#include <portaudio.h>
#include <nlohmann/json.hpp>

InputVisualizer visualizer;

int SpeechToText::Initialize() {

    std::cout << "\n>─────────────────────[INITIALIZING SPEECH-TO-TEXT]─────────────────────<\n" << std::endl;
    // initialize vosk api
    vosk_set_log_level(0);

    
    model = vosk_model_new(cfg.get<std::string>("voskapi_model_path", "./model").c_str());
    if (!model) {
        std::cerr << "[ERROR] Failed to load Vosk-API model from \"" << cfg.get<std::string>("voskapi_model_path", "./model") << "\"!" << std::endl;
        return 1;
    }
    std::cout << "[INFO] Successfully loaded the Vosk-API model from \"" << cfg.get<std::string>("voskapi_model_path", "./model") << "\"!" << std::endl;

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
    framesPerBuffer =  int(sampleRate * cfg.get<double>("buffer_factor", 0.05));
    std::cout << "[INFO] Buffer factor has been set to  \"" << cfg.get<double>("buffer_factor", 0.05) << "\"." << std::endl;
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

    if (cfg.get<bool>("enable_user_interface", true)) {
        int fftSize = 2048; // tune if you like
        int ringSec = 2;
        if (!visualizer.Initialize(static_cast<int>(sampleRate), fftSize, ringSec)) {
            std::cerr << "[ERROR] Failed to initialize input visualizer" << std::endl;
        } else {
            std::cout << "[INFO] input visualizer initialized (sr=" << sampleRate << " fft=" << fftSize << ")" << std::endl;
        }
    }

    std::cout << "\n>────────────────[INITIALIZED SPEECH-TO-TEXT SUCCESSULLY]───────────────<\n" << std::endl;
    Run();
    return 0;
}

bool SpeechToText::ReopenStream(PaDeviceIndex device) {
    PaDeviceIndex dev = (device != paNoDevice) ? device : selectedDevice;
    if (dev == paNoDevice) dev = Pa_GetDefaultInputDevice();
    if (dev == paNoDevice) {
        std::cerr << "[ERROR] ReopenStream: no input device available!" << std::endl;
        return false;
    }

    const PaDeviceInfo *devInfo = Pa_GetDeviceInfo(dev);
    if (!devInfo) {
        std::cerr << "[ERROR] ReopenStream: Pa_GetDeviceInfo returned null!" << std::endl;
        return false;
    }

    // stop & close existing stream if open
    if (stream) {
        PaError e = Pa_IsStreamActive(stream);
        if (e == 1) Pa_StopStream(stream);
        Pa_CloseStream(stream);
        stream = nullptr;
    }

    // store selection
    selectedDevice = dev;
    sampleRate = devInfo->defaultSampleRate;

    // recompute frames based on buffer_factor
    framesPerBuffer = int(sampleRate * cfg.get<double>("buffer_factor", 0.05));
    if (framesPerBuffer <= 0) framesPerBuffer = 256; // fallback

    PaStreamParameters inputParams;
    inputParams.device = dev;
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency = devInfo->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(&stream, &inputParams, nullptr, sampleRate, framesPerBuffer, paNoFlag, nullptr, nullptr);
    if (err != paNoError) {
        std::cerr << "[ERROR] ReopenStream: Pa_OpenStream failed: " << Pa_GetErrorText(err) << std::endl;
        stream = nullptr;
        return false;
    }
    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "[ERROR] ReopenStream: Pa_StartStream failed: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(stream);
        stream = nullptr;
        return false;
    }

    std::cout << "[INFO] ReopenStream: opened device: " << devInfo->name << " @ " << sampleRate << " Hz (frames=" << framesPerBuffer << ")" << std::endl;
    stopRequested.store(false);
    return true;
}


int SpeechToText::Run() {
    std::cout << "[INFO] Listening...\n";

    std::vector<int16_t> buffer(framesPerBuffer);
    bool inSpeech = false;
    bool silenceTimerRunning = false;
    auto silenceStart = std::chrono::steady_clock::now();
    visualizer.initialized=true;

    // listen and process
    while (true) {
        if (!stopRequested.load()) {
            std::this_thread::sleep_for(std::chrono::duration<std::chrono::milliseconds>(20));
        }
        PaError err = Pa_ReadStream(stream, buffer.data(), framesPerBuffer);
        if (err && err != paInputOverflowed) break;
        visualizer.PushSamples(buffer.data(), buffer.size());


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

                    if (!j_result["text"].get<std::string>().empty()) {
                        std::cout << "[INFO] Dispatching \"" << j_result["text"].get<std::string>() << "\"..." << std::endl;
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


    return 0;
}

void SpeechToText::Shutdown() {
    // cleanup
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    vosk_recognizer_free(recognizer);
    vosk_model_free(model);

}