#include "Settings.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iostream>

int Settings::Initialize() {    
    std::cout << "\n>────────────────────────[LOADING CONFIGURATION]────────────────────────<\n" << std::endl;

    if (!LoadFromFile("/home/kona/Projects/Software/konamask/konamask/config.ini")) {
        std::cerr << "[ERROR] Failed to load config.ini, using defaults." << std::endl;
        return 1;
    }
    std::cout << "[INFO] Setting values..." << std::endl;
    SPEECH_RATE = get<int>("speech_rate", 150);
    std::cout << "[INFO] Speech rate has been set to \"" << SPEECH_RATE << "\"." << std::endl;
    SPEECH_PITCH = get<int>("speech_pitch", 50);
    std::cout << "[INFO] Speech pitch has been set to \"" << SPEECH_PITCH << "\"." << std::endl;
    SPEECH_VOLUME = get<int>("speech_volume",100);
    std::cout << "[INFO] Speech volume has been set to \"" << SPEECH_VOLUME << "\"." << std::endl;
    SPEECH_VOICEBANK = get<std::string>("speech_vociebank", "en-us");
    std::cout << "[INFO] Speech volume has been set to \"" << SPEECH_VOICEBANK << "\"." << std::endl;

    VOSK_MODEL_PATH = get<std::string>("voskapi_model_path", "./model");
    std::cout << "[INFO] Vosk API path has been set to  \"" << VOSK_MODEL_PATH << "\"." << std::endl;
    BUFFER_FACTOR = get<double>("voskapi_model_path", 0.05);
    std::cout << "[INFO] Buffer factor has been set to  \"" << BUFFER_FACTOR << "\"." << std::endl;

    PA_SAMPLE_SPEC_RATE = get<int>("pa_sample_spec_rate", 22050);
    std::cout << "[INFO] PulseAudio sample rate has been set to  \"" << PA_SAMPLE_SPEC_RATE << "\"." << std::endl;

    UI_ENABLED = get<int>("enable_user_interface", 22050);
    std::cout << "[INFO] UI has been set to \"" << UI_ENABLED << "\"(1 = enabled, 0 = disabled)." << std::endl;

    std::cout << "[INFO] Values set!" << std::endl;

    std::cout << "\n>───────────────────[SUCCESSULLY LOADED CONFIGURATION]──────────────────<\n" << std::endl;
    return 0;
}

Settings& Settings::GetInstance() {
    static Settings instance;
    return instance;
}

bool Settings::LoadFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cout << "[ERROR] Configuration file not found or cannot be oppened! Creating new file..." << std::endl;
        std::ofstream out(filename, std::ios::out);
        std::cout << "[INFO] Successfully created the configuration file!" << std::endl;

    }
    std::cout << "[INFO] Reading configuration..." << std::endl;

    std::string line;
    while (std::getline(file, line)) {
        // remove whitespace
        auto trim = [](std::string& s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), s.end());
        };

        trim(line);
        // skip comments and empty lines
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        auto delimPos = line.find('=');
        if (delimPos == std::string::npos) continue;

        std::string key = line.substr(0, delimPos);
        std::string value = line.substr(delimPos + 1);
        trim(key);
        trim(value);

        data_[key] = value;
    }
    std::cout << "[INFO] Successfully read the configuration file!" << std::endl;

    return true;
}

template<typename T>
T Settings::get(const std::string& key, const T& defaultValue) const {
    auto it = data_.find(key);
    if (it == data_.end()) return defaultValue;

    std::istringstream iss(it->second);
    T value;
    if constexpr (std::is_same_v<T, bool>) {
        std::string val = it->second;
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        return (val == "1" || val == "true" || val == "yes");
    } else {
        if (!(iss >> value)) return defaultValue;
        return value;
    }
}

template<typename T>
void Settings::set(const std::string& key, const T& value) {
    std::ostringstream oss;
    oss << value;
    data_[key] = oss.str();
}

template int Settings::get<int>(const std::string&, const int&) const;
template double Settings::get<double>(const std::string&, const double&) const;
template bool Settings::get<bool>(const std::string&, const bool&) const;
template std::string Settings::get<std::string>(const std::string&, const std::string&) const;

template void Settings::set<int>(const std::string&, const int&);
template void Settings::set<double>(const std::string&, const double&);
template void Settings::set<bool>(const std::string&, const bool&);
template void Settings::set<std::string>(const std::string&, const std::string&);