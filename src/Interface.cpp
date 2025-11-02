// most initialization code was taken from: https://github.com/ocornut/imgui/

#include "Interface.h"
#include "TextToSpeech.h" // for manual voice output
#include "SpeechToText.h" // for capturing voice input
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <imgui.h>
#include <imgui_internal.h>
#include "Settings.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl2.h"
#include <linux/limits.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <thread>
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>
#include <array> // for ImVec4 conversion
#include <limits.h> // for config.ini path
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h> // for texture (image) loading
#include <fstream> // for CheckFile and CopyFile
#include <sys/stat.h> // for CheckFile and CopyFile
#include <filesystem>
#include <vector>
#include <chrono>
#include <algorithm>
#include <string>
#include "Logger.h"
#include "font.c"
#include "icons.c"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#include <volk.h>
#endif

//#define _DEBUG
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
#endif


static VkAllocationCallbacks*   g_Allocator = nullptr;
static VkInstance               g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily = (uint32_t)-1;
static VkQueue                  g_Queue = VK_NULL_HANDLE;       
static VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;
static VkDescriptorSetLayout    g_DescriptorSetLayout = VK_NULL_HANDLE; // for manual font upload
static VkCommandPool            g_CommandPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
SDL_Window*                     window;
std::atomic<bool>               g_RenderPaused{false};
static uint32_t                 g_MinImageCount = 2;
static VkImageUsageFlags        g_SwapChainImageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // new api
static bool                     g_SwapChainRebuild = false;

VkFormat swapchain_image_format;

struct SurfaceFormat {
    VkFormat format;
    VkColorSpaceKHR colorSpace;
};

ImFont* f_iconData;     // holds icon font data 

// TextureData struct code from https://github.com/ocornut/imgui/wiki/Image-Loading-and-Displaying-Examples
struct TextureData {
    VkDescriptorSet DS;         // descriptor set
    int             width;
    int             height;
    int             Channels;

    // for proper cleanup
    VkImageView     ImageView;
    VkImage         Image;
    VkDeviceMemory  ImageMemory;
    VkSampler       Sampler;
    VkBuffer        UploadBuffer;
    VkDeviceMemory  UploadBufferMemory;

    TextureData() { memset(this, 0, sizeof(*this)); }
};

struct ImGuiFilePicker {
    
    std::filesystem::path current_path = std::filesystem::current_path();
    std::vector<std::string> image_exts = {".png", ".jpg", ".jpeg", ".bmp", ".gif"}; // .webp usually cause an exception
    std::string selected_path;
    std::string filter; // filter string (e.g. "*.png;*.jpg") - not implemented, reserved
    std::chrono::steady_clock::time_point last_click_time = std::chrono::steady_clock::now(); // last click time to detect double-click
    std::string last_clicked_item;

    // draw the widget > returns true when the user double-clicks a file (selection made)
    bool Draw(const char* title, bool* p_open, std::string* out_selected_path = nullptr) {
        if (p_open && !*p_open) return false;

        bool selection_made = false;

        ImGui::SetNextWindowSize(ImVec2(640, 360), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, p_open)) {
            ImGui::End();
            return false;
        }

        // path breadcrumb
        DrawBreadcrumb();

        ImGui::Separator();

        // two columns > directories on left, files on right
        ImGui::Columns(2, "filepicker_cols", true);
        DrawDirectoriesColumn();
        ImGui::NextColumn();
        DrawFilesColumn(&selection_made);
        ImGui::Columns(1);

        ImGui::End();

        if (selection_made && out_selected_path) {
            *out_selected_path = selected_path;
        }
        return selection_made;
    }
private:
    void DrawBreadcrumb() {
        std::string accum;
        ImGui::TextDisabled("current:"); ImGui::SameLine();
        // build clickable path pieces
        for (auto it = current_path.begin(); it != current_path.end(); ++it) {
            if (it != current_path.begin()) ImGui::SameLine();
            std::string part = it->string();
            if (ImGui::Button(part.c_str())) {
                // assemble path up to this element
                std::filesystem::path p;
                for (auto jt = current_path.begin(); jt != it; ++jt) p /= *jt;
                p /= *it;
                if (std::filesystem::exists(p) && std::filesystem::is_directory(p)) current_path = p;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("..")) {
            if (current_path.has_parent_path()) current_path = current_path.parent_path();
        }
    }

    void DrawDirectoriesColumn() {
        ImGui::Text("folders");

        // enumerate directories
        std::error_code ec;
        std::vector<std::filesystem::directory_entry> dirs;
        for (auto &de : std::filesystem::directory_iterator(current_path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (de.is_directory(ec)) dirs.push_back(de);
        }
        // sort by name
        std::sort(dirs.begin(), dirs.end(), [](auto &a, auto &b){ return a.path().filename().string() < b.path().filename().string(); });

        // display
        ImGui::BeginChild("##dirs", ImVec2(0,0), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (auto &d : dirs) {
            std::string name = d.path().filename().string();
            if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    // enter directory on double click
                    current_path /= name;
                }
            }
        }
        ImGui::EndChild();
    }

    void DrawFilesColumn(bool* selection_made) {
        ImGui::Text("files");
        ImGui::Separator();
        std::error_code ec;
        std::vector<std::filesystem::directory_entry> files;
        for (auto &de : std::filesystem::directory_iterator(current_path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (de.is_regular_file(ec)) {
                auto ext = de.path().extension().string();
                // lowercase ext
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
                if (std::find(image_exts.begin(), image_exts.end(), ext) != image_exts.end()) files.push_back(de);
            }
        }

        std::sort(files.begin(), files.end(), [](auto &a, auto &b){ return a.path().filename().string() < b.path().filename().string(); });

        ImGui::BeginChild("##files", ImVec2(0,0), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (auto &f : files) {
            std::string name = f.path().filename().string();
            bool selected = (selected_path == f.path().string());
            if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                // single click selects
                selected_path = f.path().string();

                // detect double click (using ImGui builtin)
                if (ImGui::IsMouseDoubleClicked(0)) {
                    // choose file
                    if (std::find(image_exts.begin(), image_exts.end(), f.path().extension().string()) != image_exts.end()) {
                        selected_path = f.path().string();
                        if (selection_made) *selection_made = true;
                    }
                }
            }
        }
        ImGui::EndChild();
    }
};

// audio input stream
InputVisualizer::InputVisualizer()
    : ring(), ringMask(0), writeIndex(0), readIndex(0),
      sr(0), N(0), halfN(0),
      tw_re(), tw_im(), win(), fft_re(), fft_im(),
      spectrum(), waveform(), spectrumSmoothed(), peakHold(),
      lastPeakDecay(std::chrono::steady_clock::now()),
      gain(1.0f), smoothingAlpha(0.6f), peakDecayPerSec(12.0f),
      minDb(-90.0f), maxDb(0.0f),
      tmp_re(), tmp_im()
{}

InputVisualizer::~InputVisualizer() {}

bool InputVisualizer::Initialize(int sampleRate, int fftSize, int ringBufferSeconds) {
    if (fftSize <= 0) return false;
    // power of two
    int p2 = 1;
    while (p2 < fftSize) p2 <<= 1;
    N = p2;
    sr = sampleRate;
    halfN = N / 2;

    // prepare buffers
    fft_re.assign(N, 0.0);
    fft_im.assign(N, 0.0);
    tmp_re.assign(N, 0.0);
    tmp_im.assign(N, 0.0);

    spectrum.assign(halfN, 0.0f);
    spectrumSmoothed.assign(halfN, 0.0f);
    peakHold.assign(halfN, minDb);
    waveform.assign(N, 0.0f);
    win.assign(N, 1.0);
    for (int i = 0; i < N; ++i) win[i] = hannWindow(i, N);

    prepareTwiddles();

    // ring buffer size = next power-of-two of (sr * seconds)
    size_t ringSamples = 1;
    size_t desired = static_cast<size_t>(sr) * std::max(1, ringBufferSeconds);
    while (ringSamples < desired) ringSamples <<= 1;
    ring.resize(ringSamples);
    ringMask = ringSamples - 1;
    writeIndex.store(0);
    readIndex.store(0);

    lastPeakDecay = std::chrono::steady_clock::now();

    return true;
}

void InputVisualizer::prepareTwiddles() {
    // precompute
    tw_re.resize(N/2);
    tw_im.resize(N/2);
    for (int k = 0; k < N/2; ++k) {
        double angle = -2.0 * M_PI * k / N;
        tw_re[k] = std::cos(angle);
        tw_im[k] = std::sin(angle);
    }
}

void InputVisualizer::inplaceFFT(std::vector<double>& re, std::vector<double>& im) {
    int n = N;
    // bit-reversal permutation
    int j = 0;
    for (int i = 1; i < n - 1; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    // fft
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                int twiddleIndex = k * step;
                double tre = tw_re[twiddleIndex] * re[i + k + half] - tw_im[twiddleIndex] * im[i + k + half];
                double tim = tw_re[twiddleIndex] * im[i + k + half] + tw_im[twiddleIndex] * re[i + k + half];
                double ur = re[i + k];
                double ui = im[i + k];
                re[i + k] = ur + tre;
                im[i + k] = ui + tim;
                re[i + k + half] = ur - tre;
                im[i + k + half] = ui - tim;
            }
        }
    }
}

inline void InputVisualizer::writeRing(const int16_t* src, size_t n) {
    // write # samples to ring buffer - writer is single-threaded (audio thread)
    uint64_t w = writeIndex.load(std::memory_order_relaxed);
    size_t pos = static_cast<size_t>(w & ringMask);
    size_t remain = n;
    while (remain > 0) {
        size_t chunk = std::min(remain, ring.size() - pos);
        std::memcpy(&ring[pos], src + (n - remain), chunk * sizeof(int16_t));
        pos = (pos + chunk) & ringMask;
        remain -= chunk;
    }
    // update write index once at end
    writeIndex.store(w + n, std::memory_order_release);
    // if writer outruns reader, advance reader (drop oldest)
    uint64_t r = readIndex.load(std::memory_order_relaxed);
    if ((w + n) - r > ring.size()) {
        // drop oldest frames
        readIndex.store((w + n) - ring.size(), std::memory_order_relaxed);
    }
}

inline size_t InputVisualizer::availableSamples() const {
    uint64_t w = writeIndex.load(std::memory_order_acquire);
    uint64_t r = readIndex.load(std::memory_order_acquire);
    if (w <= r) return 0;
    uint64_t avail = w - r;
    if (avail > ring.size()) avail = ring.size();
    return static_cast<size_t>(avail);
}

inline void InputVisualizer::readFromRing(int16_t* dst, size_t n) {
    // read # samples from ring buffer
    uint64_t r = readIndex.load(std::memory_order_relaxed);
    size_t pos = static_cast<size_t>(r & ringMask);
    size_t remain = n;
    while (remain > 0) {
        size_t chunk = std::min(remain, ring.size() - pos);
        std::memcpy(dst + (n - remain), &ring[pos], chunk * sizeof(int16_t));
        pos = (pos + chunk) & ringMask;
        remain -= chunk;
    }
    readIndex.store(r + n, std::memory_order_release);
}

void InputVisualizer::PushSamples(const int16_t* samples, size_t count) {
    if (!ring.empty() && count > 0) writeRing(samples, count);
}

void InputVisualizer::Process() {
    // if not enough samples for an fft, try to read # samples
    size_t avail = availableSamples();
    if (avail < static_cast<size_t>(N)) {
        // not enough, but still read partial wave for waveform rendering
        size_t toRead = std::min(avail, static_cast<size_t>(N));
        if (toRead == 0) return;
        std::vector<int16_t> scratch(toRead);
        readFromRing(scratch.data(), toRead);
        // fill waveform with newest samples aligned to end
        int pad = N - static_cast<int>(toRead);
        for (int i = 0; i < pad; ++i) waveform[i] = 0.0f;
        for (size_t i = 0; i < toRead; ++i) waveform[pad + i] = static_cast<float>(scratch[i]) / 32768.0f * gain;
        // not enough for fft, keep previous spectrum with slight decay
        float decayFactor = 1.0f - (1.0f - smoothingAlpha) * 0.5f;
        for (int i = 0; i < halfN; ++i) spectrumSmoothed[i] *= decayFactor;
        return;
    }

    // read exactly # samples for fft
    std::vector<int16_t> samples(N);
    readFromRing(samples.data(), N);

    // convert to double, apply window, and copy to fft arrays
    for (int i = 0; i < N; ++i) {
        double v = static_cast<double>(samples[i]) / 32768.0;
        v *= static_cast<double>(gain);
        tmp_re[i] = v * win[i];
        tmp_im[i] = 0.0;
        waveform[i] = static_cast<float>(v); // store raw waveform (no window) for display
    }

    // compute FFT in-place on tmp arrays
    std::copy(tmp_re.begin(), tmp_re.end(), fft_re.begin());
    std::fill(fft_im.begin(), fft_im.end(), 0.0);
    inplaceFFT(fft_re, fft_im);

    // magnitude for first half
    // convert to dB and normalize (double) for display
    const double eps = 1e-12;
    for (int k = 0; k < halfN; ++k) {
        double re = fft_re[k];
        double im = fft_im[k];
        double mag = std::sqrt(re*re + im*im) / (double)N; // scale by #
        double db = 20.0 * std::log10(mag + eps); // dBFS
        // clamp
        if (db < minDb) db = minDb;
        if (db > maxDb) db = maxDb;
        // normalize
        float norm = static_cast<float>((db - minDb) / (maxDb - minDb));
        // smoothing
        spectrumSmoothed[k] = smoothingAlpha * spectrumSmoothed[k] + (1.0f - smoothingAlpha) * norm;
        // peak hold update (in db space)
        float peak = peakHold[k];
        float dbf = static_cast<float>(db);
        if (dbf > peak) peakHold[k] = dbf;
        spectrum[k] = norm;
    }

    // peak decay based on elapsed time
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - lastPeakDecay).count();
    lastPeakDecay = now;
    float decayAmount = static_cast<float>(peakDecayPerSec * elapsed);
    for (int k = 0; k < halfN; ++k) {
        peakHold[k] -= decayAmount;
        if (peakHold[k] < minDb) peakHold[k] = minDb;
    }
}

void InputVisualizer::render(Settings* cfg) {
    // frequency spectrum
    static float tcr;
    static float tcg;
    static float tcb;
    // validate range
    auto in_range = [](int v){ return (v >= 0 && v <= 255); };
    if (!in_range(cfg->get<int>("ui_theme_red",   50)) || !in_range(cfg->get<int>("ui_theme_green", 20)) || !in_range(cfg->get<int>("ui_theme_blue",  60))) {
        std::cerr << "[ERROR] Color parse/validation failed, reverting to defaults." << std::endl;
        Logger::GetInstance().log("[ERROR] Color parse/validation failed, reverting to defaults.\n");
        tcr = static_cast<float>(50); tcg = static_cast<float>(20); tcb = static_cast<float>(60);
    } 
    else {
        tcr = static_cast<float>(cfg->get<int>("ui_theme_red",   167));
        tcg = static_cast<float>(cfg->get<int>("ui_theme_green", 42));
        tcb = static_cast<float>(cfg->get<int>("ui_theme_blue",  92));
    }
    ImGui::Text("spectrum");
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 sz = ImVec2(ImGui::GetColumnWidth(), 160.0f);
        ImGui::InvisibleButton("spec_canvas", sz);
        dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(100,100,100,255));

        int bins = static_cast<int>(spectrumSmoothed.size());
        float barW = sz.x / (float)bins;
        for (int i = 0; i < bins; ++i) {
            float x0 = p.x + i * barW;
            float x1 = x0 + barW * 0.9f;
            float v = spectrumSmoothed[i];
            float y = p.y + sz.y * (1.0f - v);
            // draw bar
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, p.y + sz.y), IM_COL32(tcr,tcg,tcb,200));
            // draw peak line in different color
            float peakNorm = (peakHold[i] - minDb) / (maxDb - minDb);
            if (peakNorm < 0.0f) peakNorm = 0.0f;
            if (peakNorm > 1.0f) peakNorm = 1.0f;
            float py = p.y + sz.y * (1.0f - peakNorm);
            dl->AddLine(ImVec2(x0, py), ImVec2(x1, py), IM_COL32(255,140,80,220), 1.0f);
        }
        ImGui::Dummy(ImVec2(ImGui::GetColumnWidth(), 12.0f));
    }
    // controls
    ImGui::PushItemWidth(-1);
    ImGui::SliderFloat("##gain", &gain, 0.0f, 10.0f, "Gain = %.2f");
    ImGui::SliderFloat("##smoothing", &smoothingAlpha, 0.0f, 0.999f, "Smoothing = %.3f");
    ImGui::SliderFloat("##peak_decay", &peakDecayPerSec, 0.0f, 60.0f, "Decay = %.1f dB/s");
    ImGui::PopItemWidth();

    // waveform
    ImGui::Text("waveform");
    {
        ImGui::PlotLines("##plot_lines", waveform.data(), waveform.size(), 0, nullptr, -1.0f, 1.0f, ImVec2(ImGui::GetColumnWidth(), 80));
    }

    // simple frequency readout under cursor
    ImGui::Text("fft size %d, sr %d, bin width %.2f Hz", N, sr, sr / (float)N);
}

static int selected_pa_device = -1;
static bool pa_devices_populated = false;
static std::vector<std::string> pa_device_labels;
static std::vector<int> pa_device_indices;

static void populate_pa_device_list() {
    pa_device_labels.clear();
    pa_device_indices.clear();
    int n = Pa_GetDeviceCount();
    if (n < 0) {
        pa_devices_populated = false;
        return;
    }
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo *di = Pa_GetDeviceInfo(i);
        if (!di) continue;
        if (di->maxInputChannels <= 0) continue;
        pa_device_labels.push_back(std::to_string(i) + ": " + std::string(di->name));
        pa_device_indices.push_back(i);
    }
    pa_devices_populated = true;
}

void SpeechToText::render() {

    ImGui::BeginChild("##title_header_stt", ImVec2(0, 54), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosX(16);
    ImGui::SetCursorPosY(20.8f);
    ImGui::TextColored(ImVec4(0.86f,0.88f,0.92f,1.0f), "%s", "INPUT MANAGEMENT");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 230);
    ImGui::SetCursorPosY(13.5f);
    if (stt.workerRunning.load()) {
        if (ImGui::Button("Restart backend", ImVec2(108,28))) {
            stt.PauseListening();
            stt.ResumeListening();
        }
    } else { ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true); ImGui::Button("Restart backend", ImVec2(108,28)); ImGui::PopItemFlag(); }
    ImGui::SameLine();
    ImGui::SetCursorPosY(13.5f);
    if (paused.load()) {
        if (ImGui::Button("Start backend", ImVec2(100,28))) {
            stt.ResumeListening();
        }
    } else {
        if (ImGui::Button("Stop backend", ImVec2(100,28))) {
            stt.PauseListening();
        }
    }
    ImGui::EndChild();

    ImGui::BeginChild("##STT_CONF", ImVec2(0, ImGui::GetWindowHeight()-62), true);

    ImGui::SeparatorText("Mode:");
    if (ImGui::RadioButton("Submit-on-sentence", (int*)&stt.currentMode, (int)ProcessingMode::FinalOnSilence)) {
        stt.SwitchMode(ProcessingMode::FinalOnSilence);
    }
    if (ImGui::RadioButton("Word-for-word", (int*)&stt.currentMode, (int)ProcessingMode::IncrementalPartial)) {
        stt.SwitchMode(ProcessingMode::IncrementalPartial);
    }
    ImGui::TextDisabled("Submit-on-sentence: Submits content on the end of the sentence.\nWord-for-word: Submits each word seperately (for real-time speech) - experimental.");
    
    ImGui::Dummy(ImVec2(0,20));
    // input device chooser
    ImGui::SeparatorText("Input Devices");
    if (ImGui::Button("Refresh list")) {
        populate_pa_device_list();
    }
    ImGui::SameLine();
    if (ImGui::Button("Select default")) {
        PaDeviceIndex def = Pa_GetDefaultInputDevice();
        selected_pa_device = (def == paNoDevice) ? -1 : static_cast<int>(def);
    }
    if (!pa_devices_populated) populate_pa_device_list();
    if (pa_device_labels.empty()) {
        ImGui::SetCursorPosX(20);
        ImGui::TextDisabled("No input devices were detected!\nPA not initialized?");
    } else {
        // build items for combo
        static int combo_idx = 0;
        std::vector<const char*> items;
        items.reserve(pa_device_labels.size());
        for (auto &s : pa_device_labels) items.push_back(s.c_str());
        ImGui::Combo("input device", &combo_idx, items.data(), static_cast<int>(items.size()));
        selected_pa_device = pa_device_indices[combo_idx];
        ImGui::Text("Selected device id: %d", selected_pa_device);
    }
    

    ImGui::Dummy(ImVec2(0,20));

    // voice activity detection parameters
    ImGui::SeparatorText("VAD / STT params");
    int conf_threshold = cfg.get<int>("silence_threshold", 200);
    int conf_timeout = cfg.get<int>("silence_timeout", 1000);
    double buffactor = cfg.get<double>("buffer_factor", 0.05);
    if (ImGui::SliderInt("silence threshold", &conf_threshold, 1, 30000)) {
        cfg.set<int>("silence_threshold", conf_threshold);
    }
    if (ImGui::SliderInt("silence timeout (ms)", &conf_timeout, 100, 5000)) {
        cfg.set<int>("silence_timeout", conf_timeout);
    }
    if (ImGui::InputDouble("buffer factor", &buffactor)) {
        if (buffactor <= 0.0) buffactor = 0.01;
        cfg.set<double>("buffer_factor", buffactor);
    }
    ImGui::Text("Current framesPerBuffer: %d", framesPerBuffer);

    if (ImGui::Button("Apply new input buffer")) {
        std::cout << "[INFO] Backend input switch requested!" << std::endl;
        stt.RequestRestartInput();
        std::cout << "[INFO] Backend input switched!" << std::endl;
    }

    ImGui::EndChild();

}


static void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS) 
        return;
    fprintf(stderr, "[ERROR] (Vulkan) VkResult = %d\n", err);
    Logger::GetInstance().log("\n\n>------------[EXCEPTION]------------<\n\n[ERROR] (Vulkan) VkResult failed!\n");
    if (err < 0)
        abort();
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData) {
    (void)flags; (void)object; (void)location; (void)messageCode; (void)pUserData; (void)pLayerPrefix;
    fprintf(stderr, "[INFO] (Vulkan) Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
    return VK_FALSE;
}
#endif

SurfaceFormat choose_swapchain_surface_format(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface) {
    // query supported surface formats
    uint32_t formatCount = 0;
    VkResult r = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    if (r != VK_SUCCESS || formatCount == 0) {
        throw std::runtime_error("failed to get physical device surface formats");
    }

    std::vector<VkSurfaceFormatKHR> availableFormats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, availableFormats.data());

    // prefer sRGB format with common color space
    for (const auto &f : availableFormats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return { f.format, f.colorSpace };
    }
    for (const auto &f : availableFormats) {
        if (f.format == VK_FORMAT_R8G8B8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return { f.format, f.colorSpace };
    }

    // "any format is fine"
    if (availableFormats.size() == 1 && availableFormats[0].format == VK_FORMAT_UNDEFINED) {
        return { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    }

    // fallback (pick first available)
    return { availableFormats[0].format, availableFormats[0].colorSpace };
}

static bool CheckFile(const char* path) {
    struct stat info;
    if (stat(path, &info) != 0) {
        std::cerr << "[ERROR] File check failed: " << strerror(errno) << "\n[INFO] Path \"" << path << "\" does not exist." << std::endl;
        Logger::GetInstance().log("[ERROR] File check failed: ");
        Logger::GetInstance().log(strerror(errno));
        Logger::GetInstance().log("[\n[INFO] Path \"");
        Logger::GetInstance().log(path);
        Logger::GetInstance().log("\" does not exist.\n");

        return false;
    }

    if (info.st_mode & S_IFREG) {
        return true; // path exists & is a file
    } else {
        std::cerr << "[ERROR] Path \"" << path << "\" is not a file." << std::endl;
        Logger::GetInstance().log("[\n[ERROR] Path \"");
        Logger::GetInstance().log(path);
        Logger::GetInstance().log("\" is not a file.\n");
        return false;
    }
}

static bool CopyFile(const std::string& src, const std::string& dest) {
    std::ifstream sourceFile(src, std::ios::binary);
    if (!sourceFile.is_open()) {
        std::cerr << "[ERROR] Could not open source file (" << src << ")." << std::endl;
        Logger::GetInstance().log("[ERROR] Could not open source file (" );
        Logger::GetInstance().log(src);
        Logger::GetInstance().log(").\n");
        return false;
    }

    std::ofstream destinationFile(dest, std::ios::binary);
    if (!destinationFile.is_open()) {
        Logger::GetInstance().log("[ERROR] Could not open destination file (" );
        Logger::GetInstance().log(dest);
        Logger::GetInstance().log(").\n");
        sourceFile.close();
        return false;
    }

    destinationFile << sourceFile.rdbuf();

    sourceFile.close();
    destinationFile.close();

    return true;
}

// vulkan helpers
uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0xFFFFFFFF;
}

VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; // keep (new api)
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; // keep (new api)
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void endSingleTimeCommands(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkFormat format,
                           VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    barrier.srcAccessMask = 0; // TODO: set based on oldLayout
    barrier.dstAccessMask = 0; // TODO: set based on newLayout

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        Logger::GetInstance().log("\n\n>------------[EXCEPTION]------------<\n\n[ERROR] (Vulkan) transitionImageLayout: Unsupported layout transition\n");
        throw std::invalid_argument("[ERROR] Unsupported layout transition!");
    }

    vkCmdPipelineBarrier(
        cmd,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

void createBuffer(VkDevice device, VkPhysicalDevice physDevice, VkDeviceSize size,
                  VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                  VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        Logger::GetInstance().log("\n\n>------------[EXCEPTION]------------<\n\n[ERROR] (Vulkan) createBuffer: Failed to create buffer!\n");
        throw std::runtime_error("[ERROR] Failed to create buffer!");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(physDevice, memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        Logger::GetInstance().log("\n\n>------------[EXCEPTION]------------<\n\n[ERROR] (Vulkan) vkGetBufferMemoryRequirements: Failed to allocate buffer memory!\n");
        throw std::runtime_error("[ERROR] Failed to allocate buffer memory!");
    }

    vkBindBufferMemory(device, buffer, bufferMemory, 0);
}
//
static void CreateCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
                              | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = g_QueueFamily;   // same family as your graphics queue

    if (vkCreateCommandPool(g_Device, &poolInfo, nullptr, &g_CommandPool) != VK_SUCCESS) {
        Logger::GetInstance().log("\n\n>------------[EXCEPTION]------------<\n\n[ERROR] (Vulkan) CreateCommandPool: Failed to create command pool!\n");
        throw std::runtime_error("[ERROR] Failed to create command pool!");
    }
}

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension) {
    for (const VkExtensionProperties& p : properties) {\
        if (strcmp(p.extensionName, extension)==0) 
            return true;
    }
    return false;
}

static void VkSetup(ImVector<const char*> instance_extensions) {
    VkResult err;
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
volkInitialize();
#endif
    {  
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);        
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        check_vk_result(err);
    
        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char* layers[] = {"VK_LAYER_KRONOS_validation"};
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkLoadInstance(g_Instance);
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = debug_report;
        debug_report_ci.pUserData = nullptr;
        err = f_vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
        check_vk_result(err);
#endif

    }

    // select physical device
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    // select graphics queue family
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);
    // create logical device
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // enumerate physical device extension
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
        CreateCommandPool();
        }

    // descriptor pool
    {
        VkDescriptorPoolSize pool_sizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes)
        pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
}

static void SetupVkWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height) {
    wd->Surface = surface;

    // check for WSI support
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE)
    {
        fprintf(stderr, "[ERROR] (Vulkan) No WSI support on physical device 0\n");
        Logger::GetInstance().log("\n\n>------------[EXCEPTION]------------<\n\n[ERROR] (Vulkan) No WSI support on physical device 0\n");
        exit(-1);
    }
    // create swapchain
    {
        auto chosen = choose_swapchain_surface_format(g_PhysicalDevice, surface);
        swapchain_image_format = chosen.format;
        VkColorSpaceKHR swapchain_color_space = chosen.colorSpace;

        VkSwapchainCreateInfoKHR swapchainInfo = {};
        swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchainInfo.surface = surface;
        swapchainInfo.minImageCount = g_MinImageCount;
        swapchainInfo.imageFormat = swapchain_image_format;
        swapchainInfo.imageColorSpace = swapchain_color_space;

    }

    // select Surface Format
    const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    // select Present Mode
#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));
    //printf("[vulkan] Selected PresentMode = %d\n", wd->PresentMode);

    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount, g_SwapChainImageUsage);}

static void VkCleanup() {
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    // remove the debug report callback
    auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif

    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVkWindow() {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);    // wait indefinitely instead of periodically checking
        check_vk_result(err);

        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    // record imgui primitives into command buffer
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    // submit command buffer
    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

// create a VkSampler
VkSampler CreateFontSampler(VkDevice device) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.maxAnisotropy = 1.0f;
    VkSampler sampler;
    vkCreateSampler(device, &info, nullptr, &sampler);
    return sampler;
}

// create a VkImage + allocate & bind memory
VkImage CreateFontImage(VkDevice device, VkPhysicalDevice phys, int w, int h, VkDeviceMemory& outMemory) {
    VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgInfo.imageType   = VK_IMAGE_TYPE_2D;
    imgInfo.format      = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent      = { (uint32_t)w, (uint32_t)h, 1 };
    imgInfo.mipLevels   = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image;
    vkCreateImage(device, &imgInfo, nullptr, &image);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = req.size;
    // findMemoryType is your helper to pick a memory type index with VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    allocInfo.memoryTypeIndex = findMemoryType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vkAllocateMemory(device, &allocInfo, nullptr, &outMemory);
    vkBindImageMemory(device, image, outMemory, 0);
    return image;
}

// create a VkImageView
VkImageView CreateFontImageView(VkDevice device, VkImage image) {
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1, 0,1 };
    VkImageView view;
    vkCreateImageView(device, &viewInfo, nullptr, &view);
    return view;
}

void UploadFontPixels(VkDevice device, VkPhysicalDevice physDevice,
                      VkCommandPool cmdPool, VkQueue queue,
                      VkImage image, unsigned char* pixels, int width, int height) {
    // create a staging buffer
    VkDeviceSize imageSize = uint64_t(width) * height * 4; // RGBA8
    
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    createBuffer(device, physDevice,
                 imageSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingMemory);

    // copy pixel data into the buffer
    void* mapped;
    vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
    memcpy(mapped, pixels, size_t(imageSize));
    vkUnmapMemory(device, stagingMemory);

    // record commands to transition image and copy buffer→image
    VkCommandBuffer cmd = beginSingleTimeCommands(device, cmdPool);

    //undef > transfer‐dst
    transitionImageLayout(cmd, image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // copy
    VkBufferImageCopy region{};
    region.bufferOffset                    = 0;
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset                     = { 0, 0, 0 };
    region.imageExtent                     = { uint32_t(width), uint32_t(height), 1 };
    vkCmdCopyBufferToImage(cmd,
                           stagingBuffer,
                           image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);

    // transfer‐dst > shader‐read
    transitionImageLayout(cmd, image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    endSingleTimeCommands(device, cmdPool, queue, cmd);

    // cleanup staging resources
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
}

std::array<float,4> ImVec4ToFloats(const ImVec4 &c) {
    return { c.x, c.y, c.z, c.w };
}
inline ImVec4 FloatsToImVec4(const std::array<float,4>& a) noexcept {
    return ImVec4(a[0], a[1], a[2], a[3]);
}

static int Shutdown(VkSurfaceKHR g_Surface) {

        // wait for device idle if available
        if (g_Device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g_Device);
        }

        // shutdown ImGui renderer & platform backends (renderer first)
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL2_Shutdown();

        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::DestroyContext();
        }

#ifdef IMGUI_IMPL_VULKANH_HEADERS_AVAILABLE
        if (g_Instance != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
            ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
        }
#endif

        // destroy Vulkan objects
        if (g_CommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(g_Device, g_CommandPool, g_Allocator);
            g_CommandPool = VK_NULL_HANDLE;
        }

        if (g_DescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
            g_DescriptorPool = VK_NULL_HANDLE;
        }

        if (g_PipelineCache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(g_Device, g_PipelineCache, g_Allocator);
            g_PipelineCache = VK_NULL_HANDLE;
        }

        if (g_Device != VK_NULL_HANDLE) {
            vkDestroyDevice(g_Device,g_Allocator);
            g_Device = VK_NULL_HANDLE;
        }

        if (g_Instance != VK_NULL_HANDLE) {
            vkDestroyInstance(g_Instance, g_Allocator);
            g_Instance = VK_NULL_HANDLE;
        }

        if (g_Surface != VK_NULL_HANDLE && g_Instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(g_Instance, g_Surface, g_Allocator);
            g_Surface = VK_NULL_HANDLE;
        }

        // clear globals
        g_PhysicalDevice = VK_NULL_HANDLE;
        g_Queue = VK_NULL_HANDLE;
        g_QueueFamily = (uint32_t)-1;
        g_MinImageCount = 2;
        g_SwapChainRebuild = false;

        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        SDL_Quit();
        return 0;
}

void Interface::Minimize() {
    if (!window) return;
    // stop rendering loop
    g_RenderPaused.store(true, std::memory_order_release);

    // hide or minimize the window
    SDL_HideWindow(window);
    //SDL_MinimizeWindow(g_Window); // minimize to taskbar - scrapped (will not work on some WMs)
}

void Interface::Show() {
    if (!window) return;

    // show and bring front
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
    SDL_RestoreWindow(window);

    // may need to recreate swapchain if window size changed while hidden

    g_RenderPaused.store(false, std::memory_order_release);
}

uint32_t findTextureMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++)
        if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;

    return 0xFFFFFFFF; // unable to find memoryType
}

// LoadTextureFromFile code from: https://github.com/ocornut/imgui/wiki/Image-Loading-and-Displaying-Examples
static bool LoadTextureFromFile(const char* filename, TextureData* tex_data) {
    // specifying 4 channels forces stb to load the image in RGBA which is an easy format for Vulkan
    tex_data->Channels = 4;
    unsigned char* image_data = stbi_load(filename, &tex_data->width, &tex_data->height, 0, tex_data->Channels);

    if (image_data == NULL)
        return false;

    // calculate allocation size (in number of bytes)
    size_t image_size = tex_data->width * tex_data->height * tex_data->Channels;

    VkResult err;

    // Create the Vulkan image.
    {
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent.width = tex_data->width;
        info.extent.height = tex_data->height;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        err = vkCreateImage(g_Device, &info, g_Allocator, &tex_data->Image);
        check_vk_result(err);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g_Device, tex_data->Image, &req);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = findTextureMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        err = vkAllocateMemory(g_Device, &alloc_info, g_Allocator, &tex_data->ImageMemory);
        check_vk_result(err);
        err = vkBindImageMemory(g_Device, tex_data->Image, tex_data->ImageMemory, 0);
        check_vk_result(err);
    }

    // Create the Image View
    {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = tex_data->Image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        err = vkCreateImageView(g_Device, &info, g_Allocator, &tex_data->ImageView);
        check_vk_result(err);
    }

    // Create Sampler
    {
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT; // outside image bounds just use border color
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.minLod = -1000;
        sampler_info.maxLod = 1000;
        sampler_info.maxAnisotropy = 1.0f;
        err = vkCreateSampler(g_Device, &sampler_info, g_Allocator, &tex_data->Sampler);
        check_vk_result(err);
    }

    // Create Descriptor Set using ImGUI's implementation
    tex_data->DS = ImGui_ImplVulkan_AddTexture(tex_data->Sampler, tex_data->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Create Upload Buffer
    {
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = image_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        err = vkCreateBuffer(g_Device, &buffer_info, g_Allocator, &tex_data->UploadBuffer);
        check_vk_result(err);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_Device, tex_data->UploadBuffer, &req);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = findTextureMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        err = vkAllocateMemory(g_Device, &alloc_info, g_Allocator, &tex_data->UploadBufferMemory);
        check_vk_result(err);
        err = vkBindBufferMemory(g_Device, tex_data->UploadBuffer, tex_data->UploadBufferMemory, 0);
        check_vk_result(err);
    }

    // Upload to Buffer:
    {
        void* map = NULL;
        err = vkMapMemory(g_Device, tex_data->UploadBufferMemory, 0, image_size, 0, &map);
        check_vk_result(err);
        memcpy(map, image_data, image_size);
        VkMappedMemoryRange range[1] = {};
        range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range[0].memory = tex_data->UploadBufferMemory;
        range[0].size = image_size;
        err = vkFlushMappedMemoryRanges(g_Device, 1, range);
        check_vk_result(err);
        vkUnmapMemory(g_Device, tex_data->UploadBufferMemory);
    }

    // Release image memory using stb
    stbi_image_free(image_data);

    // Create a command buffer that will perform following steps when hit in the command queue.
    // TODO: this works in the example, but may need input if this is an acceptable way to access the pool/create the command buffer.
    VkCommandPool command_pool = g_MainWindowData.Frames[g_MainWindowData.FrameIndex].CommandPool;
    VkCommandBuffer command_buffer;
    {
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandPool = command_pool;
        alloc_info.commandBufferCount = 1;

        err = vkAllocateCommandBuffers(g_Device, &alloc_info, &command_buffer);
        check_vk_result(err);

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(command_buffer, &begin_info);
        check_vk_result(err);
    }

    // Copy to Image
    {
        VkImageMemoryBarrier copy_barrier[1] = {};
        copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copy_barrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier[0].image = tex_data->Image;
        copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_barrier[0].subresourceRange.levelCount = 1;
        copy_barrier[0].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, copy_barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = tex_data->width;
        region.imageExtent.height = tex_data->height;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(command_buffer, tex_data->UploadBuffer, tex_data->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier use_barrier[1] = {};
        use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        use_barrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        use_barrier[0].image = tex_data->Image;
        use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        use_barrier[0].subresourceRange.levelCount = 1;
        use_barrier[0].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, use_barrier);
    }

    // End command buffer
    {
        VkSubmitInfo end_info = {};
        end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        end_info.commandBufferCount = 1;
        end_info.pCommandBuffers = &command_buffer;
        err = vkEndCommandBuffer(command_buffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &end_info, VK_NULL_HANDLE);
        check_vk_result(err);
        err = vkDeviceWaitIdle(g_Device);
        check_vk_result(err);
    }

    return true;
}

void RemoveTexture(TextureData* tex_data) {
    vkFreeMemory(g_Device, tex_data->UploadBufferMemory, nullptr);
    vkDestroyBuffer(g_Device, tex_data->UploadBuffer, nullptr);
    vkDestroySampler(g_Device, tex_data->Sampler, nullptr);
    vkDestroyImageView(g_Device, tex_data->ImageView, nullptr);
    vkDestroyImage(g_Device, tex_data->Image, nullptr);
    vkFreeMemory(g_Device, tex_data->ImageMemory, nullptr);
    ImGui_ImplVulkan_RemoveTexture(tex_data->DS);
}

static int ResizeCallback(ImGuiInputTextCallbackData* data) { // for string usage inside ImGui::InputText()
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        // user-data is the std::string pointer
        std::string* str = (std::string*)data->UserData;
        str->resize(data->BufTextLen); // resize string to new text length (not counting null)
        data->Buf = const_cast<char*>(str->c_str()); // update ImGui's buffer pointer
    }
    return 0;
}

std::string Interface::ReadFileToString() {
    std::ifstream file(cfg.logpath);
    if (!file.is_open()) {
        return "[Error] Could not log file";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::future<bool> Interface::prompt_user_async(std::string context) {
    auto p = std::make_unique<prompt>(PromptType::boolean, std::move(context));
    std::future<bool> f = p->promise_bool.get_future();
    {
        // push into queue
        std::lock_guard<std::mutex> lk(prompt_mutex_);
        prompt_queue_.push_back(std::move(p));
    }
    return f;
}

std::future<std::string> Interface::prompt_user_async_string(std::string context) {
    auto p = std::make_unique<prompt>(PromptType::string, std::move(context), /*buf_size=*/1024);
    std::future<std::string> f = p->promise_str.get_future();
    {
        std::lock_guard<std::mutex> lk(prompt_mutex_);
        prompt_queue_.push_back(std::move(p));
    }
    return f;
}

bool Interface::render_prompt() {
    // {
    {
        std::lock_guard<std::mutex> lk(prompt_mutex_);
        if (!active_prompt_ && !prompt_queue_.empty()) {
            // move the front prompt into active_prompt_
            active_prompt_ = std::move(prompt_queue_.front());
            prompt_queue_.pop_front();
            
            ImGui::OpenPopup("Prompt##Interface");
        }
    }

    if (!active_prompt_) return false;
    else return true;
    
} 

int Interface::Render(std::atomic<bool>* runningFlag) {
    // configuration path
    char config_path[PATH_MAX];
    char image_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/%s", getenv("HOME"), ".config/konacode/konamask/config.ini");
    snprintf(image_path, sizeof(image_path), "%s/%s", getenv("HOME"), ".config/konacode/konamask/background");
    // create window with Vulkan graphics context
    float main_scale = ImGui_ImplSDL2_GetContentScaleForDisplay(0);
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    window = SDL_CreateWindow("konamask", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(1280 * main_scale), (int)(720 * main_scale), window_flags);
    if (window == nullptr) {
        printf("[ERROR] (Vulkan/SDL2) SDL_CreateWindow(): %s\n", SDL_GetError());
        Logger::GetInstance().log("\n\n>------------[EXCEPTION]------------<\n\n[ERROR] (Vulkan/SDL2) SDL_CreateWindow():\n");
        Logger::GetInstance().log(SDL_GetError());
        Logger::GetInstance().log("\n");
        return -1;
    }
    std::cout << "[INFO] (Vulkan/SDL2) Window created successfully!" << std::endl;
        Logger::GetInstance().log("[INFO] (Vulkan/SDL2) Window created successfully!\n");

    ImVector<const char*> extensions;
    uint32_t extensions_count = 0;
    SDL_Vulkan_GetInstanceExtensions(window, &extensions_count, nullptr);
    extensions.resize(extensions_count);
    SDL_Vulkan_GetInstanceExtensions(window, &extensions_count, extensions.Data);
    VkSetup(extensions);

    // create window surface
    VkSurfaceKHR surface;
    VkResult err;
    if (SDL_Vulkan_CreateSurface(window, g_Instance, &surface) == 0) {
        printf("[ERROR] (Vulkan/SDL2) Failed to create Vulkan/SDL2 surface.\n");
        Logger::GetInstance().log("[ERROR] (Vulkan/SDL2) Failed to create Vulkan/SDL2 surface.\n");
        return 1;
    }
    std::cout << "[INFO] (Vulkan/SDL2) Successfully created Vulkan/SDL2 surface!" << std::endl;
    Logger::GetInstance().log("[INFO] (Vulkan/SDL2) Successfully created Vulkan/SDL2 surface!\n");

    // create framebuffers
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVkWindow(wd, surface, w, h);

    // setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // enable keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // enable gamepad Controls

    // setup ImGui style
    //ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // apply user configuration
    float tcr;
    float tcg;
    float tcb;
    // validate range & convert to normalized floats
    auto in_range = [](int v){ return (v >= 0 && v <= 255); };
    if (!in_range(cfg.get<int>("ui_theme_red",   50)) || !in_range(cfg.get<int>("ui_theme_green", 20)) || !in_range(cfg.get<int>("ui_theme_blue",  60))) {
        std::cerr << "[ERROR] Color parse/validation failed, reverting to defaults." << std::endl;
        Logger::GetInstance().log("[ERROR] Color parse/validation failed, reverting to defaults.\n");
        tcr = static_cast<float>(50); tcg = static_cast<float>(20); tcb = static_cast<float>(60);
    } 
    else {
        // convert to normalized floats 0.0 - 1.0
        tcr = static_cast<float>(cfg.get<int>("ui_theme_red",   167)) / 255.0f;
        tcg = static_cast<float>(cfg.get<int>("ui_theme_green", 42)) / 255.0f;
        tcb = static_cast<float>(cfg.get<int>("ui_theme_blue",  92)) / 255.0f;
    }
    ImVec4 theme_color(tcr, tcg, tcb, 1.0f); // only needed for the GUI

    ImGuiStyle& style = ImGui::GetStyle();
    // style.Colors[ImGuiCol_Text]                  = ImVec4(0.86f, 0.93f, 0.89f, 0.78f);
    // style.Colors[ImGuiCol_TextDisabled]          = ImVec4(0.86f, 0.93f, 0.89f, 0.28f);
    // style.Colors[ImGuiCol_WindowBg]              = ImVec4(0.05f, 0.06f, 0.12f, 0.86f);
    // style.Colors[ImGuiCol_Border]                = ImVec4(0.02f, 0.03f, 0.09f, 0.86f);
    // style.Colors[ImGuiCol_BorderShadow]          = ImVec4(0.02f, 0.03f, 0.09f, 0.00f);
    // style.Colors[ImGuiCol_FrameBg]               = ImVec4(0.20f, 0.22f, 0.27f, 0.78f);
    // style.Colors[ImGuiCol_FrameBgHovered]        = ImVec4(tcr, tcg, tcb, 0.45f);
    // style.Colors[ImGuiCol_FrameBgActive]         = ImVec4(tcr, tcg, tcb, 0.72f);
    style.Colors[ImGuiCol_TitleBg]               = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.20f, 0.22f, 0.27f, 0.75f);
    style.Colors[ImGuiCol_TitleBgActive]         = ImVec4(tcr, tcg, tcb, 0.86f);
    // style.Colors[ImGuiCol_MenuBarBg]             = ImVec4(0.20f, 0.22f, 0.27f, 0.47f);
    style.Colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.0f, 0.0f, 0.0f, 0.00f);
    // style.Colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.09f, 0.15f, 0.16f, 0.86f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(tcr, tcg, tcb, 0.36f);
    style.Colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(tcr, tcg, tcb, 0.86f);
    style.Colors[ImGuiCol_CheckMark]             = ImVec4(tcr, tcg, tcb, 1.00f);
    style.Colors[ImGuiCol_SliderGrab]            = ImVec4(tcr, tcg, tcb, 0.96f);
    style.Colors[ImGuiCol_SliderGrabActive]      = ImVec4(tcr, tcg, tcb, 0.10f);
    // style.Colors[ImGuiCol_Button]                = ImVec4(0.47f, 0.77f, 0.83f, 0.14f);
    // style.Colors[ImGuiCol_ButtonHovered]         = ImVec4(tcr, tcg, tcb, 0.86f);
    // style.Colors[ImGuiCol_ButtonActive]          = ImVec4(tcr, tcg, tcb, 1.00f);
    // style.Colors[ImGuiCol_Header]                = ImVec4(tcr, tcg, tcb, 0.76f);
    // style.Colors[ImGuiCol_HeaderHovered]         = ImVec4(tcr, tcg, tcb, 0.86f);
    style.Colors[ImGuiCol_Separator]             = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
    style.Colors[ImGuiCol_SeparatorHovered]      = ImVec4(tcr, tcg, tcb, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive]       = ImVec4(tcr, tcg, tcb, 1.00f);
    style.Colors[ImGuiCol_ResizeGrip]            = ImVec4(0.47f, 0.77f, 0.83f, 0.04f);
    style.Colors[ImGuiCol_ResizeGripHovered]     = ImVec4(tcr, tcg, tcb, 0.78f);
    style.Colors[ImGuiCol_ResizeGripActive]      = ImVec4(tcr, tcg, tcb, 1.00f);
    // style.Colors[ImGuiCol_PlotLines]             = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    // style.Colors[ImGuiCol_PlotLinesHovered]      = ImVec4(tcr, tcg, tcb, 1.00f);
    // style.Colors[ImGuiCol_PlotHistogram]         = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    style.Colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(tcr, tcg, tcb, 1.00f);
    style.Colors[ImGuiCol_TextSelectedBg]        = ImVec4(tcr, tcg, tcb, 0.45f);
    // style.Colors[ImGuiCol_PopupBg]               = ImVec4(0.20f, 0.22f, 0.27f, 0.9f);

    style.Colors[ImGuiCol_WindowBg]     = ImVec4(0.04f, 0.05f, 0.06f, 0.64f);
    style.Colors[ImGuiCol_ChildBg]      = ImVec4(0.06f, 0.07f, 0.08f, 0.56f);
    style.Colors[ImGuiCol_PopupBg]      = ImVec4(0.06f, 0.07f, 0.08f, 0.92f);
    style.Colors[ImGuiCol_Border]       = ImVec4(0.14f, 0.16f, 0.18f, 0.25f);

    // text
    style.Colors[ImGuiCol_Text]         = ImVec4(0.93f, 0.94f, 0.95f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.58f, 0.60f, 1.00f);

    // accent
    ImVec4 accent = ImVec4(0.30f, 0.55f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_Header]         = ImVec4(tcr, tcg, tcb, 0.30f);
    style.Colors[ImGuiCol_HeaderHovered]  = ImVec4(tcr, tcg, tcb, 0.95f);
    style.Colors[ImGuiCol_HeaderActive]   = ImVec4(tcr*0.8, tcg*0.8, tcb*0.8, 0.86f);
    style.Colors[ImGuiCol_Button]         = ImVec4(accent.x*0.10f, accent.y*0.12f, accent.z*0.18f, 0.6f);
    style.Colors[ImGuiCol_ButtonHovered]  = ImVec4(tcr, tcg, tcb, 0.95f);
    style.Colors[ImGuiCol_ButtonActive]   = ImVec4(tcr*0.5, tcg*0.5, tcb*0.5, 0.95f);

    // frames / inputs
    style.Colors[ImGuiCol_FrameBg]        = ImVec4(0.09f, 0.10f, 0.11f, 0.7f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.14f, 0.16f, 0.95f);
    style.Colors[ImGuiCol_FrameBgActive]  = ImVec4(tcr, tcg, tcb, 1.00f);

    // small UI niceties
    style.Colors[ImGuiCol_Separator]      = ImVec4(0.12f,0.14f,0.16f,0.45f);
    //C[ImGuiCol_TooltipBg]      = ImVec4(0.06f,0.07f,0.08f,0.95f);

    // rounding / spacing
    style.WindowRounding    = 1.0f;
    style.ChildRounding     = 1.0f;
    style.FrameRounding     = 1.0f;
    style.ScrollbarRounding = 8.0f;
    style.ItemSpacing       = ImVec2(10, 8);
    style.WindowPadding     = ImVec2(10, 8);
    style.FramePadding      = ImVec2(10, 8);


    style.ScaleAllSizes(main_scale);    // bake a fixed style scale
    style.ScaleAllSizes(main_scale);    // bake a fixed style scale
    // io.FontGlobalScale = main_scale;               // scales all fonts by main_scale
    // io.Fonts->Clear();
    // if (cfg.get<std::string>("ui_custom_font", "") != "false") {
    //     io.Fonts->AddFontFromFileTTF(cfg.get<std::string>("ui_custom_font", "").c_str(), cfg.get<int>("ui_font_size", 24) * main_scale);
    // }
    // else {
    //     std::cout << "[INFO] Font parameter was disabled, using default!" << std::endl;
    //     io.Fonts->AddFontDefault();
    // }
    
    
    // setup platform/renderer backends
    ImGui_ImplSDL2_InitForVulkan(window);
    
    if (swapchain_image_format == VK_FORMAT_UNDEFINED) {
        std::cerr << "[ERROR] (Vulkan): swapchain_image_format not selected before initializing ImGui Vulkan backend" << std::endl;
        // TODO: handle error
    }


    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = swapchain_image_format; // set earlier by choose_swapchain_surface_format()
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo rpci = {};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &colorAttachment;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;

    VkRenderPass imguiRenderPass = VK_NULL_HANDLE;
    VkResult VkRes = vkCreateRenderPass(g_Device, &rpci, nullptr, &imguiRenderPass);
    if (VkRes != VK_SUCCESS) {
        fprintf(stderr, "vkCreateRenderPass failed: %d\n", VkRes);
        // TODO: handle error
    }


    // ImGui_ImplVulkan_InitInfo init_info = {};

    //init_info.ApiVersion = VK_API_VERSION_1_3;              // pass in value of VkApplicationInfo::apiVersion, otherwise will default to header version.
//     init_info.Instance = g_Instance;
//     init_info.PhysicalDevice = g_PhysicalDevice;
//     init_info.Device = g_Device;
//     init_info.QueueFamily = g_QueueFamily;
//     init_info.Queue = g_Queue;
//     init_info.PipelineCache = g_PipelineCache;
//     init_info.DescriptorPool = g_DescriptorPool;
//     init_info.MinImageCount = g_MinImageCount;
//     init_info.ImageCount = wd->ImageCount;
//     init_info.Allocator = g_Allocator;
// #ifdef IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING // new api change
//     init_info.UseDynamicRendering = true;
//     VkFormat color_fmt = VK_FORMAT_B8G8R8A8_UNORM;
//     VkFormat color_attachment_formats[1] = { color_fmt };
// #else
//     init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
//     init_info.RenderPass = wd->RenderPass;
//     init_info.Subpass = 0;
// #endif
// lower-case comment - fill ImGui_ImplVulkan_InitInfo (zero-clear first)
    ImGui_ImplVulkan_InitInfo init_info = {};
    memset(&init_info, 0, sizeof(init_info));
    init_info.ApiVersion = 0; // optional: leave 0 to use header default
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.DescriptorPoolSize = 0; // optional: 0 uses DescriptorPool
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.PipelineCache = g_PipelineCache;
    init_info.Allocator = g_Allocator;
    init_info.CheckVkResultFn = check_vk_result;
    init_info.MinAllocationSize = 1; // optional, set to 1 or a sensible page size

    // set PipelineInfoMain fields (RenderPass, Subpass, MSAASamples)
    init_info.PipelineInfoMain.RenderPass = imguiRenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT; // 0 would default to 1

    // disable to avoid crashing on other platforms
    init_info.UseDynamicRendering = false;

    ImGui_ImplVulkan_Init(&init_info);

 

    // manual font upload
    {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels = nullptr;
        int texW = 0, texH = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &texW, &texH); // return 4-channel RGBA

        if (pixels == nullptr || texW <= 0 || texH <= 0) {
            // nothing to upload
            fprintf(stderr, "imgui font: no pixels returned\n");
            exit(-1);
        }

        const VkDeviceSize imageSize = (VkDeviceSize)texW * (VkDeviceSize)texH * 4ull;

        // create staging buffer
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;
        createBuffer(g_Device, g_PhysicalDevice,
                     imageSize,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer, stagingBufferMemory);

        // map & copy pixels
        void* mapped = nullptr;
        VkResult res = vkMapMemory(g_Device, stagingBufferMemory, 0, imageSize, 0, &mapped);
        if (res != VK_SUCCESS || mapped == nullptr) {
            fprintf(stderr, "vkMapMemory failed: %d\n", res);
            // cleanup
            if (stagingBuffer != VK_NULL_HANDLE) vkDestroyBuffer(g_Device, stagingBuffer, nullptr);
            if (stagingBufferMemory != VK_NULL_HANDLE) vkFreeMemory(g_Device, stagingBufferMemory, nullptr);
            exit(-1);
        }
        memcpy(mapped, pixels, (size_t)imageSize);
        vkUnmapMemory(g_Device, stagingBufferMemory);

        // create GPU image
        VkImage fontImage = VK_NULL_HANDLE;
        VkDeviceMemory fontImageMemory = VK_NULL_HANDLE;

        auto CreateImage = [&](uint32_t width, uint32_t height, VkFormat format,
                               VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                               VkImage& imageOut, VkDeviceMemory& memoryOut)->bool {
            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = width;
            imageInfo.extent.height = height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = format;
            imageInfo.tiling = tiling;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = usage;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateImage(g_Device, &imageInfo, nullptr, &imageOut) != VK_SUCCESS) {
                fprintf(stderr, "failed to create image\n");
                return false;
            }

            VkMemoryRequirements memReq;
            vkGetImageMemoryRequirements(g_Device, imageOut, &memReq);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memReq.size;
            allocInfo.memoryTypeIndex = findMemoryType(g_PhysicalDevice, memReq.memoryTypeBits, properties);

            if (vkAllocateMemory(g_Device, &allocInfo, nullptr, &memoryOut) != VK_SUCCESS) {
                fprintf(stderr, "failed to allocate image memory\n");
                vkDestroyImage(g_Device, imageOut, nullptr);
                return false;
            }

            vkBindImageMemory(g_Device, imageOut, memoryOut, 0);
            return true;
        };

        // create font image
        if (!CreateImage((uint32_t)texW, (uint32_t)texH, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         fontImage, fontImageMemory)) {
            // cleanup staging
            vkDestroyBuffer(g_Device, stagingBuffer, nullptr);
            vkFreeMemory(g_Device, stagingBufferMemory, nullptr);
            exit(-1);
        }

        // record copy & layout transitions on a single-use command buffer
        VkCommandBuffer cmd = beginSingleTimeCommands(g_Device, g_CommandPool);

        // transition undefined -> transfer dst
        transitionImageLayout(cmd, fontImage, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        // prepare buffer -> image copy region
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { (uint32_t)texW, (uint32_t)texH, 1 };

        vkCmdCopyBufferToImage(cmd, stagingBuffer, fontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // transition transfer dst -> shader read
        transitionImageLayout(cmd, fontImage, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        endSingleTimeCommands(g_Device, g_CommandPool, g_Queue, cmd);
        
        // create image view & sampler
        VkImageView fontView = CreateFontImageView(g_Device, fontImage);
        VkSampler   fontSampler = CreateFontSampler(g_Device);
        // register descriptor
        VkDescriptorSetLayoutBinding fontBinding{};
        fontBinding.binding = 0;
        fontBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        fontBinding.descriptorCount = 1;
        fontBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        fontBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &fontBinding;

        vkCreateDescriptorSetLayout(g_Device, &layoutInfo, nullptr, &g_DescriptorSetLayout);
        VkDescriptorSet desc = ImGui_ImplVulkan_AddTexture(
            fontSampler,
            fontView,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
        // set texture ID
        // io.Fonts->SetTexID((ImTextureID)desc);
        // free staging resources

        io.FontGlobalScale = main_scale;               // scales all fonts by main_scale
        io.Fonts->Clear();
        ImFontConfig f_cfg;
        f_cfg.FontDataOwnedByAtlas=false;
        if (cfg.get<std::string>("ui_custom_font", "") != "false") {
            if (CheckFile(cfg.get<std::string>("ui_custom_font", "").c_str())&&!cfg.get<std::string>("ui_custom_font", "").empty()) {
                io.Fonts->AddFontFromFileTTF(cfg.get<std::string>("ui_custom_font", "").c_str(), cfg.get<int>("ui_font_size", 24) * main_scale);
            } else {
                io.Fonts->AddFontFromMemoryTTF(dmsans_semibold, sizeof(dmsans_semibold), cfg.get<int>("ui_font_size", 24) * main_scale, &f_cfg);
                // io.Fonts->AddFontDefault();
                Logger::GetInstance().log("[ERROR] Custom font file (\""+cfg.get<std::string>("ui_custom_font", "")+"\") does not exist, using default!\n");
            }
        }
        else {
            std::cout << "[INFO] Font parameter was disabled, using default!" << std::endl;
            io.Fonts->AddFontFromMemoryTTF(dmsans_semibold, sizeof(dmsans_semibold), cfg.get<int>("ui_font_size", 24) * main_scale, &f_cfg);
            // io.Fonts->AddFontDefault();
        }
        vkDestroyBuffer(g_Device, stagingBuffer, nullptr);
        vkFreeMemory(g_Device, stagingBufferMemory, nullptr);

        // io.Fonts->Clear();
    
    }

    // icon font upload
    ImFontConfig f_icon_cfg;
    f_icon_cfg.PixelSnapH=true;
    f_icon_cfg.FontDataOwnedByAtlas=false;
    f_iconData = io.Fonts->AddFontFromMemoryTTF(iconData, sizeof(iconData), main_scale*14, &f_icon_cfg);


    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
    // configuration variables (currently no way of avoiding)
    //tts
    int sr = cfg.get<int>("speech_rate", 150);
    int sp = cfg.get<int>("speech_pitch", 50);
    int sv = cfg.get<int>("speech_volume", 100);
    std::string voicebank = cfg.get<std::string>("speech_vociebank", "en-us");
    voicebank.reserve(256);
    voicebank.resize(voicebank.size());
    //stt
    std::string voskapi = cfg.get<std::string>("voskapi_model_path", "./model");
    voskapi.reserve(256);
    voskapi.resize(voskapi.size());
    unsigned int sthreshold = cfg.get<int>("silence_threshold", 200);
    unsigned int stimeout = cfg.get<int>("silence_timeout", 1000);
    // advanced
    int pasamplerate = cfg.get<int>("pa_sample_spec_rate", 22050);
    double bufferfactor = cfg.get<double>("buffer_factor", 0.05);

    std::string fontdir = cfg.get<std::string>("ui_custom_font", "false");
    fontdir.reserve(256);
    fontdir.resize(fontdir.size());
    int fontsize = cfg.get<int>("ui_font_size", 14);
    
    // rendering variables
    bool budgetfm = false;    
    char bgsuccess = 'u'; // unset
    bool debug_log_enabled = cfg.get<bool>("enable_logging_to_file", false);
    bool stats = false;
    static std::string logFile = "";
    ImGuiListClipper clipperC;

    //bool imgbg = (cfg.get<bool>("enable_custom_background", false) &&CheckFile(image_path)); throws (idk why)
    bool imgbg = CheckFile(image_path) && cfg.get<bool>("enable_custom_background", false);
    float r;
    float g;
    float b;
    
    char usr[PATH_MAX];
    snprintf(usr, sizeof(usr), "%s", getenv("HOME"));
    static char manInput[512] = "";
    // validate range & convert to normalized floats
    //auto in_range = [](int v){ return (v >= 0 && v <= 255); }; defined above
    if (!in_range(cfg.get<int>("ui_bgc_red",   50)) || !in_range(cfg.get<int>("ui_bgc_green", 20)) || !in_range(cfg.get<int>("ui_bgc_blue",  60))) {
        std::cerr << "[ERROR] Color parse/validation failed, reverting to defaults." << std::endl;
        Logger::GetInstance().log("[ERROR] Color parse/validation failed, reverting to defaults.\n");
        r = static_cast<float>(50); g = static_cast<float>(20); b = static_cast<float>(60);
    } 
    else {
        // convert to normalized floats 0.0 - 1.0
        r = static_cast<float>(cfg.get<int>("ui_bgc_red",   50)) / 255.0f;
        g = static_cast<float>(cfg.get<int>("ui_bgc_green", 20)) / 255.0f;
        b = static_cast<float>(cfg.get<int>("ui_bgc_blue",  60)) / 255.0f;
    }
    ImVec4 backgorund_color(r, g, b, 1.0f);
    // load user background image
    TextureData texture;
    bool ret = LoadTextureFromFile(image_path, &texture);
    if (imgbg) {
        IM_ASSERT(ret);
    }

        enum Tabs {OVERVIEW_TAB,MANUAL_INPUT_TAB,LOG_TAB,SETTINGS_TAB,DEBUG_TAB};
        enum Tabs c_tab = OVERVIEW_TAB;
        bool open = false;
        short sidebar_size = -64;
        visualizer.enabled.store(true);

    ImGuiFilePicker picker;
    std::string out_path;
    SDL_Event event;
    int fb_width, fb_height;
    while (runningFlag->load()) {
        
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                break;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                break;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }
        if (g_RenderPaused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // resize swap chain?
        SDL_GetWindowSize(window, &fb_width, &fb_height);
        if (fb_width > 0 && fb_height > 0 && (g_SwapChainRebuild || g_MainWindowData.Width != fb_width || g_MainWindowData.Height != fb_height)) {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);

            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount, g_SwapChainImageUsage);           g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }

        // start the ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        //ImGui::SetNextWindowViewport(viewport->ID);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##DockRoot", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();


        // top nav
        ImGui::BeginChild("TopNav", ImVec2(0,46), false, ImGuiWindowFlags_NoScrollbar);
        
        ImGui::SetCursorPosX(20.0f);
        ImGui::SetCursorPosY(15.0f);
        ImGui::PushFont(f_iconData); 
        ImGui::Text(""); ImGui::SameLine();
        ImGui::PopFont();
        ImGui::TextColored(ImVec4(0.86f,0.88f,0.92f,1.0f), "%s", "KONAMASK");

        // // account button at right
        // ImGui::SetCursorPosX(fb_width - 310.0f);
        // ImGui::SetCursorPosY(10.0f);
        // if (ImGui::Button("Settings##top", ImVec2(100.0f, 28.0f))) settings = !settings;
        // ImGui::SetCursorPosX(fb_width - 200.0f);
        // ImGui::SetCursorPosY(10.0f);
        // if (ImGui::Button("Log##top", ImVec2(100.0f, 28.0f))) debug_log = !debug_log;
        ImGui::SetCursorPosX(fb_width - 92.0f);
        ImGui::SetCursorPosY(10.0f);
        ImGui::PushFont(f_iconData); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0,0));
        if (ImGui::Button("", ImVec2(28.0f, 28.0f))) Minimize();
        ImGui::PopFont(); ImGui::PopStyleVar();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Minimize to tray");
        ImGui::SetCursorPosX(fb_width - 60.0f);
        ImGui::SetCursorPosY(10.0f);
        ImGui::PushFont(f_iconData); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0,0));
        if (ImGui::Button("9", ImVec2(28.0f, 28.0f))) {
            cfg.SaveToFile(cfg.logpath);
            if (!Shutdown(surface)) {
                    stt.Stop();
                    std::cout << "[ERROR] (Vulkan/SDL2) Unable to shutdown properly!" << std::endl;
                    Logger::GetInstance().log("[ERROR] (Vulkan/SDL2) Unable to shutdown properly!\n");
                    visualizer.initialized.store(false);
                    // std::exit(EXIT_FAILURE);
                }
            }
        ImGui::PopFont(); ImGui::PopStyleVar();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Quit koncloak");
        
        ImGui::PushItemWidth(200.0f);
        ImGui::PopItemWidth();
        ImGui::EndChild();

        ImGui::BeginChild("##Visualisation", ImVec2(fb_width/2.0, 0), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPosY(16.0f);
        ImGui::TextColored(ImVec4(0.86f,0.88f,0.92f,1.0f), "%s", "INPUT VISUALIZATION");
        visualizer.Process();
        if (visualizer.initialized) {
            ImGui::SetCursorPosY(44.0f);
            ImGui::Separator();
            visualizer.render(&cfg);
            ImGui::SetCursorPos(ImVec2(fb_width-ImGui::CalcTextSize("Stop graph rendering").x-2,fb_height-ImGui::CalcTextSize("Stop graph rendering").y-2));
            if (ImGui::Button("Enable")) {
                visualizer.enabled.store(true);
            }
        } else if (!visualizer.enabled.load()) {
            ImGui::SetCursorPosY(44.0f);
            ImGui::Separator();
            ImGui::SetCursorPosX(ImGui::GetColumnWidth()/2-ImGui::CalcTextSize("Graph rendering is disabled").x/2);
            ImGui::SetCursorPosY(fb_height/2.5f+10.0f);
            ImGui::Text("Graph rendering is disabled.");
            ImGui::SetCursorPosX(ImGui::GetColumnWidth()/2-ImGui::CalcTextSize("Enable").x/2);
            if (ImGui::Button("Enable")) {
                visualizer.enabled.store(true);
            }

        } else {
            ImGui::SetCursorPosY(44.0f);
            ImGui::Separator();
            ImGui::SetCursorPosX(ImGui::GetColumnWidth()/2-ImGui::CalcTextSize("Initializing graphs").x/2);
            ImGui::SetCursorPosY(fb_height/2.5f+10.0f);
            ImGui::Text("Initializing graphs...");
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("##configuration", ImVec2( sidebar_size, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        switch (c_tab) {
            case OVERVIEW_TAB:
                stt.render();
            break;

            case MANUAL_INPUT_TAB:
                ImGui::BeginChild("##title_header_tts", ImVec2(0, 54), false, ImGuiWindowFlags_NoScrollbar);
                ImGui::SetCursorPosX(16);
                ImGui::SetCursorPosY(20.8f);
                ImGui::TextColored(ImVec4(0.86f,0.88f,0.92f,1.0f), "%s", "OUTPUT MANAGEMENT");
                ImGui::EndChild();

                ImGui::BeginChild("##tts_content", ImVec2(0, ImGui::GetWindowHeight()-62), true);

                ImGui::SetCursorPosX(20.0f); ImGui::SetCursorPosY(20.0f);
                ImGui::PushFont(f_iconData); 
                ImGui::Text("I");
                ImGui::PopFont(); ImGui::SameLine();
                ImGui::SetCursorPosY(20.0f); ImGui::SetCursorPosX(40.0f);
                ImGui::Text("Generated voice parameters:");
                TextToSpeech::render();

                // ImGui::InputTextMultiline("##Input", (char*)&manInput, IM_ARRAYSIZE(manInput), ImVec2(ImGui::GetWindowWidth()-40.0f, ImGui::GetWindowHeight()/2.8f));
                // ImGui::SetCursorPosX(ImGui::GetWindowWidth()-230.0f);
                // if (ImGui::Button("Speak through the microphone", ImVec2(200.0f,34.0f))) TextToSpeech::Verbalize((char*)&manInput);
                ImGui::EndChild();
            break;

            case LOG_TAB:
                ImGui::BeginChild("##title_header_log", ImVec2(0, 54), false, ImGuiWindowFlags_NoScrollbar);
                ImGui::SetCursorPosX(16);
                ImGui::SetCursorPosY(20.8f);
                ImGui::TextColored(ImVec4(0.86f,0.88f,0.92f,1.0f), "%s", "LOG");
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetWindowWidth()-ImGui::CalcTextSize("OPEN LOG FILE").x-24);
                ImGui::SetCursorPosY(20.8f);
                ImGui::TextLinkOpenURL("OPEN LOG FILE", cfg.logpath);
                ImGui::EndChild();

                ImGui::BeginChild("##log_content", ImVec2(ImGui::GetWindowWidth(), ImGui::GetWindowHeight()-62), false);
                ImGui::SetCursorPosY(20.0f);
                logFile = ReadFileToString();
                ImGui::TextWrapped("%s", logFile.c_str());
                ImGui::EndChild();
            break;

            case SETTINGS_TAB:
                ImGui::BeginChild("##title_header_cfg", ImVec2(0, 54), false, ImGuiWindowFlags_NoScrollbar);
                ImGui::SetCursorPosX(16);
                ImGui::SetCursorPosY(20.8f);
                ImGui::TextColored(ImVec4(0.86f,0.88f,0.92f,1.0f), "%s", "MISCELLANEOUS SETTINGS");
                ImGui::EndChild();
            
                ImGui::BeginChild("##cfg_content", ImVec2(0, ImGui::GetWindowHeight()-62), true);
                ImGui::SetCursorPosY(20.0f);
                if (imgbg) { 
                    ImGui::Text("Change background image:");
                    if (budgetfm) {
                        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                        ImGui::Button("Select new background");
                        ImGui::PopItemFlag();
                        ImGui::PopStyleVar();
                        if (picker.Draw("pick an image", &budgetfm, &out_path)) {
                            printf("[INFO] Selected new background: %s\n[INFO] Replacing the current image...\n", out_path.c_str());
                                if (CopyFile(out_path,image_path)) {
                                    RemoveTexture(&texture);
                                    ret = LoadTextureFromFile(image_path, &texture);
                                    IM_ASSERT(ret);
                                    std::cout << "[INFO] Applied new image successfully!" << std::endl;
                                    bgsuccess = 's'; // set
                                } else { std::cout << "[ERROR] Could not update the background image!" << std::endl; bgsuccess = 'f'; } // fail
                            budgetfm = false;
                        }
                    }
                    else {
                        if (ImGui::Button("Select new background"))
                            budgetfm = true;
                        if (bgsuccess == 's') {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(18,192,18,255));
                            ImGui::Text("Successfully updated the background!");
                            ImGui::PopStyleColor();
                        } else if (bgsuccess == 'f') {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(242,37,85,255));
                            ImGui::Text("Failed to update the background!");
                            ImGui::PopStyleColor();                        
                        }
                    }


                } 
                else {
                    ImGui::Text("Change background color:");
                    ImGui::ColorEdit3("", (float*)&backgorund_color);
                }
                ImGui::Spacing();
                ImGui::Text("Change theme color:");
                ImGui::ColorEdit3("", (float*)&theme_color); ImGui::Spacing();
                ImGui::Text("Add custom fonts:");
                ImGui::InputText("Font (.ttf) path", const_cast<char*>(fontdir.c_str()), fontdir.capacity()+1, flags, ResizeCallback, (void*)&fontdir); ImGui::Spacing();
                ImGui::InputInt("Font size", (int*)&fontsize);
                if (ImGui::Button("Save")) {
                    std::cout << "[INFO] Saving settings..." << std::endl;
                    Logger::GetInstance().log("[INFO] Saving settings...");
                    ImVec4ToFloats({r,g,b,0});
                    if (!imgbg) {
                        try {
                            cfg.set<int>("ui_bgc_red", r*255);
                            cfg.set<int>("ui_bgc_green", g*255);
                            cfg.set<int>("ui_bgc_blue", b*255);
                            std::cout << "[INFO] Background color updated successfully!" << std::endl;
                            Logger::GetInstance().log("[INFO] Background color updated successfully!\n");
                        } 
                        catch (...) {   
                            std::cout << "[ERROR] Unable to update background color configuration! Skipping..." << std::endl; 
                            Logger::GetInstance().log("[ERROR] Unable to update background color configuration! Skipping...\n");
                        }

                    } 
                    else { 
                        std::cout << "[INFO] Skipping background color saving due to background image being active." << std::endl; 
                        Logger::GetInstance().log("[INFO] Skipping background color saving due to background image being active.\n"); 
                    }
                    ImVec4ToFloats({tcr,tcg,tcb,0});
                    try {
                        cfg.set<int>("ui_theme_red", tcr*255); // TODO: fix values not updating
                        cfg.set<int>("ui_theme_green", tcg*255);
                        cfg.set<int>("ui_theme_blue", tcb*255);
                        std::cout << "[INFO] Theme color updated successfully!" << std::endl;
                        Logger::GetInstance().log("[INFO] Theme color updated successfully!\n");

                    } 
                    catch (...) {   
                        std::cout << "[ERROR] Unable to update theme color configuration! Skipping..." << std::endl; 
                        Logger::GetInstance().log("[ERROR] Unable to update theme color configuration! Skipping...\n");
                    }
                    if (cfg.SaveToFile(config_path)) {
                        std::cout << "[INFO] Successfully applied all settings!" << std::endl;
                        Logger::GetInstance().log("[INFO] Successfully applied all settings!\n");
                    } 
                    else { 
                        std::cout << "[ERROR] Unable to save settings: an unexpected exception occured! - Is the file in use of another proces?" << std::endl; 
                        Logger::GetInstance().log("[ERROR] Unable to save settings: an unexpected exception occured! - Is the file in use of another proces?\n");
                    }
                }
                ImGui::Dummy(ImVec2(0.0f,10.0f));
                ImGui::EndChild();

            break;

            case DEBUG_TAB:
                // ImGui::SetNextWindowBgAlpha(0.0f);
                ImGui::SetNextWindowPos(ImVec2(fb_width/2.0f+20.0f, 63.0f));
                ImGui::SetNextWindowSize(ImVec2(ImGui::GetWindowWidth(), ImGui::GetWindowHeight()));
                ImGui::ShowDebugLogWindow();
            break;

            default:
                c_tab=OVERVIEW_TAB;
            break;
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("Sidebar", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
        ImGui::BeginGroup();
        ImGui::SetCursorPosX(9.0f);
        ImGui::SetCursorPosY(10.0f);
        ImGui::PushFont(f_iconData);  
        if (open&&sidebar_size<=-70) {
            ImGui::PopFont(); 
            if (ImGui::Button("Sidebar", ImVec2(std::abs(sidebar_size)/1.2f,34.0f))) open=!open; 
            ImGui::SameLine(); ImGui::SetCursorPosX(20.0f); ImGui::SetCursorPosY(12.0f);
            ImGui::PushFont(f_iconData); 
            ImGui::Text("");

        }
        else {
            if (ImGui::Button("", ImVec2(std::abs(sidebar_size)/1.72f, 34.0f))) {
                open=!open;
            }
        }
        if (open&&sidebar_size>=-148) sidebar_size-=4;      // stretch
        else if (!open&&sidebar_size<=-68) sidebar_size+=4; // shrink
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::Separator();
        ImGui::BeginGroup();

        ImGui::SetCursorPosX(9.0f);
        ImGui::SetCursorPosY(60.0f);
        ImGui::PushFont(f_iconData);  
        if (open&&sidebar_size<=-70) { 
            ImGui::PopFont(); 
            if (ImGui::Button("Input", ImVec2(std::abs(sidebar_size)/1.2f,34.0f))) c_tab=OVERVIEW_TAB; 
            ImGui::SameLine(); ImGui::SetCursorPosX(20.0f); ImGui::SetCursorPosY(62.0f);
            ImGui::PushFont(f_iconData); 
            ImGui::Text("#");
        }
        else {
            if (ImGui::Button("#", ImVec2(std::abs(sidebar_size)/1.72f, 34.0f))) c_tab=OVERVIEW_TAB;
        }
        ImGui::PopFont();

        ImGui::SetCursorPosX(9.0f);
        ImGui::SetCursorPosY(100.0f);
        ImGui::PushFont(f_iconData);  
        if (open&&sidebar_size<=-70) { 
            ImGui::PopFont(); 
            if (ImGui::Button("Manual Input", ImVec2(std::abs(sidebar_size)/1.2f,34.0f))) c_tab=MANUAL_INPUT_TAB; 
            ImGui::SameLine(); ImGui::SetCursorPosX(20.0f); ImGui::SetCursorPosY(102.0f);
            ImGui::PushFont(f_iconData); 
            ImGui::Text("");
        }
        else {
            if (ImGui::Button("", ImVec2(std::abs(sidebar_size)/1.72f, 34.0f))) c_tab=MANUAL_INPUT_TAB;
        }
        ImGui::PopFont();
        
        ImGui::SetCursorPosX(9.0f);
        ImGui::SetCursorPosY(140.0f);
        ImGui::PushFont(f_iconData);  
        if (open&&sidebar_size<=-70) { 
            ImGui::PopFont(); 
            if (ImGui::Button("Logs", ImVec2(std::abs(sidebar_size)/1.2f,34.0f))) c_tab=LOG_TAB; 
            ImGui::SameLine(); ImGui::SetCursorPosX(20.0f); ImGui::SetCursorPosY(142.0f);
            ImGui::PushFont(f_iconData); 
            ImGui::Text("(");
        }
        else {
            if (ImGui::Button("(", ImVec2(std::abs(sidebar_size)/1.72f, 34.0f))) c_tab=LOG_TAB;
        }
        ImGui::PopFont();
        
        ImGui::SetCursorPosX(9.0f);
        ImGui::SetCursorPosY(180.0f);
        ImGui::PushFont(f_iconData);  
        if (open&&sidebar_size<=-70) { 
            ImGui::PopFont(); 
            if (ImGui::Button("Settings", ImVec2(std::abs(sidebar_size)/1.2f,34.0f))) c_tab=SETTINGS_TAB; 
            ImGui::SameLine(); ImGui::SetCursorPosX(20.0f); ImGui::SetCursorPosY(182.0f);
            ImGui::PushFont(f_iconData); 
            ImGui::Text("~");
        }
        else {
            if (ImGui::Button("~", ImVec2(std::abs(sidebar_size)/1.72f, 34.0f))) c_tab=SETTINGS_TAB;
        }
        ImGui::PopFont();
        
        ImGui::SetCursorPosX(9.0f);
        ImGui::SetCursorPosY(220.0f);
        ImGui::PushFont(f_iconData);  
        if (open&&sidebar_size<=-70) { 
            ImGui::PopFont(); 
            if (ImGui::Button("ImGui Debug", ImVec2(std::abs(sidebar_size)/1.2f,34.0f))) c_tab=DEBUG_TAB; 
            ImGui::SameLine(); ImGui::SetCursorPosX(20.0f); ImGui::SetCursorPosY(222.0f);
            ImGui::PushFont(f_iconData); 
            ImGui::Text("");
        }
        else {
            if (ImGui::Button("", ImVec2(std::abs(sidebar_size)/1.72f, 34.0f))) c_tab=DEBUG_TAB;
        }
        ImGui::PopFont();
        
        ImGui::SetCursorPosX(9.0f);
        ImGui::SetCursorPosY(260.0f);
        ImGui::PushFont(f_iconData);  
        if (open&&sidebar_size<=-70) { 
            ImGui::PopFont();
            if (stats) {
                if (ImGui::Button("Hide FPS", ImVec2(std::abs(sidebar_size)/1.2f,34.0f))) stats=false; 
            } else {
                if (ImGui::Button("Show FPS", ImVec2(std::abs(sidebar_size)/1.2f,34.0f))) stats=true; 
            }
            ImGui::SameLine(); ImGui::SetCursorPosX(20.0f); ImGui::SetCursorPosY(262.0f);
            ImGui::PushFont(f_iconData); 
            ImGui::Text("");
        }
        else {
            if (ImGui::Button("", ImVec2(std::abs(sidebar_size)/1.72f, 34.0f))) stats=!stats;
        }
        ImGui::PopFont();

        ImGui::EndGroup();
        ImGui::EndChild();

        ImGui::End();

    if (imgbg) {
        // remove padding
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        {
            ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(fb_width,fb_height), ImGuiCond_Always);
        
            ImGui::Begin("##background", nullptr,
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoBackground);
            
            
            ImGui::Image(
                (ImTextureID)texture.DS, 
                ImVec2(fb_width, fb_height), 
            
                ImVec2(ImClamp(((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f) - fb_width) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f))), 0.0f, 1.0f), ImClamp(((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f) - fb_height) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f))), 0.0f, 1.0f)), 
                ImVec2(ImClamp(1.0f - ((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f) - fb_width) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f))), 0.0f, 1.0f), ImClamp(1.0f - ((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f) - fb_height) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f))), 0.0f, 1.0f)));
            
            
            
            ImGui::End();
        }
        ImGui::PopStyleVar();
    }


    if (render_prompt()) {

        ImGui::SetNextWindowPos(ImVec2(fb_width/2.0-fb_width/8.0,fb_height/2.0-fb_height/8.0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(fb_width/4.0,fb_height/4.0), ImGuiCond_Always);
        ImGui::Begin("prompt", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar);

        ImGui::SetCursorPosY(13.5f);
        ImGui::SetCursorPosX(ImGui::GetWindowWidth()/2-53);
        ImGui::Text("ACTION REQUIRED");
        ImGui::Separator();
        ImGui::TextUnformatted(active_prompt_->context.c_str(), NULL);
        // if (ImGui::Button("Terminate", ImVec2(100,28))) {
        //     active_prompt_->promise.set_value(false);
        //     active_prompt_.reset();
        //     ImGui::CloseCurrentPopup();
        // }
        // ImGui::SameLine();
        // ImGui::SetCursorPosY(ImGui::GetWindowHeight()-48);
        // if (ImGui::Button("Accept", ImVec2(100,28))) {
        //     // clear active prompt and perform callback
        //     active_prompt_->promise.set_value(true);
        //     active_prompt_.reset();
        //     ImGui::CloseCurrentPopup();
        // }
        if (active_prompt_->type == PromptType::boolean) { // present true/false return to promise
            ImGui::SetCursorPosX(ImGui::GetWindowWidth()/2-106);
            ImGui::SetCursorPosY(ImGui::GetWindowHeight()-48);
            if (ImGui::Button("Terminate", ImVec2(100, 28))) {
                active_prompt_->promise_bool.set_value(false);
                active_prompt_.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Accept", ImVec2(100, 28))) {
                active_prompt_->promise_bool.set_value(true);
                active_prompt_.reset();
                ImGui::CloseCurrentPopup();
            }
        } else { // present context return to promise (requires further processing)
            ImGui::InputText("##prompt_input", active_prompt_->input_buf.data(), active_prompt_->input_buf.size());

            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 48);
            if (ImGui::Button("Cancel", ImVec2(100, 28))) {
                active_prompt_->promise_str.set_value(std::string());
                active_prompt_.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Accept", ImVec2(100, 28))) {
                std::string result = active_prompt_->input_buf.data();
                active_prompt_->promise_str.set_value(std::move(result));
                active_prompt_.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::End();
    }
    if (stats) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f,0.0f,0.0f,0.0f));
        {
            ImGui::SetNextWindowPos(ImVec2(2, fb_height-16), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(fb_width,32), ImGuiCond_Always);
        
            ImGui::Begin("##statistics", nullptr,
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoBackground);
            ImGui::GetForegroundDrawList();
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
            ImGui::End();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

        // rendering
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized)
        {
            wd->ClearValue.color.float32[0] = backgorund_color.x;
            wd->ClearValue.color.float32[1] = backgorund_color.y;
            wd->ClearValue.color.float32[2] = backgorund_color.z;
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }
    }

    // cleanup
    if (!Shutdown(surface)) {
        std::cout << "[ERROR] (Vulkan/SDL2) Unable to shutdown properly!" << std::endl;
        Logger::GetInstance().log("[ERROR] (Vulkan/SDL2) Unable to shutdown properly!\n");
    }

    return 0;
}

int Interface::Initialize() {
    std::cout << "\n>────────────────[INITIALIZING GRAPHICAL USER INTERFACE]────────────────<\n" << std::endl;
    Logger::GetInstance().log("\n>----------------[INITIALIZING GRAPHICAL USER INTERFACE]----------------<\n\n");
    // setup SDL
#ifdef _WIN32
    ::SetProcessDPIAware();
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        printf("[ERROR] (Vulkan/SDL2) %s\n", SDL_GetError());
        Logger::GetInstance().log("[ERROR] (Vulkan/SDL2) SDL Initialization threw:\n[ERROR] ");
        Logger::GetInstance().log(SDL_GetError());
        Logger::GetInstance().log("\n");
        return -1;
    }
    std::cout << "[INFO] (Vulkan/SDL2) SDL dependencies loaded successfully!" << std::endl;
    Logger::GetInstance().log("[INFO] (Vulkan/SDL2) SDL dependencies loaded successfully!\n");

// from 2.0.18: Enable native IME
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif


    std::cout << "\n>───────────[INITIALIZED GRAPHICAL USER INTERFACE SUCCESSULLY]──────────<\n" << std::endl;
    Logger::GetInstance().log("\n>-----------[INITIALIZED GRAPHICAL USER INTERFACE SUCCESSULLY]----------<\n\n");
    return 0;
}

