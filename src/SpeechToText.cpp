#include "SpeechToText.h"
#include "Interface.h"
#include "Logger.h"
#include <cstdint>
#include <iostream>
#include <thread>
#include <vosk_api.h>

static InputVisualizer visualizer;
static Interface ui;

struct PaDevInfo {
    int index;
    std::string name;
    int maxInputs;
    double defaultSampleRate;
};

struct DeviceManager {
    PaStream* stream = nullptr;
    PaDeviceIndex currentDevice = paNoDevice;
    PaSampleFormat activeFormat = paInt16;
    int activeChannels = 1;
    double activeSampleRate = 48000.0;
    unsigned long framesPerBuffer = 512;

    std::vector<PaDevInfo> devices;

    // populate device list (call after Pa_Initialize)
    void enumerateDevices() {
        devices.clear();
        int cnt = Pa_GetDeviceCount();
        for (int i = 0; i < cnt; ++i) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
            if (!info) continue;
            PaDevInfo d;
            d.index = i;
            d.name = info->name ? info->name : std::string("unknown");
            d.maxInputs = info->maxInputChannels;
            d.defaultSampleRate = info->defaultSampleRate;
            devices.push_back(d);
        }
    }

    PaDeviceIndex pickDefault() {
        PaDeviceIndex def = Pa_GetDefaultInputDevice();
        if (def != paNoDevice) {
            const PaDeviceInfo* di = Pa_GetDeviceInfo(def);
            if (di && di->maxInputChannels > 0) {
                return def;
            }
        }
        for (auto &d : devices) if (d.maxInputs > 0) return d.index;
        return paNoDevice;
    }

    bool openStream(PaDeviceIndex dev, unsigned long requestedFramesPerBuffer, double requestedSampleRate = 0.0) {
        if (stream) {
            Pa_StopStream(stream);
            Pa_CloseStream(stream);
            stream = nullptr;
        }
        if (dev == paNoDevice) return false;
        const PaDeviceInfo* di = Pa_GetDeviceInfo(dev);
        if (!di) return false;

        // choose sample rate
        double sr = (requestedSampleRate > 0.0) ? requestedSampleRate : di->defaultSampleRate;
        framesPerBuffer = requestedFramesPerBuffer;
        // int16 first (float32 fallback)
        PaStreamParameters inParams;
        std::memset(&inParams, 0, sizeof(inParams));
        inParams.device = dev;
        // choose capture channels
        int tryChannels = std::max(1, di->maxInputChannels);
        inParams.channelCount = tryChannels;
        inParams.hostApiSpecificStreamInfo = nullptr;
        inParams.suggestedLatency = di->defaultLowInputLatency;

        // attempt int16
        inParams.sampleFormat = paInt16;
        PaError e = Pa_IsFormatSupported(&inParams, nullptr, sr);
        if (e == paFormatIsSupported) {
            activeFormat = paInt16;
            activeChannels = tryChannels;
            activeSampleRate = sr;
        } else {
            // try float32 (common with PipeWire)
            inParams.sampleFormat = paFloat32;
            e = Pa_IsFormatSupported(&inParams, nullptr, sr);
            if (e == paFormatIsSupported) {
                activeFormat = paFloat32;
                activeChannels = tryChannels;
                activeSampleRate = sr;
            } else {
                // fallback (try to open playback with int16 - PA does conversion)
                activeFormat = paInt16;
                activeChannels = std::min(1, tryChannels);
                activeSampleRate = sr;
            }
        }

        // open stream with parameters
        PaStreamParameters finalParams;
        std::memset(&finalParams, 0, sizeof(finalParams));
        finalParams.device = dev;
        finalParams.channelCount = activeChannels;
        finalParams.sampleFormat = activeFormat;
        finalParams.suggestedLatency = di->defaultLowInputLatency;
        finalParams.hostApiSpecificStreamInfo = nullptr;

        PaError openErr = Pa_OpenStream(&stream,
                                       &finalParams,
                                       nullptr,              // no output
                                       activeSampleRate,
                                       framesPerBuffer,
                                       paClipOff,
                                       nullptr,
                                       nullptr);
        if (openErr != paNoError) {
            std::cerr << "[ERROR] Pa_OpenStream failed: " << Pa_GetErrorText(openErr) << " (" << openErr << ")\n";
            stream = nullptr;
            return false;
        }
        PaError startErr = Pa_StartStream(stream);
        if (startErr != paNoError) {
            std::cerr << "[ERROR] Pa_StartStream failed: " << Pa_GetErrorText(startErr) << " (" << startErr << ")\n";
            Pa_CloseStream(stream);
            stream = nullptr;
            return false;
        }
        currentDevice = dev;
        std::cout << "[INFO] opened input device '" << (di->name ? di->name : "unknown")
                  << "' sr=" << activeSampleRate
                  << " ch=" << activeChannels
                  << " fmt=" << ((activeFormat == paFloat32) ? "float32" : "int16")
                  << " frames=" << framesPerBuffer << "\n";
        return true;
    }

    // close stream safely
    void closeStream() {
        if (!stream) return;
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        stream = nullptr;
        currentDevice = paNoDevice;
    }

    template<typename VisualizerType>
    bool readAndProcess(std::vector<int16_t> &outMono, VisualizerType &visualizer, VoskRecognizer* vosk_recognizer, size_t framesPerBufferLocal) {
        if (!stream) return false;
        // prepare storage sized to framesPerBufferLocal
        outMono.assign(framesPerBufferLocal, 0);

        if (activeFormat == paInt16) {
            std::vector<int16_t> buf(framesPerBufferLocal * activeChannels);
            PaError r = Pa_ReadStream(stream, buf.data(), framesPerBufferLocal);
            if (r && r != paInputOverflowed) {
                std::cerr << "[ERROR] Pa_ReadStream int16: " << Pa_GetErrorText(r) << " (" << r << ")\n";
                return false;
            }
            // downmix if multi-channel
            if (activeChannels == 1) {
                // copy directly
                std::copy(buf.begin(), buf.begin() + framesPerBufferLocal, outMono.begin());
            } else {
                for (size_t i = 0; i < framesPerBufferLocal; ++i) {
                    int sum = 0;
                    for (int c = 0; c < activeChannels; ++c) sum += static_cast<int>(buf[i * activeChannels + c]);
                    sum /= activeChannels;
                    outMono[i] = static_cast<int16_t>(std::clamp(sum, -32768, 32767));
                }
            }
        } else { // float32
            std::vector<float> fbuf(framesPerBufferLocal * activeChannels);
            PaError r = Pa_ReadStream(stream, fbuf.data(), framesPerBufferLocal);
            if (r && r != paInputOverflowed) {
                std::cerr << "[ERROR] Pa_ReadStream float: " << Pa_GetErrorText(r) << " (" << r << ")\n";
                return false;
            }
            if (activeChannels == 1) {
                for (size_t i = 0; i < framesPerBufferLocal; ++i) {
                    float v = fbuf[i];
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    outMono[i] = static_cast<int16_t>(v * 32767.0f);
                }
            } else {
                for (size_t i = 0; i < framesPerBufferLocal; ++i) {
                    double sum = 0.0;
                    for (int c = 0; c < activeChannels; ++c) sum += fbuf[i * activeChannels + c];
                    double avg = sum / activeChannels;
                    if (avg > 1.0) avg = 1.0;
                    if (avg < -1.0) avg = -1.0;
                    outMono[i] = static_cast<int16_t>(avg * 32767.0);
                }
            }
        }

        // feed vosk (mono int16)
        if (vosk_recognizer) {
            vosk_recognizer_accept_waveform(vosk_recognizer,
                                           reinterpret_cast<const char*>(outMono.data()),
                                           static_cast<int>(outMono.size() * sizeof(int16_t)));
        }
        // push to visualizer (mono int16)
        visualizer.PushSamples(outMono.data(), outMono.size());
        return true;
    }
    void showDeviceSelectorUI() {

    }
};

DeviceManager dm;

bool ui_prompt(std::string output) {
    auto fut = ui.prompt_user_async(output);
    bool accepted = fut.get(); // wait till callback
    if (accepted) {
        return true;
    } else {
        return false;
    }

// std::thread t([ui = &ui_, fe = fm_, output ]{ // bricks ui
//     auto fut = ui->prompt_user_async(output);
//     bool accepted = fut.get(); // blocks thread until UI responds
//     if (accepted) {
//         return true;
//     } else {
//         return false;
//     }
// });
//     t.detach();
    // return false;
}

std::string ui_prompt_str(std::string output) {
    auto fut = ui.prompt_user_async_string(output);
    return fut.get();
    // std::string value = fut.get();
    // if (value.empty()) {
    //     return NULL;
    // } else {
    //     return value;
    // }
}


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

    dm.enumerateDevices();
    PaDeviceIndex dev = dm.pickDefault();
    if (dev == paNoDevice) {
        std::cerr << "[ERROR] No default input device found!" << std::endl;
        Logger::GetInstance().log("[ERROR] No default input device found!\n");
        Pa_Terminate();
        return 1;
    }
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
    Logger::GetInstance().log(" @ ");
    Logger::GetInstance().log(std::to_string(sampleRate));
    Logger::GetInstance().log(" Hz\n");

    recognizer = vosk_recognizer_new(model, sampleRate);
    vosk_recognizer_set_max_alternatives(recognizer, 0);
    vosk_recognizer_set_words(recognizer, true);

    dm.openStream(dev, framesPerBuffer, sampleRate);
    // PaStreamParameters inputParams;
    // inputParams.device = dev;
    // inputParams.channelCount = 1;
    // inputParams.sampleFormat = paInt16;
    // inputParams.suggestedLatency = devInfo->defaultLowInputLatency;
    // inputParams.hostApiSpecificStreamInfo = nullptr;

    // if (Pa_OpenStream(&stream, &inputParams, nullptr, sampleRate,
    //                   framesPerBuffer, paNoFlag, nullptr, nullptr) != paNoError) {
    //     std::cerr << "[INFO] Failed to open stream!" << std::endl;
    //     Logger::GetInstance().log("[INFO] Failed to open stream!\n");
    //     return 1;
    // }
    // Pa_StartStream(stream);
    // if (cfg.get<bool>("enable_user_interface", false)) {
    //     visualizer.Initialize(sampleRate, 2048,2);
    //     std::cout << "[INFO] Input visualizer has been initialized!" << std::endl;
    //     Logger::GetInstance().log("[INFO] Input visualizer has been initialized!");
    // }
    std::cout << "[INFO] Successfully oppened the PortAudio stream!" << std::endl;
    Logger::GetInstance().log("[INFO] Successfully oppened the PortAudio stream!\n");

    std::cout << "\n>────────────────[INITIALIZED SPEECH-TO-TEXT SUCCESSULLY]───────────────<\n" << std::endl;
    Logger::GetInstance().log("\n>────────────────[INITIALIZED SPEECH-TO-TEXT SUCCESSULLY]───────────────<\n\n");
    Run();
    return 0;
}

static void dump_portaudio_devices() {
    int apiCount = Pa_GetHostApiCount();
    for (int api = 0; api < apiCount; ++api) {
        const PaHostApiInfo* apiInfo = Pa_GetHostApiInfo(api);
        if (!apiInfo) continue;
        std::cout << "[INFO] (PA) hostapi " << api << " : " << apiInfo->name << " (devices: " << apiInfo->deviceCount << ")\n";
    }
    int devCount = Pa_GetDeviceCount();
    for (int i = 0; i < devCount; ++i) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
        if (!di) continue;
        std::cout << "[INFO] (PA) Device " << i << " : " << di->name
                  << " (inputs: " << di->maxInputChannels << ", defaultSampleRate: " << di->defaultSampleRate << ")\n";
    }
}

static bool device_supports_format(PaDeviceIndex dev, double sampleRate, PaSampleFormat fmt, int channels) {
    PaStreamParameters p;
    p.device = dev;
    p.channelCount = channels;
    p.sampleFormat = fmt;
    const PaDeviceInfo* info = Pa_GetDeviceInfo(dev);
    if (!info) return false;
    p.suggestedLatency = info->defaultLowInputLatency;
    p.hostApiSpecificStreamInfo = nullptr;
    PaError e = Pa_IsFormatSupported(&p, nullptr, sampleRate);
    if (e == paFormatIsSupported) return true;
    std::cerr << "[WARN] Pa_IsFormatSupported returned: " << Pa_GetErrorText(e) << "\n";
    return false;
}

int SpeechToText::Run() {
    std::cout << "[INFO] Listening..." << std::endl;
    Logger::GetInstance().log("[INFO] Listening...\n");

    std::vector<int16_t> buffer(framesPerBuffer);
    bool inSpeech = false;
    bool silenceTimerRunning = false;
    auto silenceStart = std::chrono::steady_clock::now();
    
    // diagnostics counters
    size_t frames_with_nonzero = 0;
    size_t frames_read = 0;

    // listen and process
    while (true) {
        // PaError err = Pa_ReadStream(stream, buffer.data(), framesPerBuffer);
        // if (err == paInputOverflowed) {
        //     // input overflow — not fatal, log and continue
        //     std::cerr << "[WARN] Pa_ReadStream: input overflow\n";
        //     // optionally zero-fill buffer so we don't feed garbage
        //     std::fill(buffer.begin(), buffer.end(), 0);
        // } else if (err == paNoError) {
        //     // good
        // } else {
        //     // fatal/unknown error - print and break
        //     std::cerr << "[ERROR] Pa_ReadStream failed: " << Pa_GetErrorText(err) << " (" << err << ")\n";
        //     break;
        // }
            bool ok = dm.readAndProcess(visualizer, recognizer, framesPerBuffer);
    if (!ok) {
        dm.closeStream();
        std::cerr << "[ERROR] stream read failed. re-enumerating devices.\n";
        dm.enumerateDevices();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // try reopen default
        PaDeviceIndex fallback = dm.pickDefault();
        if (fallback != paNoDevice) dm.openStream(fallback, framesPerBuffer, sampleRate);
        continue;
    }

        int maxAmp = 0;
        for (auto s : buffer) {
            int v = std::abs(static_cast<int>(s));
            if (v > maxAmp) maxAmp = v;
        }
        if (maxAmp > 0) frames_with_nonzero++;

        // voice activity detection
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
                j_result = nlohmann::json::parse(vosk_recognizer_final_result(recognizer));
                if (!j_result["text"].get<std::string>().empty()) {
                    std::cout << "[INFO] Dispatching \"" << j_result["text"].get<std::string>() << "\"...\n";
                    TextToSpeech::Verbalize(j_result["text"].get<std::string>().c_str());
                }
                inSpeech = false;
                silenceTimerRunning = false;
            }
        }

        PaError accErr = vosk_recognizer_accept_waveform(recognizer,
            reinterpret_cast<const char *>(buffer.data()),
            static_cast<int>(framesPerBuffer * sizeof(int16_t)));
            
        visualizer.PushSamples(buffer.data(), buffer.size());

        frames_read++;
        
    }


    // cleanup
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    vosk_recognizer_free(recognizer);
    vosk_model_free(model);

    return 0;
}
