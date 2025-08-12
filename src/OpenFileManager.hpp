#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shellapi.h>
#else
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <cstring>
#endif

namespace util {

inline bool is_executable_in_path(const std::string &name) {
    // check PATH for an executable named `name` (simple which)
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return false;
    std::string path(pathEnv);
    size_t start = 0;
    while (start < path.size()) {
        size_t pos = path.find(':', start);
        std::string dir = path.substr(start, pos - start);
        std::filesystem::path p = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(p)) {
            // check executable bit
#ifndef _WIN32
            if (access(p.c_str(), X_OK) == 0) return true;
#else
            return true;
#endif
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return false;
}

#ifdef _WIN32

inline std::wstring to_wstring_utf16(const std::string &s) {
    // convert UTF-8 std::string to UTF-16 wstring
    if (s.empty()) return {};
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w;
    w.resize(size_needed);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], size_needed);
    return w;
}

inline bool open_in_file_manager(const std::filesystem::path &p, bool select = false) {
    // open path using explorer (if select==true and path is a file > highlight file)
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec) return false;

    std::wstring wpath = to_wstring_utf16(abs.u8string());
    if (select && std::filesystem::is_regular_file(abs)) {
        // explorer /select,"fullpath"
        std::wstring params = L"/select,\"" + wpath + L"\"";
        HINSTANCE res = ShellExecuteW(NULL, L"open", L"explorer.exe", params.c_str(), NULL, SW_SHOWDEFAULT);
        return (INT_PTR)res > 32;
    } else {
        // just open the folder
        std::wstring folder = to_wstring_utf16(abs.u8string());
        // if it's a file, open parent folder
        if (std::filesystem::is_regular_file(abs)) folder = to_wstring_utf16(abs.parent_path().u8string());
        HINSTANCE res = ShellExecuteW(NULL, L"open", folder.c_str(), NULL, NULL, SW_SHOWDEFAULT);
        return (INT_PTR)res > 32;
    }
}

#else

static inline void safe_exec_no_wait(const std::string &cmd, const std::vector<std::string> &args) {
    // fork+exec a command and detach so caller doesn't block. child does execvp.
    pid_t pid = fork();
    if (pid == -1) return;
    if (pid == 0) {
        // child
        // build argv
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(cmd.c_str()));
        for (const auto &a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        // redirect stdio to /dev/null so it doesn't hold terminal
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execvp(cmd.c_str(), argv.data());
        // if execvp fails:
        _exit(127);
    }
    int status;
    waitpid(pid, &status, WNOHANG);
}

inline bool open_in_file_manager(const std::filesystem::path &p, bool select = false) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec) return false;

#if defined(__APPLE__)
    // thank you Stack overflow
    std::string arg = abs.u8string();
    if (select && std::filesystem::is_regular_file(abs)) {
        safe_exec_no_wait("open", {"-R", arg});
    } else {
        if (std::filesystem::is_regular_file(abs)) arg = abs.parent_path().u8string();
        safe_exec_no_wait("open", {arg});
    }
    return true;
#else
    // prefer a filemanager that supports --select (when select requested)
    std::string arg = abs.u8string();

    if (select && std::filesystem::is_regular_file(abs)) {
        // list of common file managers and their select argument
        struct FM { const char* name; const char* select_arg; };
        const FM fams[] = {
            {"nautilus", "--select"},
            {"dolphin", "--select"},
            {"nemo", "--select"},
            {"thunar", "--select"},
            {"caja", "--select"},
            {"pcmanfm", "--select"}
        };
        for (const auto &fm : fams) {
            if (is_executable_in_path(fm.name)) {
                safe_exec_no_wait(fm.name, {fm.select_arg, arg});
                return true;
            }
        }
        // if none support select, fall back to opening the parent folder
        arg = abs.parent_path().u8string();
    } else {
        // if path is file and not selecting, open parent folder
        if (std::filesystem::is_regular_file(abs)) arg = abs.parent_path().u8string();
    }

    // try gio (glib) if available (gio open <uri>)
    if (is_executable_in_path("gio")) {
        // gio open accepts file:// URIs; (normally works with plain paths)
        safe_exec_no_wait("gio", {"open", arg});
        return true;
    }
    // fallback to xdg-open (widely available)
    if (is_executable_in_path("xdg-open")) {
        safe_exec_no_wait("xdg-open", {arg});
        return true;
    }
    // final fallback (try many file managers without select)
    const char* fallback_fms[] = {"nautilus", "dolphin", "nemo", "thunar", "caja", "pcmanfm"};
    for (auto name : fallback_fms) {
        if (is_executable_in_path(name)) {
            safe_exec_no_wait(name, {arg});
            return true;
        }
    }

    // nothing to call
    return false;
#endif
}

#endif

}