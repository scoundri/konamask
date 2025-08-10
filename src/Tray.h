#pragma once

#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <map>
#include <memory>

#if defined(_WIN32) || defined(_WIN64)
  #define TRAY_WIN32
  #include <windows.h>
  #include <shellapi.h>
#else
  #define TRAY_X11
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
  #include <X11/Xatom.h>
  #include <unistd.h>
#endif

// tray menu interface
class TrayMenu {
public:
    using Callback = std::function<void()>;

    TrayMenu(const std::string& icon_name_or_path, const std::string& tooltip = "");
    ~TrayMenu();

    void AddItem(const std::string& text, Callback cb);
    void AddSeparator();
    void SetTooltip(const std::string& tip);
    bool Show();
    void Shutdown();

private:
    struct Item { std::string text; Callback cb; bool separator=false; };
    std::vector<Item> items_;
    std::string tooltip_;
    std::string icon_;
    std::atomic<bool> running_{false};

#if defined(TRAY_WIN32)
    // windows-specific
    HWND hwnd_ = nullptr;
    UINT wm_tray_callback_ = 0;
    NOTIFYICONDATA nid_{};
    std::thread loop_thread_;
    std::map<int, Callback> id_to_cb_;
    int next_id_ = 1000;

    bool init_windows();
    void run_windows(); // message loop runs in background thread
    void destroy_windows();
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
#elif defined(TRAY_X11)
    // X11-specific
    Display* disp_ = nullptr;
    Window icon_win_ = 0;
    Atom _NET_SYSTEM_TRAY_S0 = 0;
    Atom _NET_SYSTEM_TRAY_OPCODE = 0;
    Atom WM_PROTOCOLS = 0;
    Atom WM_DELETE_WINDOW = 0;
    std::thread loop_thread_;

    bool init_x11();
    void run_x11();
    void destroy_x11();
    Window find_system_tray_owner();
    bool dock_to_tray(Window manager);
#endif
};