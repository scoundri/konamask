#pragma once
#include "Settings.h"
#include <string>
#include <iostream>
#include <mutex>
#include <atomic>
#include <system_error>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/stat.h>
  #include <cerrno>
  #include <cstring>
#endif

class Logger {
public:

    static Logger& GetInstance() {
        static Logger instance;
        return instance;
    }

    static void Initialize() {
        auto& inst = GetInstance();
        std::lock_guard<std::mutex> lk(inst.m_initMutex);
        inst.m_logPath = inst.cfg.logpath;
        inst.m_enabled = inst.cfg.get<bool>("enable_logging_to_file", false);
        if (inst.m_enabled && inst.m_logPath.empty()) {
            std::cerr << "[ERROR] (Logger) Logging enabled but logpath is empty; disabling file logging." << std::endl;
            inst.m_enabled = false;
        }
        inst.m_initialized.store(true, std::memory_order_release);
    }

    void log(const std::string& message) {
        if (!m_initialized.load(std::memory_order_acquire)) {
            Initialize();
        }

        if (!m_enabled) {
            // thread-safe stdout fallback
            std::lock_guard<std::mutex> lk(m_writeMutex);
            std::cout << message;
            return;
        }

        std::lock_guard<std::mutex> lk(m_writeMutex);
        writeAppendShared(m_logPath, message);
    }

    // toggling at runtime
    void SetEnabled(bool enabled) {
        std::lock_guard<std::mutex> lk(m_initMutex);
        m_enabled = enabled;
    }

    bool IsEnabled() const {
        return m_enabled;
    }

private:
    Settings& cfg = Settings::GetInstance();

    Logger() = default;
    ~Logger() = default;

    // non-copyable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void writeAppendShared(const std::string& path, const std::string& data) {
#ifdef _WIN32 // some code for WIN32 inspired by a stackoverflow issue, but I don't have the URL for it
        HANDLE hFile = CreateFileA(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile == INVALID_HANDLE_VALUE) {
            std::cerr << "[ERROR] (Logger) CreateFile failed: " << GetLastError() << std::endl;
            return;
        }

        // ensure file pointer at end
        SetFilePointer(hFile, 0, nullptr, FILE_END);

        DWORD written = 0;
        BOOL ok = WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        if (!ok || written != data.size()) {
            std::cerr << "[ERROR] (Logger) WriteFile failed: " << GetLastError() << "\n";
        }
        CloseHandle(hFile);
#else
        // posix
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd == -1) {
            std::cerr << "[ERROR] (Logger) Open failed: " << std::strerror(errno) << std::endl;
            return;
        }

        const char* buf = data.data();
        ssize_t remaining = static_cast<ssize_t>(data.size());
        while (remaining > 0) {
            ssize_t written = write(fd, buf, remaining);
            if (written == -1) {
                if (errno == EINTR) continue;
                std::cerr << "[ERROR] (Logger) Write failed: " << std::strerror(errno) << std::endl;
                break;
            }
            remaining -= written;
            buf += written;
        }
        close(fd);
#endif
    }

    std::string m_logPath;
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_initialized{false};

    std::mutex m_initMutex;
    std::mutex m_writeMutex;
};
