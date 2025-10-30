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
    Start();
    return 0;
}

void SpeechToText::Start() {
    if (workerRunning.exchange(true)) {
        return;
    }

    if (workerThread.joinable()) {
        try {
            workerThread.join();
        } catch (...) {
        }
    }

    try {
        workerThread = std::thread(&SpeechToText::WorkerLoop, this);
    } catch (...) {
        try {
            WorkerLoop();
        } catch (...) { 
            workerRunning.store(false);
            Shutdown();
            throw "worker thread (SpeechToText::workerThread) threw inside SpeechToText::Start()";
        }
    }
}

bool SpeechToText::ReopenInputStream() {
    std::lock_guard<std::mutex> lk(streamMutex);

    // close existing
    if (stream) {
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        stream = nullptr;
    }

    PaDeviceIndex dev = Pa_GetDefaultInputDevice();
    if (dev == paNoDevice) {
        std::cerr << "[ERROR] reopenInputStream: no default input device!" << std::endl;
        return false;
    }
    const PaDeviceInfo* devInfo = Pa_GetDeviceInfo(dev);
    if (!devInfo) {
        std::cerr << "[ERROR] reopenInputStream: Pa_GetDeviceInfo returned null!" << std::endl;
        return false;
    }

    // ensure framesPerBuffer is reasonable
    if (framesPerBuffer <= 0) framesPerBuffer = 512;

    PaStreamParameters inputParams;
    inputParams.device = dev;
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency = devInfo->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(&stream, &inputParams, nullptr, sampleRate,
                               static_cast<unsigned long>(framesPerBuffer),
                               paNoFlag, nullptr, nullptr);
    if (err != paNoError) {
        std::cerr << "[ERROR] ReopenInputStream: Pa_OpenStream failed: " << Pa_GetErrorText(err) << std::endl;
        stream = nullptr;
        return false;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "[ERROR] ReopenInputStream: Pa_StartStream failed: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(stream);
        stream = nullptr;
        return false;
    }

    std::cout << "[INFO] ReopenInputStream: opened device '" << (devInfo->name ? devInfo->name : "unknown")
              << "' sr=" << sampleRate << " frames=" << framesPerBuffer << std::endl;
    return true;
}

static bool testDeviceOpenAndRead(PaDeviceIndex dev, unsigned long framesPerBuffer, double requestedSampleRate) {
    PaStream *tmp = nullptr;
    const PaDeviceInfo *di = Pa_GetDeviceInfo(dev);
    if (!di) return false;

    PaStreamParameters inParams;
    std::memset(&inParams, 0, sizeof(inParams));
    inParams.device = dev;
    inParams.channelCount = 1;
    inParams.sampleFormat = paInt16; // test int16 first
    inParams.suggestedLatency = di->defaultLowInputLatency;
    inParams.hostApiSpecificStreamInfo = nullptr;

    // reasonable sr
    double sr = (requestedSampleRate > 0.0) ? requestedSampleRate : di->defaultSampleRate;
    if (sr <= 0) sr = 48000.0;

    // open/read as int16
    PaError err = Pa_OpenStream(&tmp, &inParams, nullptr, sr, framesPerBuffer, paNoFlag, nullptr, nullptr);
    if (err == paNoError) {
        err = Pa_StartStream(tmp);
        if (err == paNoError) {
            std::vector<int16_t> buf(framesPerBuffer);
            PaError r = Pa_ReadStream(tmp, buf.data(), framesPerBuffer);
            // close
            Pa_StopStream(tmp);
            Pa_CloseStream(tmp);
            tmp = nullptr;
            if (r == paNoError || r == paInputOverflowed) return true;
            // fallthrough to float
        } else {
            Pa_CloseStream(tmp);
            tmp = nullptr;
        }
    }

    // float32 fallback
    inParams.sampleFormat = paFloat32;
    err = Pa_OpenStream(&tmp, &inParams, nullptr, sr, framesPerBuffer, paNoFlag, nullptr, nullptr);
    if (err != paNoError) {
        if (tmp) { Pa_CloseStream(tmp); tmp = nullptr; }
        return false;
    }
    err = Pa_StartStream(tmp);
    if (err != paNoError) {
        Pa_CloseStream(tmp);
        return false;
    }
    {
        std::vector<float> fb(framesPerBuffer);
        PaError r = Pa_ReadStream(tmp, fb.data(), framesPerBuffer);
        Pa_StopStream(tmp);
        Pa_CloseStream(tmp);
        tmp = nullptr;
        return (r == paNoError || r == paInputOverflowed);
    }
}

static PaDeviceIndex findWorkingDeviceAuto(unsigned long framesPerBuffer, double requestedSampleRate) {
    int devCount = Pa_GetDeviceCount();
    if (devCount < 0) return paNoDevice;

    // try system default
    PaDeviceIndex def = Pa_GetDefaultInputDevice();
    if (def != paNoDevice && testDeviceOpenAndRead(def, framesPerBuffer, requestedSampleRate)) {
        return def;
    }

    for (int i = 0; i < devCount; ++i) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
        if (!di) continue;
        if (di->maxInputChannels <= 0) continue;
        if (static_cast<PaDeviceIndex>(i) == def) continue;
        if (testDeviceOpenAndRead(static_cast<PaDeviceIndex>(i), framesPerBuffer, requestedSampleRate)) {
            return static_cast<PaDeviceIndex>(i);
        }
    }
    return paNoDevice;
}

bool SpeechToText::SwitchToDevice(PaDeviceIndex newDev) {
    if (newDev == paNoDevice) return false;
    std::lock_guard<std::mutex> lk(streamMutex);
    // close old stream
    if (stream) {
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        stream = nullptr;
    }

    const PaDeviceInfo *di = Pa_GetDeviceInfo(newDev);
    if (!di) return false;
    sampleRate = di->defaultSampleRate;

    // open stream
    PaStreamParameters inParams;
    inParams.device = newDev;
    inParams.channelCount = 1;
    inParams.sampleFormat = paInt16;
    inParams.suggestedLatency = di->defaultLowInputLatency;
    inParams.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(&stream, &inParams, nullptr, sampleRate,
                               static_cast<unsigned long>(framesPerBuffer), paNoFlag, nullptr, nullptr);
    if (err != paNoError) {
        std::cerr << "[ERROR] switchToDevice: Pa_OpenStream failed: " << Pa_GetErrorText(err) << std::endl;
        stream = nullptr;
        return false;
    }
    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "[ERROR] switchToDevice: Pa_StartStream failed: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(stream);
        stream = nullptr;
        return false;
    }
    std::cout << "[INFO] switched to device '" << (di->name?di->name:"unknown") << "' sr=" << sampleRate << std::endl;
    return true;
}


void SpeechToText::Stop() {
    if (!workerRunning.load()) return;
    workerRunning.store(false);
    controlCv.notify_all();
    if (workerThread.joinable()) workerThread.join();
}

void SpeechToText::SwitchMode(ProcessingMode m) {
    currentMode.store(m);
    // wake worker
    controlCv.notify_one();
}

void SpeechToText::RequestRestartInput() {
    restartRequested.store(true);
    controlCv.notify_one();
}

void SpeechToText::WorkerLoop() {
    // local buffers
    std::vector<int16_t> buffer;
    buffer.resize(framesPerBuffer);

    std::string lastPartial;
    size_t lastSpokenCount = 0;

    int backoffMs = 50;
    const int maxBackoffMs = 5000;

    auto split_words = [](const std::string &s) {
        std::vector<std::string> w;
        std::istringstream iss(s);
        std::string tok;
        while (iss >> tok) w.push_back(tok);
        return w;
    };

    while (workerRunning.load()) {
        // restart request handling
        if (restartRequested.load()) {
            std::unique_lock<std::mutex> lk(streamMutex);
            if (!stream || Pa_IsStreamActive(stream) <= 0) {
                lk.unlock();
                if (!ReopenInputStream()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                    backoffMs = std::min(maxBackoffMs, backoffMs * 2);
                    continue;
                }
            backoffMs = 50;
            }
        }
        PaError err;
        {
            std::lock_guard<std::mutex> lk(streamMutex);
            if (!stream) { continue; }
            err = Pa_ReadStream(stream, buffer.data(), static_cast<unsigned long>(framesPerBuffer));
            if (err && err != paInputOverflowed) {
            std::cerr << "[ERROR] Pa_ReadStream returned " << err << " (" << Pa_GetErrorText(err) << ")" << std::endl;
        
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
            // try auto-discovery of a working device
            PaDeviceIndex wokring = findWorkingDeviceAuto(static_cast<unsigned long>(framesPerBuffer), sampleRate);
            if (wokring != paNoDevice) {
                std::cout << "[INFO] Auto-found working device: " << wokring << " - switching..." << std::endl;
                if (SwitchToDevice(wokring)) {
                    continue; // attempt next read on new device
                } else {
                    std::cerr << "[ERROR] Failed to switch to auto-found device!" << std::endl;
                }
            }
        
            }
        }


        visualizer.PushSamples(buffer.data(), buffer.size());

        // dispatch
        ProcessingMode mode = currentMode.load();
        if (mode == ProcessingMode::FinalOnSilence) { // call handler
            ProcessBuffer_Final(buffer);
        } else {
            ProcessBuffer_Partial(buffer);
        }
        std::unique_lock<std::mutex> lk(streamMutex);
        controlCv.wait_for(lk, std::chrono::milliseconds(1), [&](){ return !workerRunning.load() || restartRequested.load() || false;});
    }

    // final cleanup
    {
        std::unique_lock<std::mutex> lk(streamMutex);
        if (stream) {
            Pa_StopStream(stream);
            Pa_CloseStream(stream);
            stream = nullptr;
        }
    }
}

void SpeechToText::ProcessBuffer_Final(const std::vector<int16_t>& buffer) {
    // feed recognizer
    vosk_recognizer_accept_waveform(recognizer,
        reinterpret_cast<const char*>(buffer.data()),
        static_cast<int>(buffer.size() * sizeof(int16_t)));

    // VAD + final-on-silence
    static bool inSpeech = false;
    static auto silenceStart = std::chrono::steady_clock::now();
    int maxAmp = 0;
    for (auto s : buffer) maxAmp = std::max(maxAmp, std::abs((int)s));

    if (maxAmp > SILENCE_THRESHOLD) {
        if (!inSpeech) {
            inSpeech = true;
            vosk_recognizer_reset(recognizer); // reset on new speech
        }
    } else if (inSpeech) {
        // start timer or finalize
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - silenceStart).count();
        if (elapsed >= SILENCE_TIMEOUT_MS) {
            const char* final_c = vosk_recognizer_final_result(recognizer);
            if (final_c && final_c[0]) {
                // parse and speak full text
                auto j = nlohmann::json::parse(final_c);
                auto txt = j.value("text", std::string());
                if (!txt.empty()) {
                    std::lock_guard<std::mutex> lk(tts_mtx);
                    TextToSpeech::Verbalize(txt.c_str());
                }
            }
            inSpeech = false;
        }
    } else {
        silenceStart = std::chrono::steady_clock::now();
    }
}

void SpeechToText::ProcessBuffer_Partial(const std::vector<int16_t>& buffer) {
    // feed recognizer
    vosk_recognizer_accept_waveform(recognizer,
        reinterpret_cast<const char*>(buffer.data()),
        static_cast<int>(buffer.size() * sizeof(int16_t)));

    // query partial
    const char* partial_c = vosk_recognizer_partial_result(recognizer);
    if (!partial_c || partial_c[0] == '\0') return;

    try {
        auto pj = nlohmann::json::parse(partial_c);
        std::string ptxt = pj.value("partial", std::string());
        // split words
        static std::vector<std::string> spokenWords;
        // if ptxt shorter than previous, assume recognizer backtracked -> clear
        if (ptxt.size() < std::accumulate(spokenWords.begin(), spokenWords.end(), 0,
                                          [](int a, const std::string &s){ return a + (int)s.size() + 1; })) {
            spokenWords.clear();
        }
        std::istringstream iss(ptxt);
        std::vector<std::string> words;
        std::string w;
        while (iss >> w) words.push_back(w);

        // speak new input (words)
        size_t already = spokenWords.size();
        if (words.size() > already) {
            for (size_t i = already; i < words.size(); ++i) {
                std::lock_guard<std::mutex> lk(tts_mtx);
                TextToSpeech::Verbalize(words[i].c_str());
            }
            spokenWords = words;
        }
    } catch (...) {
        // ignore errors
    }
}

void SpeechToText::Shutdown() {
    Stop();
    {
        std::lock_guard<std::mutex> lk(streamMutex);
        if (stream) {
            Pa_StopStream(stream);
            Pa_CloseStream(stream);
            stream = nullptr;
        }
    }
    Pa_Terminate();
    if (recognizer) {
        vosk_recognizer_free(recognizer);
        recognizer = nullptr;
    }
    if (model) {
        vosk_model_free(model);
        model = nullptr;
    }
}

SpeechToText::~SpeechToText() {
    Stop();
    Shutdown();
}