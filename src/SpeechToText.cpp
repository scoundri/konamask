#include "SpeechToText.h"
#include "Interface.h"
#include "Logger.h"
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cmath>
#include <future>
#include <fstream>
#include <iostream>
#include <cstring>
#include <rtaudio/RtAudio.h>

static InputVisualizer visualizer;
static Interface ui;

static bool ui_prompt(std::string output) {
    Interface ui_;
    auto fut = ui_.prompt_user_async(output);
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

static std::string ui_prompt_str(std::string output) {
    Interface ui_;
    auto fut = ui_.prompt_user_async_string(output);
    return fut.get();
    // std::string value = fut.get();
    // if (value.empty()) {
    //     return NULL;
    // } else {
    //     return value;
    // }
}


struct PaDevInfo {
    int index;
    std::string name;
    int maxInputs;
    double defaultSampleRate;
};

struct DeviceManager {
    // rtaudio instance
    RtAudio dac;

    // device/stream state
    unsigned int currentDevice = (unsigned int)-1; // device id
    RtAudioFormat activeFormat = RTAUDIO_SINT16;   // RTAUDIO_SINT16 || RTAUDIO_FLOAT32
    int activeChannels = 1;
    double activeSampleRate = 48000.0;
    unsigned int framesPerBuffer = 512;

    std::vector<PaDevInfo> devices;

    std::deque<int16_t> fifo;
    std::mutex fifo_mtx;
    std::condition_variable fifo_cv;
    std::atomic<bool> streamOpen{false};

    // monitor thread
    std::thread monitor_thread;
    std::atomic<bool> monitor_running{false};
    std::string monitor_wav_path;
    std::atomic<int> monitor_out_channels{1}; // out callback will use this

    DeviceManager() {}

    ~DeviceManager() {
        stopMonitor();
        closeStream();
        if (dac.isStreamOpen()) {
            try { dac.closeStream(); } catch(...) {}
        }
    }

    // enumerate devices
    void enumerateDevices() {
        devices.clear();
        unsigned int nd = 0;
        try {
            nd = dac.getDeviceCount();
        } catch (const std::exception &e) {
            std::cerr << "[ERROR] RtAudio getDeviceCount failed: " << e.what() << std::endl;
            return;
        }
        for (unsigned int i = 0; i < nd; ++i) {
            try {
                RtAudio::DeviceInfo info = dac.getDeviceInfo(i);
                if (info.inputChannels <= 0) continue;
                PaDevInfo d;
                d.index = static_cast<int>(i);
                d.name = info.name;
                d.maxInputs = static_cast<int>(info.inputChannels);
                if (!info.sampleRates.empty()) d.defaultSampleRate = static_cast<double>(info.sampleRates[0]);
                else d.defaultSampleRate = (info.preferredSampleRate > 0 ? static_cast<double>(info.preferredSampleRate) : 48000.0);
                devices.push_back(d);
            } catch (const std::exception &e) {
                std::cerr << "[ERROR] RtAudio getDeviceInfo(" << i << ") failed: " << e.what() << " - skipping!" << std::endl;
                continue;
            }
        }
    }


    // choose default device
    // fallback - first device with >0 inputChannels
    PaDeviceIndex pickDefault() {
        int def = -1;
        try {
            def = static_cast<int>(dac.getDefaultInputDevice());
        } catch (const std::exception &e) {
            (void)e;
            def = -1;
        }
        if (def >= 0) {
            try {
                RtAudio::DeviceInfo info = dac.getDeviceInfo(static_cast<unsigned int>(def));
                if (info.inputChannels > 0) return def;
            } catch (...) {
                // accept exception
            }
        }
        for (auto &d : devices) if (d.maxInputs > 0) return d.index;
        return paNoDevice;
    }

    // rtaudio callback
    static int rtaudio_callback(void *outputBuffer, void *inputBuffer,
                                unsigned int nBufferFrames,
                                double /*streamTime*/, RtAudioStreamStatus status,
                                void *userData)
    {
        DeviceManager *mgr = reinterpret_cast<DeviceManager*>(userData);
        if (!mgr) return 0;

        if (status != RTAUDIO_NO_ERROR) {
            // continue
        }

        if (!inputBuffer) {
            // no data available
            std::lock_guard<std::mutex> lk(mgr->fifo_mtx);
            for (unsigned int i = 0; i < nBufferFrames; ++i) mgr->fifo.push_back(0);
            mgr->fifo_cv.notify_one();
            return 0;
        }

        if (mgr->activeFormat == RTAUDIO_SINT16) {
            // read int16
            int16_t* in = static_cast<int16_t*>(inputBuffer);
            unsigned int channels = static_cast<unsigned int>(mgr->activeChannels);
            for (unsigned int f = 0; f < nBufferFrames; ++f) {
                int sum = 0;
                for (unsigned int c = 0; c < channels; ++c) sum += static_cast<int>(in[f * channels + c]);
                int avg = sum / static_cast<int>(channels);
                if (avg > 32767) avg = 32767;
                if (avg < -32768) avg = -32768;
                {
                    std::lock_guard<std::mutex> lk(mgr->fifo_mtx);
                    mgr->fifo.push_back(static_cast<int16_t>(avg));
                }
            }
        } else { // float32
            float* in = static_cast<float*>(inputBuffer);
            unsigned int channels = static_cast<unsigned int>(mgr->activeChannels);
            for (unsigned int f = 0; f < nBufferFrames; ++f) {
                double sum = 0.0;
                for (unsigned int c = 0; c < channels; ++c) sum += static_cast<double>(in[f * channels + c]);
                double avg = sum / channels;
                if (avg > 1.0) avg = 1.0;
                if (avg < -1.0) avg = -1.0;
                int s16 = static_cast<int>(avg * 32767.0);
                if (s16 > 32767) s16 = 32767;
                if (s16 < -32768) s16 = -32768;
                {
                    std::lock_guard<std::mutex> lk(mgr->fifo_mtx);
                    mgr->fifo.push_back(static_cast<int16_t>(s16));
                }
            }
        }

        // notify
        mgr->fifo_cv.notify_one();
        return 0;
    }
    static int recordCallback(void *outputBuffer, void *inputBuffer,
                              unsigned int nBufferFrames, double /*streamTime*/,
                              RtAudioStreamStatus status, void *userData)
    {
        DeviceManager *mgr = reinterpret_cast<DeviceManager*>(userData);
        if (!mgr) return 0;

        // status can be RTAUDIO_STREAM_STOPPED / XR
        (void)status;

        if (!inputBuffer) {
            // no data -> push zeros
            std::lock_guard<std::mutex> lk(mgr->fifo_mtx);
            for (unsigned int i = 0; i < nBufferFrames; ++i) mgr->fifo.push_back(0);
            mgr->fifo_cv.notify_one();
            return 0;
        }

        // handle int16
        if (mgr->activeFormat == RTAUDIO_SINT16) {
            int16_t* in = static_cast<int16_t*>(inputBuffer);
            unsigned int channels = static_cast<unsigned int>(mgr->activeChannels);
            std::lock_guard<std::mutex> lk(mgr->fifo_mtx);
            for (unsigned int f = 0; f < nBufferFrames; ++f) {
                int sum = 0;
                for (unsigned int c = 0; c < channels; ++c) sum += static_cast<int>(in[f * channels + c]);
                int avg = sum / static_cast<int>(channels);
                if (avg > 32767) avg = 32767;
                if (avg < -32768) avg = -32768;
                mgr->fifo.push_back(static_cast<int16_t>(avg));
            }
            mgr->fifo_cv.notify_one();
            return 0;
        }

        // handle float32
        float* in = static_cast<float*>(inputBuffer);
        unsigned int channels = static_cast<unsigned int>(mgr->activeChannels);
        {
            std::lock_guard<std::mutex> lk(mgr->fifo_mtx);
            for (unsigned int f = 0; f < nBufferFrames; ++f) {
                double sum = 0.0;
                for (unsigned int c = 0; c < channels; ++c) sum += static_cast<double>(in[f * channels + c]);
                double avg = sum / static_cast<double>(channels);
                if (avg > 1.0) avg = 1.0;
                if (avg < -1.0) avg = -1.0;
                int s16 = static_cast<int>(avg * 32767.0);
                if (s16 > 32767) s16 = 32767;
                if (s16 < -32768) s16 = -32768;
                mgr->fifo.push_back(static_cast<int16_t>(s16));
            }
        }
        mgr->fifo_cv.notify_one();
        return 0;
    }

    // output callback for monitor
    static int outCallback(void *outputBuffer, void * /*inputBuffer*/,
                           unsigned int nBufferFrames, double /*streamTime*/,
                           RtAudioStreamStatus status, void *userData)
    {
        DeviceManager *mgr = reinterpret_cast<DeviceManager*>(userData);
        if (!mgr) return 0;

        (void)status;

        float *out = static_cast<float*>(outputBuffer);
        unsigned int outCh = static_cast<unsigned int>(mgr->monitor_out_channels.load());
        if (outCh == 0) outCh = 1;

        // attempt to pop nBufferFrames samples
        std::unique_lock<std::mutex> lk(mgr->fifo_mtx);
        if (mgr->fifo.size() < nBufferFrames) {
            // partial underflow -> fill zeros
            for (unsigned int f = 0; f < nBufferFrames; ++f) {
                for (unsigned int c = 0; c < outCh; ++c) out[f * outCh + c] = 0.0f;
            }
            return 0;
        }

        // read and output
        for (unsigned int f = 0; f < nBufferFrames; ++f) {
            int16_t s = mgr->fifo.front();
            mgr->fifo.pop_front();
            float sample = static_cast<float>(s) / 32768.0f;
            // duplicate to channels
            out[f * outCh + 0] = sample;
            if (outCh > 1) {
                for (unsigned int c = 1; c < outCh; ++c) out[f * outCh + c] = sample;
            }
        }
        lk.unlock();
        return 0;
    }


    bool openStream(PaDeviceIndex dev, unsigned long requestedFramesPerBuffer, double requestedSampleRate) {
        closeStream();

    if (dev == paNoDevice) return false;
    unsigned int deviceId = static_cast<unsigned int>(dev);

    RtAudio::DeviceInfo info;
    try {
        info = dac.getDeviceInfo(deviceId);
    } catch (const std::exception &e) {
        std::cerr << "[ERROR] RtAudio getDeviceInfo: " << e.what() << std::endl;
        return false;
    }
    if (info.inputChannels <= 0) {
        std::cerr << "[ERROR] Selected device has no input channels!" << std::endl;
        return false;
    }

    // transform to unsigned (avoid std::max errors)
    unsigned int ch = std::max<unsigned int>(1u, static_cast<unsigned int>(info.inputChannels));
    double sr = (requestedSampleRate > 0.0) ? requestedSampleRate :
                (info.preferredSampleRate > 0 ? static_cast<double>(info.preferredSampleRate) : 48000.0);

    framesPerBuffer = requestedFramesPerBuffer;

    // open int16 first fallback to float
    RtAudioFormat fmt = RTAUDIO_SINT16;
    RtAudio::StreamParameters iParams;
    iParams.deviceId = deviceId;
    iParams.nChannels = ch;
    iParams.firstChannel = 0;

    // int16
    try {
        dac.openStream(nullptr, &iParams, fmt, static_cast<unsigned int>(sr), &framesPerBuffer, &recordCallback, this);
        activeFormat = fmt;
    } catch (const std::exception &e1) {
        // float32
        try { if (dac.isStreamOpen()) dac.closeStream(); } catch(...) {}
        fmt = RTAUDIO_FLOAT32;
        try {
            dac.openStream(nullptr, &iParams, fmt, static_cast<unsigned int>(sr), &framesPerBuffer, &recordCallback, this);
            activeFormat = fmt;
        } catch (const std::exception &e2) {
            std::cerr << "[ERROR] RtAudio openStream failed: " << e2.what() << "\n";
            return false;
        }
    }

    activeChannels = static_cast<int>(ch);
    activeSampleRate = sr;
    currentDevice = dev;
    streamOpen.store(true);

    try {
        dac.startStream();
    } catch (const std::exception &e) {
        std::cerr << "[ERROR] RtAudio startStream failed: " << e.what() << "\n";
        try { if (dac.isStreamOpen()) dac.closeStream(); } catch(...) {}
        streamOpen.store(false);
        return false;
    }

    std::cout << "[INFO] Opened rtaudio input device '" << info.name
              << "' sr=" << activeSampleRate
              << " ch=" << activeChannels
              << " fmt=" << ((activeFormat==RTAUDIO_FLOAT32)?"float32":"int16")
              << " frames=" << framesPerBuffer << "\n";

    return true;
}

    void closeStream() {
        // stop stream if open
        if (dac.isStreamOpen()) {
            try {
                dac.stopStream();
            } catch (...) {}
            try {
                dac.closeStream();
            } catch (...) {}
        }
        {
            std::lock_guard<std::mutex> lk(fifo_mtx);
            fifo.clear();
        }
        streamOpen.store(false);
    }

    template<typename VisualizerType>
    bool readAndProcess(std::vector<int16_t> &outMono, VisualizerType &visualizer, VoskRecognizer* vosk_recognizer, size_t framesPerBufferLocal) {
        if (!streamOpen.load()) return false;
        outMono.assign(framesPerBufferLocal, 0);

        // wait for enough samples
        std::unique_lock<std::mutex> lk(fifo_mtx);
        bool ok = fifo_cv.wait_for(lk, std::chrono::milliseconds(500), [&]{ return fifo.size() >= framesPerBufferLocal; });
        if (!ok) {
            // timeout - no data
            return false;
        }
        // pop framesPerBufferLocal samples
        for (size_t i = 0; i < framesPerBufferLocal; ++i) {
            outMono[i] = fifo.front();
            fifo.pop_front();
        }
        lk.unlock();

        // feed vosk
        if (vosk_recognizer) {
            vosk_recognizer_accept_waveform(vosk_recognizer,
                                           reinterpret_cast<const char*>(outMono.data()),
                                           static_cast<int>(outMono.size() * sizeof(int16_t)));
        }
        // push to visualizer
        visualizer.PushSamples(outMono.data(), outMono.size());
        return true;
    }
    void startMonitor(const std::string &dump_wav_path = std::string()) {
        if (monitor_running.load()) return;
        monitor_wav_path = dump_wav_path;
        monitor_running.store(true);

        monitor_thread = std::thread([this]() {
            RtAudio outdac;
            RtAudio::StreamParameters outParams;
            unsigned int outDev = 0;
            try { outDev = outdac.getDefaultOutputDevice(); }
            catch (const std::exception &e) {
                std::cerr << "[ERROR] Monitor: getDefaultOutputDevice failed: " << e.what() << "\n";
                monitor_running.store(false);
                return;
            }
            RtAudio::DeviceInfo dout;
            try { dout = outdac.getDeviceInfo(outDev); } catch (const std::exception &e) {
                std::cerr << "[ERROR] Monitor: getDeviceInfo(outDev) failed: " << e.what() << "\n";
                monitor_running.store(false);
                return;
            }

            outParams.deviceId = outDev;
            unsigned int outCh = std::max<unsigned int>(1u, static_cast<unsigned int>(dout.outputChannels));
            outParams.nChannels = outCh;
            outParams.firstChannel = 0;
            unsigned int outFrames = framesPerBuffer; // use same frames
            RtAudioFormat outFmt = RTAUDIO_FLOAT32;

            // expose out channels to callback
            monitor_out_channels.store(static_cast<int>(outCh));

            try {
                outdac.openStream(&outParams, nullptr, outFmt, static_cast<unsigned int>(activeSampleRate), &outFrames, &DeviceManager::outCallback, this);
                outdac.startStream();
            } catch (const std::exception &e) {
                std::cerr << "[ERROR] monitor output stream failed: " << e.what() << std::endl;
                monitor_running.store(false);
                return;
            }

            // keep alive (the outCallback will pull from fifo)
            while (monitor_running.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

            try { if (outdac.isStreamOpen()) { outdac.stopStream(); outdac.closeStream(); } } catch(...) {}
            monitor_running.store(false);
        });
    }


    void stopMonitor() {
        if (!monitor_running.load()) return;
        monitor_running.store(false);
        if (monitor_thread.joinable()) monitor_thread.join();
    }
};


static unsigned int SelectInputDeviceWithPrompt(DeviceManager &dm) {
    // enumerate devices first
    dm.enumerateDevices();
    if (dm.devices.empty()) {
        std::cerr << "[INFO] No rtaudio input devices found!\n";
        return static_cast<unsigned int>(-1);
    }

    // build the device list message
    std::ostringstream oss;
    oss << "Available input devices:\n\n";
    for (auto &d : dm.devices) {
        oss << "[" << d.index << "] " << d.name
            << " (inputs=" << d.maxInputs
            << ", defaultSr=" << d.defaultSampleRate << ")\n";
    }

    // rt-audio default input device
    unsigned int rtDefault = static_cast<unsigned int>(-1);
    try {
        rtDefault = dm.dac.getDefaultInputDevice();
        if (rtDefault != static_cast<unsigned int>(-1)) {
            RtAudio::DeviceInfo di = dm.dac.getDeviceInfo(rtDefault);
            oss << "\nrtaudio default: [" << rtDefault << "] " << di.name << "\n";
        } else {
            oss << "\nrtaudio has no default input device\n";
        }
    } catch (...) {
        oss << "\n(rtaudio default device not available)\n";
    }

    // try to query pulse default source (pactl), non-fatal
    std::string pulseDefault;
    FILE *fp = popen("pactl info 2>/dev/null", "r");
    if (fp) {
        char buf[512];
        while (fgets(buf, sizeof(buf), fp) != nullptr) {
            std::string line(buf);
            auto pos = line.find("Default Source:");
            if (pos == std::string::npos) pos = line.find("Default Source");
            if (pos != std::string::npos) {
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

    // try to find an auto-match candidate (case-insensitive substring match)
    unsigned int autoMatch = static_cast<unsigned int>(-1);
    if (!pulseDefault.empty()) {
        auto lower = [](std::string s){ std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); }); return s; };
        std::string needle = lower(pulseDefault);
        for (auto &d : dm.devices) {
            std::string dn = lower(d.name);
            if (dn.find(needle) != std::string::npos) { autoMatch = static_cast<unsigned int>(d.index); break; }
        }
        if (autoMatch != static_cast<unsigned int>(-1)) oss << "\nauto-match candidate: [" << autoMatch << "]\n";
    }

    oss << "\nEnter device index to select, 'r' for rtaudio default, 'a' for auto-match (if available),\n"
           "or leave blank to accept default / first device.\n";

    // prompt user (blocking - make sure this isn't called on the UI thread)
    std::string ans = ui_prompt_str(oss.str());

    // helper trim
    auto trim_s = [](std::string &s){
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){ return !std::isspace(ch); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end());
    };
    trim_s(ans);

    if (ans.empty()) {
        // empty -> prefer rtaudio default, else first input device
        if (rtDefault != static_cast<unsigned int>(-1)) return rtDefault;
        for (auto &d : dm.devices) if (d.maxInputs > 0) return static_cast<unsigned int>(d.index);
        return static_cast<unsigned int>(-1);
    }

    // lowercase answer for commands
    std::string ans_l = ans;
    std::transform(ans_l.begin(), ans_l.end(), ans_l.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    if (ans_l == "r") {
        return (rtDefault != static_cast<unsigned int>(-1)) ? rtDefault : static_cast<unsigned int>(-1);
    }
    if (ans_l == "a") {
        if (autoMatch != static_cast<unsigned int>(-1)) return autoMatch;
        // fallback to default
        if (rtDefault != static_cast<unsigned int>(-1)) return rtDefault;
        for (auto &d : dm.devices) if (d.maxInputs > 0) return static_cast<unsigned int>(d.index);
        return static_cast<unsigned int>(-1);
    }

    // try parsing integer index
    try {
        int idx = std::stoi(ans);
        // validate via RtAudio
        try {
            RtAudio::DeviceInfo pd = dm.dac.getDeviceInfo(static_cast<unsigned int>(idx));
            if (pd.inputChannels > 0) return static_cast<unsigned int>(idx);
        } catch (...) {
            // fallback to dm.devices search
            for (auto &d : dm.devices) if (d.index == idx && d.maxInputs > 0) return static_cast<unsigned int>(d.index);
        }
        std::cerr << "[ERROR] User selected invalid device index: " << idx << " - falling back to default!\n";
    } catch (...) {
        std::cerr << "[ERROR] Could not parse user selection: '" << ans << "' - falling back to default!\n";
    }

    // final fallback
    if (rtDefault != static_cast<unsigned int>(-1)) return rtDefault;
    for (auto &d : dm.devices) if (d.maxInputs > 0) return static_cast<unsigned int>(d.index);
    return static_cast<unsigned int>(-1);
}



DeviceManager dm;
// static InputDebugger i_debugger;

int SpeechToText::Initialize() {
    std::cout << "\n>─────────────────────[INITIALIZING SPEECH-TO-TEXT]─────────────────────<\n" << std::endl;
    Logger::GetInstance().log("\n>─────────────────────[INITIALIZING SPEECH-TO-TEXT]─────────────────────<\n\n");

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

    // enumerate and let user pick via RtAudio-based DeviceManager
    dm.enumerateDevices();
    unsigned int dev = dm.pickDefault();
    // unsigned int dev = SelectInputDeviceWithPrompt(dm);
    if (dev == static_cast<unsigned int>(-1)) {
        std::cerr << "[ERROR] No input device selected!" << std::endl;
        Logger::GetInstance().log("[ERROR] No input device selected!\n");
        return 1;
    }

    // query device info from RtAudio
    RtAudio::DeviceInfo devInfo;
    try {
        devInfo = dm.dac.getDeviceInfo(dev);
    } catch (const std::exception &e) {
        std::cerr << "[ERROR] getDeviceInfo failed for selected device: " << e.what() << std::endl;
        return 1;
    }

    sampleRate = (devInfo.preferredSampleRate > 0) ? static_cast<double>(devInfo.preferredSampleRate) : 48000.0;

    // small buffer -> low latency
    framesPerBuffer = static_cast<unsigned int>( sampleRate * cfg.get<double>("buffer_factor", 0.05) );
    if (framesPerBuffer == 0) framesPerBuffer = 512;
    std::cout << "[INFO] Buffer factor has been set to  \"" << cfg.get<double>("buffer_factor", 0.05) << "\"." << std::endl;
    Logger::GetInstance().log("[INFO] Buffer factor has been set to  \"");
    Logger::GetInstance().log(std::to_string(cfg.get<double>("buffer_factor", 0.05)));
    Logger::GetInstance().log("\".\n");

    std::cout << "[INFO] Using input device: " << devInfo.name << " @" << sampleRate << "Hz" << std::endl;
    Logger::GetInstance().log("[INFO] Using input device: ");
    Logger::GetInstance().log(devInfo.name);
    Logger::GetInstance().log(" @ ");
    Logger::GetInstance().log(std::to_string(sampleRate));
    Logger::GetInstance().log(" Hz\n");

    // create recognizer
    recognizer = vosk_recognizer_new(model, sampleRate);
    vosk_recognizer_set_max_alternatives(recognizer, 0);
    vosk_recognizer_set_words(recognizer, true);

    // open stream via DeviceManager (RtAudio)
    if (!dm.openStream(dev, framesPerBuffer, sampleRate)) {
        std::cerr << "[ERROR] Failed to open stream on selected device\n";
        return 1;
    }

    std::cout << "[INFO] Successfully opened the input stream!\n";
    Logger::GetInstance().log("[INFO] Successfully opened the input stream!\n");

    std::cout << "\n>────────────────[INITIALIZED SPEECH-TO-TEXT SUCCESSFULLY]───────────────<\n" << std::endl;
    Logger::GetInstance().log("\n>────────────────[INITIALIZED SPEECH-TO-TEXT SUCCESSFULLY]───────────────<\n\n");

    // start main loop
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

    // if (true) { // toggle debugger
    //     i_debugger.start(dm.currentDevice, dm.activeSampleRate, static_cast<int>(dm.framesPerBuffer), "/tmp/mic_dump.wav", 10);
    // }
    const unsigned int NO_DEVICE = static_cast<unsigned int>(-1);
    std::vector<int16_t> buffer; // will be filled by dm.readAndProcess
    bool inSpeech = false;
    bool silenceTimerRunning = false;
    auto silenceStart = std::chrono::steady_clock::now();


    size_t frames_with_nonzero = 0;
    size_t frames_read = 0;

    if (!recognizer) {
        std::cerr << "[ERROR] recognizer is null - aborting!" << std::endl;
        return 1;
    }

    while (true) {
        // read buffer (mono int16)
        bool ok = dm.readAndProcess(buffer, visualizer, recognizer, static_cast<size_t>(framesPerBuffer));
        if (!ok) {
            std::cerr << "[ERROR] Stream read failed! re-enumerating devices..." << std::endl;
            dm.closeStream();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            dm.enumerateDevices();

            // pick a fallback device using the device-manager API
            unsigned int fallback = dm.pickDefault();
            if (fallback != NO_DEVICE) {
                if (!dm.openStream(fallback, framesPerBuffer, sampleRate)) {
                    std::cerr << "[ERROR] Fallback openStream failed for device " << fallback << std::endl;
                } else {
                    std::cout << "[INFO] Reopened fallback device " << fallback << std::endl;
                }
            } else {
                std::cerr << "[ERROR] No fallback device available!" << std::endl;
            }
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
                // reset recognizer for a new utterance 
                // TODO: check pointers
                if (recognizer) vosk_recognizer_reset(recognizer);
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
                if (recognizer) {
                    const char* final_json = vosk_recognizer_final_result(recognizer);
                    if (final_json) {
                        try {
                            auto j = nlohmann::json::parse(final_json);
                            std::string txt;
                            if (j.contains("text")) txt = j["text"].get<std::string>();
                            if (!txt.empty()) {
                                std::cout << "[INFO] Dispatching \"" << txt << "\"..." << std::endl;
                                TextToSpeech::Verbalize(txt.c_str());
                            }
                        } catch (const std::exception &e) {
                            std::cerr << "[ERROR] Failed to parse recognizer final result: " << e.what() << std::endl;
                        }
                    } else {
                        std::cerr << "[ERROR] vosk_recognizer_final_result returned null" << std::endl;
                    }
                } else {
                    std::cerr << "[ERROR] Recognizer was null at final_result stage" << std::endl;
                }

                inSpeech = false;
                silenceTimerRunning = false;
            }
        }

        // health check
        if (frames_read == 200 && frames_with_nonzero == 0) {
            std::cerr << "[ERROR] no non-zero samples captured in first ~" << (200u * static_cast<unsigned int>(framesPerBuffer))
                      << " samples. check mic, routing, mute." << std::endl;
        }
    }

    // cleanup
    dm.closeStream();
    Pa_Terminate();
    vosk_recognizer_free(recognizer);
    vosk_model_free(model);
    return 0;
}

