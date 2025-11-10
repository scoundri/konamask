#include "TextToSpeech.h"
#include "Logger.h"
#include "Settings.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <cstring>
#include <portaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdlib>   // for system() & voice-fail shutdown
#include <string>
#include <imgui.h>
#include <mutex>
#include <thread>
#include <vector>

#include <onnxruntime/onnxruntime_cxx_api.h>
#include <fftw3.h>

static const int APP_SR = 22050;
static const int MODEL_SR = 48000;
static const int CHUNK_SAMPLES_APP = 2048; // chunk size (tune for latency)
static const int MEL_N_FFT = 1024;
static const int MEL_HOP = 256;
static const int MEL_N_MELS = 80;

// onnx runtime
static std::thread onnx_worker_thread;
static std::mutex onnx_mtx;
static std::condition_variable onnx_cv;
static std::deque<std::vector<int16_t>> onnx_queue;
static std::atomic<bool> onnx_worker_running{false};
// objects
static Ort::Env ort_env{ORT_LOGGING_LEVEL_WARNING, "rvc_worker"};
static std::unique_ptr<Ort::Session> onnx_conv_session;
static std::unique_ptr<Ort::Session> onnx_vocoder_session;
static std::unique_ptr<Ort::SessionOptions> onnx_sess_opts;

pa_simple *pa = nullptr;
static constexpr const char* VIRT_MODS_PATH = "/tmp/virt_mods";

static inline void trim_inplace(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){ return !std::isspace(ch); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end());
}

static void resample_linear(const float* in, int in_len, int in_sr, float* out, int out_len) {
    double ratio = double(in_len - 1) / double(out_len - 1);
    for (int i = 0; i < out_len; ++i) {
        double pos = i * ratio;
        int idx = (int)floor(pos);
        double frac = pos - idx;
        float s1 = in[std::min(idx, in_len-1)];
        float s2 = in[std::min(idx+1, in_len-1)];
        out[i] = float((1.0 - frac) * s1 + frac * s2);
    }
}

static bool resample_to_sr(const std::vector<float>& in, int in_sr, int target_sr, std::vector<float>& out) {
    if (in.empty()) { out.clear(); return true; }
#ifdef HAVE_LIBSAMPLERATE
    double src_ratio = double(target_sr) / double(in_sr);
    long out_len = (long)std::ceil(in.size() * src_ratio);
    out.resize(out_len);
    SRC_DATA src_data;
    src_data.data_in = in.data();
    src_data.input_frames = in.size();
    src_data.data_out = out.data();
    src_data.output_frames = out_len;
    src_data.end_of_input = 1;
    src_data.src_ratio = src_ratio;
    int err = src_simple(&src_data, SRC_SINC_MEDIUM_QUALITY, 1);
    if (err != 0) return false;
    if ((long)src_data.output_frames_gen < out_len) out.resize(src_data.output_frames_gen);
    return true;
#else
    // fallback linear
    double ratio = double(target_sr) / double(in_sr);
    size_t out_len = (size_t)std::ceil(in.size() * ratio);
    out.resize(out_len);
    resample_linear(in.data(), in.size(), in_sr, out.data(), out_len);
    return true;
#endif
}

static void waveform_to_mel(const std::vector<float>& waveform, int sr, std::vector<float>& mel_out, int n_mels=MEL_N_MELS, int n_fft=MEL_N_FFT, int hop=MEL_HOP) {
    // stft->mel pipeline using fftw (if heavy replace with a more optimized lib)
    int n_frames = (waveform.size() - n_fft) / hop + 1;
    if (n_frames < 1) { mel_out.clear(); return; }
    // allocate arrays
    std::vector<float> win(n_fft);
    for (int i=0;i<n_fft;++i) win[i] = 0.5f - 0.5f * cosf(2*M_PI*i/(n_fft-1)); // hann

    int fft_size = n_fft;
    fftwf_plan plan;
    std::vector<float> inbuf(fft_size);
    std::vector<fftwf_complex> outbuf(fft_size/2+1);
    plan = fftwf_plan_dft_r2c_1d(fft_size, inbuf.data(), outbuf.data(), FFTW_MEASURE);

    // compute a linear mel filterbank (simple approximate)
    // build mel filterbank matrix (freq bins -> mel bins)
    int n_bins = fft_size/2 + 1;
    // compute mel centre frequencies
    auto hz_to_mel = [](double f){ return 2595.0*log10(1.0 + f/700.0); };
    auto mel_to_hz = [](double m){ return 700.0*(pow(10.0, m/2595.0)-1.0); };
    double mel_min = hz_to_mel(0);
    double mel_max = hz_to_mel(sr/2.0);
    std::vector<double> mel_points(n_mels+2);
    for (int i=0;i<n_mels+2;++i) mel_points[i] = mel_to_hz(mel_min + (mel_max-mel_min)*(double(i)/(n_mels+1)));
    std::vector<int> bin_points(n_mels+2);
    for (int i=0;i<n_mels+2;++i) bin_points[i] = (int)floor((fft_size+1) * mel_points[i] / sr);

    // prepare mel_out (n_mels * n_frames)
    mel_out.assign(n_mels * n_frames, 0.0f);

    for (int frame=0; frame<n_frames; ++frame) {
        int offset = frame*hop;
        for (int i=0;i<n_fft;++i) inbuf[i] = waveform[offset + i] * win[i];
        fftwf_execute(plan);
        // magnitude spectrum
        std::vector<float> mag(n_bins);
        for (int b=0;b<n_bins;++b) {
            float re = outbuf[b][0];
            float im = outbuf[b][1];
            mag[b] = sqrtf(re*re + im*im);
        }
        // apply mel filters
        for (int m=0;m<n_mels;++m) {
            int start = bin_points[m];
            int center = bin_points[m+1];
            int end = bin_points[m+2];
            if (end<=start) continue;
            double sum = 0.0;
            for (int k=start; k<center && k < n_bins; ++k) {
                double w = (double)(k - start) / (center - start);
                sum += mag[k] * w;
            }
            for (int k=center; k<end && k < n_bins; ++k) {
                double w = (double)(end - k) / (end - center);
                sum += mag[k] * w;
            }
            // log magnitude
            mel_out[m + frame * n_mels] = (float)logf(1e-6f + (float)sum);
        }
    }
    fftwf_destroy_plan(plan);
}

static std::vector<float> run_onnx_session(Ort::Session* sess, const std::vector<float>& input, const std::vector<int64_t>& shape, const char* input_name = nullptr) {
    Ort::AllocatorWithDefaultOptions allocator;
    const char* in_name = input_name ? input_name : sess->GetInputNameAllocated(0, allocator).get();
    std::vector<int64_t> in_shape = shape;
    size_t in_size = 1;
    for (auto d: in_shape) in_size *= d;
    // create tensor
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(mem_info, const_cast<float*>(input.data()), in_size, in_shape.data(), in_shape.size());
    // run
    std::vector<const char*> input_names{in_name};
    std::vector<const char*> output_names;
    size_t out_count = sess->GetOutputCount();
    output_names.reserve(out_count);
    for (size_t i=0;i<out_count;++i) output_names.push_back(sess->GetOutputNameAllocated(i, allocator).get());
    auto output_tensors = sess->Run(Ort::RunOptions{nullptr}, input_names.data(), &input_tensor, 1, output_names.data(), output_names.size());
    // read first output (float)
    float* out_data = output_tensors.front().GetTensorMutableData<float>();
    auto out_info = output_tensors.front().GetTensorTypeAndShapeInfo();
    std::vector<int64_t> out_shape = out_info.GetShape();
    size_t out_size = 1;
    for (auto d: out_shape) out_size *= d;
    std::vector<float> out(out_data, out_data + out_size);
    return out;
}

static void start_onnx_worker(const std::string& conv_onnx_path, const std::string& voc_onnx_path) {
    std::lock_guard<std::mutex> lk(onnx_mtx);
    if (onnx_worker_running.load()) return;

    // init session options
    onnx_sess_opts = std::make_unique<Ort::SessionOptions>();
    onnx_sess_opts->SetIntraOpNumThreads(1);
    onnx_sess_opts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    
    // onnx_sess_opts->AppendExecutionProvider_CUDA(0);

    // load sessions
    onnx_conv_session = std::make_unique<Ort::Session>(ort_env, conv_onnx_path.c_str(), *onnx_sess_opts);
    onnx_vocoder_session = std::make_unique<Ort::Session>(ort_env, voc_onnx_path.c_str(), *onnx_sess_opts);

    onnx_worker_running.store(true);
    onnx_worker_thread = std::thread([](){
        std::vector<int16_t> local_chunk;
        while (onnx_worker_running.load()) {
            {
                std::unique_lock<std::mutex> ul(onnx_mtx);
                onnx_cv.wait(ul, [](){ return !onnx_queue.empty() || !onnx_worker_running.load(); });
                if (!onnx_worker_running.load()) break;
                local_chunk = std::move(onnx_queue.front());
                onnx_queue.pop_front();
            }
            // convert int16 chunk -> float [-1,1]
            std::vector<float> in_f(local_chunk.size());
            for (size_t i=0;i<local_chunk.size();++i) in_f[i] = local_chunk[i] / 32768.0f;

            // resample to model sr
            std::vector<float> resampled;
            if (!resample_to_sr(in_f, APP_SR, MODEL_SR, resampled)) {
                // fallback (pass-through wav - resample failed)
                resampled.swap(in_f);
            }

            // compute mel
            std::vector<float> mel;
            waveform_to_mel(resampled, MODEL_SR, mel, MEL_N_MELS, MEL_N_FFT, MEL_HOP);
            if (mel.empty()) {
                // nothing to run, fallback write original chunk
                int pa_err;
                pa_simple_write(pa, local_chunk.data(), local_chunk.size()*sizeof(int16_t), &pa_err);
                continue;
            }

            int time_frames = (resampled.size() - MEL_N_FFT) / MEL_HOP + 1;
            std::vector<int64_t> in_shape = {1, MEL_N_MELS, time_frames};

            // run conversion
            std::vector<float> conv_out = run_onnx_session(onnx_conv_session.get(), mel, in_shape);


            std::vector<int64_t> voc_shape = {1, MEL_N_MELS, (int)(conv_out.size()/MEL_N_MELS)};
            std::vector<float> wav_out = run_onnx_session(onnx_vocoder_session.get(), conv_out, voc_shape);

            // postprocess wav_out -> clamp and convert to int16 at MODEL_SR, resample back to APP_SR
            std::vector<float> out_resampled;
            if (!resample_to_sr(wav_out, MODEL_SR, APP_SR, out_resampled)) {
                out_resampled.swap(wav_out);
            }
            // convert to int16
            std::vector<int16_t> out_pcm(out_resampled.size());
            for (size_t i=0;i<out_resampled.size();++i) {
                float v = out_resampled[i];
                v = std::max(-1.0f, std::min(1.0f, v));
                out_pcm[i] = (int16_t)lrintf(v * 32767.0f);
            }
            // write to pulseaudio - blocks thread
            int pa_err;
            if (pa_simple_write(pa, out_pcm.data(), out_pcm.size()*sizeof(int16_t), &pa_err) < 0) {
                std::cerr << "[ERROR] pa_simple_write failed in onnx worker: " << pa_strerror(pa_err) << std::endl;
            }
        }
    });
}

static void stop_onnx_worker() {
    {
        std::lock_guard<std::mutex> lk(onnx_mtx);
        onnx_worker_running.store(false);
        onnx_cv.notify_all();
    }
    if (onnx_worker_thread.joinable()) onnx_worker_thread.join();
    onnx_conv_session.reset();
    onnx_vocoder_session.reset();
    onnx_sess_opts.reset();
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

    if (cfg.get<bool>("onnx_worker_enabled",false)) {
        start_onnx_worker(cfg.get<std::string>("onnx_conv_model","./conv.onnx"), cfg.get<std::string>("onnx_vocoder_model","./vocoder.onnx"));
    }

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
    Settings &cfg = Settings::GetInstance();
    espeak_Synchronize(); // ensure all speech is played before exiting

    if (cfg.get<bool>("onnx_worker_enabled",false)) 
        stop_onnx_worker();

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
        // push to worker queue instead of direct write
        std::vector<int16_t> chunk(wav, wav + numsamples);
        {
            std::lock_guard<std::mutex> lk(onnx_mtx);
            onnx_queue.push_back(std::move(chunk));
            // limit queue size to prevent memory inceneration
            while (onnx_queue.size() > 32) onnx_queue.pop_front();
        }
        onnx_cv.notify_one();
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
