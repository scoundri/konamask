#pragma once
#include "Settings.h"
#include <fstream>
#include <iostream> // for console reporting
#include <mutex>
#define LOGGER_ENABLED get<bool>("enable_logging_to_file", false)
class Logger {
public:

    static Logger& GetInstance() {
        static Logger instance;
        return instance;
    }

#ifdef LOGGER_ENABLED
    void log(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_logFile << message;
    }
#else
    void log(const std::string& message) {
        std::cout << message;
    }
#endif

private:
    Settings& cfg = Settings::GetInstance();

    Logger() : m_logFile(cfg.logpath, std::ios::out | std::ios::app) {
        if (!m_logFile.is_open()) {
            std::cerr << "[ERROR] (Logger) Failed to open log file." << std::endl;
        }
    }

    ~Logger() {
        if (m_logFile.is_open()) {
            m_logFile.close();
        }
    }

    std::ofstream m_logFile;
    std::mutex m_mutex;
};
