#include "Settings.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sys/stat.h> // for CheckFile
#include <filesystem> // for logpath file creation

static bool CheckFile(const char* path) {
    struct stat info;
    if (stat(path, &info) != 0) {
        std::cerr << "[ERROR] File check failed!" << "\n[INFO] Path \"" << path << "\" does not exist." << std::endl;
        return false;
    }

    if (info.st_mode & S_IFREG) {
        return true; // path exists & is a file
    } else {
        std::cerr << "[ERROR] Path \"" << path << "\" is not a file." << std::endl;
        return false;
    }
}


int Settings::Initialize() {    
    std::cout << "\n>────────────────────────[LOADING CONFIGURATION]────────────────────────<\n" << std::endl;

    char path[PATH_MAX];
    char backup[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", getenv("HOME"), ".config/konacode/konamask/config.ini");
    snprintf(backup, sizeof(backup), "%s/%s", getenv("HOME"), ".config/konacode/konamask/config_backup.ini");

    if (!LoadFromFile(path)) {
        std::cerr << "[ERROR] Failed to load config.ini, trying to load backup..." << std::endl;
        if (!LoadFromFile(backup)) {
            std::cerr << "[ERROR] Failed to load config.ini, trying to load backup..." << std::endl;
            std::cout << "\n>──────────────────[UNSUCCESSULLY LOADED CONFIGURATION]─────────────────<\n" << std::endl;
        }
        return 1;
    }
    std::cout << "[INFO] Successfully loaded config.ini! Backing up..." << std::endl;
    if (!CheckFile(backup)) {
        if (Settings::CopyFile(path, backup)) {
        std::cout << "[INFO] Successfully backed-up config.ini!" << std::endl;
        }
        else {
            std::cout << "[ERROR] Unable to backup config.ini!" << std::endl;
        }
    }

    if (get<bool>("enable_logging_to_file", false)) {
        snprintf(logpath, sizeof(logpath), "%s/%s", getenv("HOME"), ".config/konacode/konamask/latest.log");

        if (!CheckFile(logpath)) {
            // build path and ensure parent directory exists
            std::filesystem::path p(logpath);
            std::error_code ec;
            p = p.parent_path();
            if (!p.empty() && !std::filesystem::exists(p, ec)) {
                if (!std::filesystem::create_directories(p, ec)) {
                    std::cerr << "[ERROR] Failed to create directory: '" << p.string()
                              << "': " << ec.message() << '\n';
                }
            } else if (ec) {
                std::cerr << "[ERROR] Unable to check parent directory!\n[ERROR] " << p.string()
                          << "': " << ec.message() << '\n';
            }
#if defined(_WIN32)
            std::wstring wpath = p.wstring();
            HANDLE h = CreateFileW(
                wpath.c_str(),
                GENERIC_WRITE,
                0,                // no sharing
                nullptr,
                CREATE_NEW,       // fail if already exists
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (h == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
                } else {
                    std::cerr << "[ERROR] CreateFileW failed: " << err << '\n';
                }
            } else {
                CloseHandle(h);
            }
#endif
        }
    }
    std::cout << "\n>───────────────────[SUCCESSULLY LOADED CONFIGURATION]──────────────────<\n" << std::endl;
    return 0;
}


bool Settings::CopyFile(const std::string& src, const std::string& dest) {
    std::ifstream sourceFile(src, std::ios::binary);
    if (!sourceFile.is_open()) {
        std::cerr << "[ERROR] Could not open source file (" << dest << ")." << std::endl;
        return false;
    }

    std::ofstream destinationFile(dest, std::ios::binary);
    if (!destinationFile.is_open()) {
        std::cerr << "[ERROR] Could not open destination file (" << dest << ")." << std::endl;
        sourceFile.close();
        return false;
    }

    destinationFile << sourceFile.rdbuf();

    sourceFile.close();
    destinationFile.close();

    return true;
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

bool Settings::SaveToFile(const std::string& filename) const { // TODO: make comments persistent
    std::ofstream file(filename);
    if (!file.is_open()) return false;
    for (const auto& [key, value] : data_) {
        file << key << " = " << value << "\n";
        std::cout << "[INFO] Wrote value \"" << value << "\" @ \"" << key << "\" successfully!" << std::endl;
    }
    return true;
}

static void trim(std::string& s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){ return !std::isspace(ch); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end());
}

template<typename T>
T Settings::get(const std::string& key, const T& defaultValue) const {
    auto it = data_.find(key);
    if (it == data_.end()) return defaultValue;

    std::istringstream iss(it->second);
    T value;
    if constexpr (std::is_same_v<T, bool>) {
        // robust bool parsing
        std::string tmp = it->second;
        trim(tmp);
        size_t pos = tmp.find(';');
        if (pos == std::string::npos) pos = tmp.find('#');
        if (pos != std::string::npos) tmp = tmp.substr(0, pos);
        trim(tmp);
        std::transform(tmp.begin(), tmp.end(), tmp.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

        if (tmp == "1" || tmp == "true" || tmp == "yes" || tmp == "on") return true;
        if (tmp == "0" || tmp == "false" || tmp == "no" || tmp == "off") return false;
        // fallback to numeric parse
        try {
            int n = std::stoi(tmp);
            return n != 0;
        } catch (...) {
            return defaultValue;
        }
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