#include "SpeechToText.h"
#include "Interface.h"
#include "Logger.h"
#include <cstdint>
#include <iostream>
#include <thread>
#include <fstream>
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

struct InputDebugger {
    std::thread th;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};

    // configuration
    int deviceIndex = -1;                 // default input device
    std::string dumpPath;                 // empty = no dump
    int runSeconds = 0;                   // run until stop()
    double sampleRate = 48000.0;
    int framesPerBuffer = 512;

    // internals
    PaStream* inStream = nullptr;
    PaStream* outStream = nullptr;

    // wav dumper
    struct Wav {
        std::ofstream f;
        uint32_t data_bytes = 0;
        int sr = 48000;
        int ch = 1;
        bool open(const std::string &path, int sampleRate, int channels) {
            sr = sampleRate; ch = channels;
            f.open(path, std::ios::binary);
            if (!f.is_open()) return false;
            f.write("RIFF",4);
            uint32_t cz = 0; f.write(reinterpret_cast<const char*>(&cz),4);
            f.write("WAVE",4);
            f.write("fmt ",4);
            uint32_t sub1 = 16; f.write(reinterpret_cast<const char*>(&sub1),4);
            uint16_t audioFmt = 1; f.write(reinterpret_cast<const char*>(&audioFmt),2);
            uint16_t numCh = static_cast<uint16_t>(ch); f.write(reinterpret_cast<const char*>(&numCh),2);
            uint32_t srate = static_cast<uint32_t>(sr); f.write(reinterpret_cast<const char*>(&srate),4);
            uint16_t bits = 16;
            uint32_t byteRate = sr * ch * (bits/8); f.write(reinterpret_cast<const char*>(&byteRate),4);
            uint16_t blockAlign = ch * (bits/8); f.write(reinterpret_cast<const char*>(&blockAlign),2);
            f.write(reinterpret_cast<const char*>(&bits),2);
            f.write("data",4);
            uint32_t ds = 0; f.write(reinterpret_cast<const char*>(&ds),4);
            data_bytes = 0;
            return true;
        }
        void write_s16(const int16_t* s, size_t n) {
            if (!f.is_open()) return;
            f.write(reinterpret_cast<const char*>(s), n * sizeof(int16_t));
            data_bytes += static_cast<uint32_t>(n * sizeof(int16_t));
        }
        void close() {
            if (!f.is_open()) return;
            f.seekp(4, std::ios::beg);
            uint32_t riff = 36 + data_bytes; f.write(reinterpret_cast<const char*>(&riff),4);
            f.seekp(40, std::ios::beg);
            uint32_t db = data_bytes; f.write(reinterpret_cast<const char*>(&db),4);
            f.close();
        }
    } wav;

    // debugger thread
    bool start(int deviceIndex_ = -1,
               double sampleRate_ = 48000.0,
               int framesPerBuffer_ = 512,
               const std::string &dumpPath_ = "",
               int runSeconds_ = 0) {
        if (running.load()) return false; // already running
        deviceIndex = deviceIndex_;
        sampleRate = sampleRate_;
        framesPerBuffer = framesPerBuffer_;
        dumpPath = dumpPath_;
        runSeconds = runSeconds_;
        stopRequested.store(false);
        running.store(true);
        th = std::thread(&InputDebugger::debugger_start, this);
        return true;
    }

    // request stop and wait for the thread to finish
    void stop() {
        if (!running.load()) return;
        stopRequested.store(true);
        if (th.joinable()) th.join();
        running.store(false);
    }

    // ensure thread stopped
    ~InputDebugger() {
        stopRequested.store(true);
        if (th.joinable()) th.join();
    }

private:
    // clamp convert
    static inline int16_t float_to_s16(float v) {
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        return static_cast<int16_t>(v * 32767.0f);
    }

    void debugger_start() {
        // pick devices
        PaDeviceIndex inDev = (deviceIndex >= 0) ? static_cast<PaDeviceIndex>(deviceIndex) : Pa_GetDefaultInputDevice();
        if (inDev == paNoDevice) {
            std::cerr << "[ERROR] <DEBUG> No input device available for debugger" << std::endl;
            running.store(false);
            return;
        }
        const PaDeviceInfo* inInfo = Pa_GetDeviceInfo(inDev);
        if (!inInfo) {
            std::cerr << "[ERROR] <DEBUG> Pa_GetDeviceInfo(inDev) null" << std::endl;
            running.store(false);
            return;
        }
        PaDeviceIndex outDev = Pa_GetDefaultOutputDevice();
        if (outDev == paNoDevice) {
            std::cerr << "[ERROR] <DEBUG> No output device available for debugger" << std::endl;
            running.store(false);
            return;
        }
        const PaDeviceInfo* outInfo = Pa_GetDeviceInfo(outDev);
        if (!outInfo) {
            std::cerr << "[ERROR] <DEBUG> Pa_GetDeviceInfo(outDev) null" << std::endl;
            running.store(false);
            return;
        }

        int inChannels = std::max(1, inInfo->maxInputChannels);
        int outChannels = std::max(1, outInfo->maxOutputChannels);
        unsigned long buf = static_cast<unsigned long>(framesPerBuffer);

        // negotiate input format (try float32, fallback to int16)
        PaStreamParameters inParams;
        std::memset(&inParams, 0, sizeof(inParams));
        inParams.device = inDev;
        inParams.channelCount = inChannels;
        inParams.sampleFormat = paFloat32;
        inParams.suggestedLatency = inInfo->defaultLowInputLatency;
        inParams.hostApiSpecificStreamInfo = nullptr;

        PaSampleFormat activeFormat = paFloat32;
        PaError e = Pa_IsFormatSupported(&inParams, nullptr, sampleRate);
        if (e != paFormatIsSupported) {
            inParams.sampleFormat = paInt16;
            e = Pa_IsFormatSupported(&inParams, nullptr, sampleRate);
            if (e != paFormatIsSupported) {
                // still try int16 open - pulse converts
                inParams.sampleFormat = paInt16;
            } else activeFormat = paInt16;
        } else activeFormat = paFloat32;

        PaStreamParameters outParams;
        std::memset(&outParams, 0, sizeof(outParams));
        outParams.device = outDev;
        outParams.channelCount = outChannels;
        outParams.sampleFormat = paFloat32; // output float
        outParams.suggestedLatency = outInfo->defaultLowOutputLatency;
        outParams.hostApiSpecificStreamInfo = nullptr;

        // open streams
        PaError err = Pa_OpenStream(&inStream, &inParams, nullptr, sampleRate, buf, paNoFlag, nullptr, nullptr);
        if (err != paNoError) {
            std::cerr << "[ERROR] <DEBUG> Pa_OpenStream(in) failed: " << Pa_GetErrorText(err) << std::endl;
            running.store(false);
            return;
        }
        err = Pa_OpenStream(&outStream, nullptr, &outParams, sampleRate, buf, paNoFlag, nullptr, nullptr);
        if (err != paNoError) {
            std::cerr << "[ERROR] <DEBUG> Pa_OpenStream(out) failed: " << Pa_GetErrorText(err) << std::endl;
            Pa_CloseStream(inStream);
            inStream = nullptr;
            running.store(false);
            return;
        }

        err = Pa_StartStream(inStream);
        if (err != paNoError) {
            std::cerr << "[ERROR] <DEBUG> Pa_StartStream(in) failed: " << Pa_GetErrorText(err) << std::endl;
            Pa_CloseStream(inStream);
            Pa_CloseStream(outStream);
            inStream = outStream = nullptr;
            running.store(false);
            return;
        }
        err = Pa_StartStream(outStream);
        if (err != paNoError) {
            std::cerr << "[ERROR] <DEBUG> Pa_StartStream(out) failed: " << Pa_GetErrorText(err) << std::endl;
            Pa_StopStream(inStream); Pa_CloseStream(inStream);
            Pa_CloseStream(outStream);
            inStream = outStream = nullptr;
            running.store(false);
            return;
        }

        // open wav if requested
        bool dump = false;
        if (!dumpPath.empty()) {
            dump = wav.open(dumpPath, static_cast<int>(sampleRate), outChannels);
            if (!dump) {
                std::cerr << "[ERROR] <DEBUG> Failed to open WAV '" << dumpPath << "'! Continuing without dump." << std::endl;
                dump = false;
            }
        }

        // buffers
        std::vector<float> inFloat(buf * inChannels);
        std::vector<int16_t> inS16(buf * inChannels);
        std::vector<float> outFloat(buf * outChannels);

        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        auto lastPrint = start;
        double peak = 0.0, sumSq = 0.0;
        uint64_t sampleCount = 0;

        while (!stopRequested.load()) {
            if (runSeconds > 0) {
                auto now = clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >= runSeconds) break;
            }

            if (activeFormat == paFloat32) {
                PaError r = Pa_ReadStream(inStream, inFloat.data(), buf);
                if (r && r != paInputOverflowed) {
                    std::cerr << "[ERROR] <DEBUG> Pa_ReadStream(float) returned: " << Pa_GetErrorText(r) << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                for (unsigned long f = 0; f < buf; ++f) {
                    float avg = 0.0f;
                    for (int c=0; c < inChannels; ++c) avg += inFloat[f * inChannels + c];
                    avg /= static_cast<float>(inChannels);
                    for (int oc=0; oc < outChannels; ++oc) outFloat[f * outChannels + oc] = avg;
                    float a = std::fabs(avg);
                    if (a > peak) peak = a;
                    sumSq += (avg * avg);
                    ++sampleCount;
                }
                PaError w = Pa_WriteStream(outStream, outFloat.data(), buf);
                if (w && w != paOutputUnderflowed) {
                    std::cerr << "[ERROR] <DEBUG> Pa_WriteStream returned: " << Pa_GetErrorText(w) << std::endl;
                }
                if (dump) {
                    std::vector<int16_t> tmp(buf * outChannels);
                    for (unsigned long i=0;i<buf * outChannels;++i) tmp[i] = float_to_s16(outFloat[i]);
                    wav.write_s16(tmp.data(), tmp.size());
                }
            } else { // int16 input
                PaError r = Pa_ReadStream(inStream, inS16.data(), buf);
                if (r && r != paInputOverflowed) {
                    std::cerr << "[ERROR] <DEBUG> Pa_ReadStream(int16) returned: " << Pa_GetErrorText(r) << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                for (unsigned long f = 0; f < buf; ++f) {
                    double sum = 0.0;
                    for (int c=0; c < inChannels; ++c) sum += inS16[f * inChannels + c];
                    double avg = sum / inChannels;
                    float nf = static_cast<float>(avg / 32768.0);
                    for (int oc=0; oc < outChannels; ++oc) outFloat[f * outChannels + oc] = nf;
                    float a = std::fabs(nf);
                    if (a > peak) peak = a;
                    sumSq += (nf * nf);
                    ++sampleCount;
                }
                PaError w = Pa_WriteStream(outStream, outFloat.data(), buf);
                if (w && w != paOutputUnderflowed) {
                    std::cerr << "[ERROR] <DEBUG> Pa_WriteStream returned: " << Pa_GetErrorText(w) << std::endl;
                }
                if (dump) {
                    std::vector<int16_t> tmp(buf * outChannels);
                    for (unsigned long i=0;i<buf * outChannels;++i) {
                        float v = outFloat[i];
                        if (v > 1.0f) v = 1.0f;
                        if (v < -1.0f) v = -1.0f;
                        tmp[i] = static_cast<int16_t>(v * 32767.0f);
                    }
                    wav.write_s16(tmp.data(), tmp.size());
                }
            }

            auto now = clock::now();
            if (now - lastPrint >= std::chrono::milliseconds(200)) {
                double rms = (sampleCount>0) ? std::sqrt(sumSq / sampleCount) : 0.0;
                double peakDb = (peak>0.0) ? 20.0 * std::log10(peak) : -120.0;
                double rmsDb = (rms>0.0) ? 20.0 * std::log10(rms) : -120.0;
                std::cout << "[INFO] <DEBUG> PEAK=" << peak << " (" << peakDb << " dBFS)"
                          << " RMS=" << rms << " (" << rmsDb << " dBFS)\n";
                peak = 0.0; sumSq = 0.0; sampleCount = 0;
                lastPrint = now;
            }
        }

        // cleanup
        if (dump) { wav.close(); std::cout << "[INFO] wav saved: " << dumpPath << "\n"; }
        if (inStream) { Pa_StopStream(inStream); Pa_CloseStream(inStream); inStream = nullptr; }
        if (outStream) { Pa_StopStream(outStream); Pa_CloseStream(outStream); outStream = nullptr; }

        running.store(false);
        stopRequested.store(false);
    }
};

static PaDeviceIndex SelectInputDeviceWithPrompt(DeviceManager &dm) {
    // enumerate and gather devices
    dm.enumerateDevices();
    if (dm.devices.empty()) {
        std::cerr << "[INFO] No portaudio input devices found!" << std::endl;
        return paNoDevice;
    }

    // build the device list message
    std::ostringstream oss;
    oss << "Available input devices:\n";
    for (auto &d : dm.devices) {
        oss << "[" << d.index << "] " << d.name << " (inputs=" << d.maxInputs
            << ", defaultSr=" << d.defaultSampleRate << ")\n";
    }
    // mark portaudio default
    PaDeviceIndex paDefault = Pa_GetDefaultInputDevice();
    if (paDefault != paNoDevice) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(paDefault);
        if (di) oss << "\nportaudio default: [" << paDefault << "] " << di->name << "\n";
    } else {
        oss << "\nportaudio has no default input device\n";
    }

    // try to get pulse default device
    std::string pulseDefault;
    FILE *fp = popen("pactl info 2>/dev/null", "r");
    if (fp) {
        char buf[512];
        while (fgets(buf, sizeof(buf), fp) != nullptr) {
            std::string line(buf);
            // look for default source
            auto pos = line.find("Default Source:");
            if (pos == std::string::npos) pos = line.find("Default Source");
            if (pos != std::string::npos) {
                // split at ':'
                auto col = line.find(':');
                std::string value = (col != std::string::npos) ? line.substr(col+1) : line;
                // trim whitespace
                auto trim = [](std::string &s){
                    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){ return !std::isspace(ch); }));
                    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end());
                };
                trim(value);
                pulseDefault = value;
                break;
            }
        }
        pclose(fp);
    }

    if (!pulseDefault.empty()) {
        oss << "\npulse default source: " << pulseDefault << "\n";
    } else {
        oss << "\n(pulse default source not found via pactl)\n";
    }

    PaDeviceIndex autoMatch = paNoDevice;
    if (!pulseDefault.empty()) {
        auto lower = [](std::string s){ std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); }); return s; };
        std::string needle = lower(pulseDefault);
        for (auto &d : dm.devices) {
            std::string dn = lower(d.name);
            if (dn.find(needle) != std::string::npos) {
                autoMatch = d.index;
                break;
            }
        }
        if (autoMatch != paNoDevice) oss << "auto-match candidate: [" << autoMatch << "]\n";
    }

    // instructions for the user
    oss << "\nEnter device index to select, 'd' for portaudio default, 'a' for auto-match";
    if (paDefault == paNoDevice) oss << " (no pa default available)";
    oss << ", or leave blank to accept default.\n";

    std::string promptMsg = oss.str();

    // prompt user
    std::future<std::string> fut = ui.prompt_user_async_string(promptMsg);
    std::string ans = fut.get();
    // trim
    auto trim_s = [](std::string &s){ s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){ return !std::isspace(ch); })); s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end()); };
    trim_s(ans);

    if (ans.empty()) {
        // empty -> accept portaudio default if available, else first input device
        if (paDefault != paNoDevice) return paDefault;
        for (auto &d : dm.devices) if (d.maxInputs > 0) return d.index;
        return paNoDevice;
    }

    // lowercase answer for commands
    std::string ans_l = ans;
    std::transform(ans_l.begin(), ans_l.end(), ans_l.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    if (ans_l == "d") {
        return (paDefault != paNoDevice) ? paDefault : paNoDevice;
    }
    if (ans_l == "a") {
        if (autoMatch != paNoDevice) return autoMatch;
        // fallback to default if no auto match
        if (paDefault != paNoDevice) return paDefault;
        for (auto &d : dm.devices) if (d.maxInputs > 0) return d.index;
        return paNoDevice;
    }

    // try to parse as integer index
    try {
        int idx = std::stoi(ans);
        // verify the index exists and has inputs
        const PaDeviceInfo* pd = Pa_GetDeviceInfo(static_cast<PaDeviceIndex>(idx));
        if (pd && pd->maxInputChannels > 0) return static_cast<PaDeviceIndex>(idx);
        // if not valid, fall through to default
        std::cerr << "[ERROR] User selected invalid device index: " << idx << " - falling back to default!" << std::endl;
    } catch (...) {
        std::cerr << "[ERROR] Could not parse user selection: '" << ans << "' - falling back to default!" << std::endl;
    }

    // fallback
    if (paDefault != paNoDevice) return paDefault;
    for (auto &d : dm.devices) if (d.maxInputs > 0) return d.index;
    return paNoDevice;
}

DeviceManager dm;
static InputDebugger i_debugger;

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

    // dm.enumerateDevices();
    // PaDeviceIndex dev = dm.pickDefault();
    dm.enumerateDevices();
    PaDeviceIndex dev = SelectInputDeviceWithPrompt(dm);
    if (dev == paNoDevice) {
        std::cerr << "[ERROR] No input device selected/found!" << std::endl;
        Pa_Terminate();
        return 1;
    }
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
    std::cout << "[INFO] Listening...\n";
    Logger::GetInstance().log("[INFO] Listening...\n");

    if (true) { // toggle debugger
        i_debugger.start(dm.currentDevice, dm.activeSampleRate, static_cast<int>(dm.framesPerBuffer), "/tmp/mic_dump.wav", 10);
    }

    std::vector<int16_t> buffer; // will be filled by dm.readAndProcess
    bool inSpeech = false;
    bool silenceTimerRunning = false;
    auto silenceStart = std::chrono::steady_clock::now();

    size_t frames_with_nonzero = 0;
    size_t frames_read = 0;

    while (true) {
        // read buffer (mono int16)
        bool ok = dm.readAndProcess(buffer, visualizer, recognizer, framesPerBuffer);
        if (!ok) {
            std::cerr << "[ERROR] stream read failed. re-enumerating devices.\n";
            dm.closeStream();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            dm.enumerateDevices();
            PaDeviceIndex fallback = dm.pickDefault();
            if (fallback != paNoDevice) dm.openStream(fallback, framesPerBuffer, sampleRate);
            continue;
        }

        // compute amplitude on the filled buffer
        int maxAmp = 0;
        for (auto s : buffer) {
            int v = std::abs(static_cast<int>(s));
            if (v > maxAmp) maxAmp = v;
        }
        if (maxAmp > 0) frames_with_nonzero++;
        frames_read++;

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

        // health check
        if (frames_read == 200 && frames_with_nonzero == 0) {
            std::cerr << "[ERROR] no non-zero samples captured in first ~" << (200 * framesPerBuffer) << " frames. check mic, routing, mute.\n";
        }
    }

    // cleanup
    dm.closeStream();
    Pa_Terminate();
    vosk_recognizer_free(recognizer);
    vosk_model_free(model);
    return 0;
}

