#include "Tray.h"
#include <cstring>

TrayMenu::TrayMenu(const std::string& icon_name_or_path, const std::string& tooltip)
    : tooltip_(tooltip), icon_(icon_name_or_path)
{

}

TrayMenu::~TrayMenu() {
    Shutdown();
}

void TrayMenu::AddItem(const std::string& text, Callback cb) {
    items_.push_back({text, cb, false});
}

void TrayMenu::AddSeparator() {
    items_.push_back({"", nullptr, true});
}

void TrayMenu::SetTooltip(const std::string& tip) {
    tooltip_ = tip;
#if defined(TRAY_WIN32)
    if (nid_.hIcon || nid_.cbSize) {
        // update tooltip on windows if already shown
        strncpy(nid_.szTip, tip.c_str(), sizeof(nid_.szTip)-1);
        Shell_NotifyIcon(NIM_MODIFY, &nid_);
    }
#endif
}

bool TrayMenu::Show() {
    if (running_.load()) return true;
    running_.store(true);
#if defined(TRAY_WIN32)
    if (!init_windows()) { running_.store(false); return false; }
    loop_thread_ = std::thread([this]{ run_windows(); });
    return true;
#elif defined(TRAY_X11)
    if (!init_x11()) { running_.store(false); return false; }
    loop_thread_ = std::thread([this]{ run_x11(); });
    return true;
#endif
}

void TrayMenu::Shutdown() {
    if (!running_.load()) return;
    running_.store(false);
#if defined(TRAY_WIN32)
    PostMessage(hwnd_, WM_CLOSE, 0, 0);
    if (loop_thread_.joinable()) loop_thread_.join();
    destroy_windows();
#elif defined(TRAY_X11)
    if (disp_) {
        // post a dummy event to wake the loop
        XClientMessageEvent ev;
        memset(&ev,0,sizeof(ev));
        ev.type = ClientMessage;
        ev.window = icon_win_;
        ev.format = 32;
        XSendEvent(disp_, DefaultRootWindow(disp_), False, 0, (XEvent*)&ev);
        XFlush(disp_);
    }
    if (loop_thread_.joinable()) loop_thread_.join();
    destroy_x11();
#endif
}

// windows implementation
#if defined(TRAY_WIN32)

bool TrayMenu::init_windows() {
    // create a hidden window class for tray callbacks
    HINSTANCE hinst = GetModuleHandle(NULL);
    const char* cls = "TrayMenuHiddenWndClass";
    WNDCLASSEXA wcex{};
    wcex.cbSize = sizeof(wcex);
    wcex.lpfnWndProc = TrayMenu::WndProc;
    wcex.hInstance = hinst;
    wcex.lpszClassName = cls;
    RegisterClassExA(&wcex);

    hwnd_ = CreateWindowExA(0, cls, "tray_hidden", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
                            NULL, NULL, hinst, this);
    if (!hwnd_) return false;

    // register a custom window message for tray callback
    wm_tray_callback_ = RegisterWindowMessageA("TrayIconCallbackMessage");
    if (!wm_tray_callback_) wm_tray_callback_ = WM_USER + 1;

    // prepare NOTIFYICONDATA
    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = wm_tray_callback_;
    strncpy(nid_.szTip, tooltip_.c_str(), sizeof(nid_.szTip)-1);

    // load icon
    HICON hIcon = nullptr;
    if (!icon_.empty()) {
        // try load as file
        hIcon = (HICON)LoadImageA(NULL, icon_.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
        if (!hIcon) {
            // try small stock icon
            hIcon = LoadIcon(NULL, IDI_APPLICATION);
        }
    } else {
        hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    nid_.hIcon = hIcon;

    if (!Shell_NotifyIcon(NIM_ADD, &nid_)) return false;
    return true;
}

void TrayMenu::destroy_windows() {
    if (nid_.hIcon) DestroyIcon(nid_.hIcon);
    Shell_NotifyIcon(NIM_DELETE, &nid_);
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK TrayMenu::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    TrayMenu* self = nullptr;
    if (message == WM_CREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        self = (TrayMenu*)cs->lpCreateParams;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (TrayMenu*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    }

    if (self && message == self->wm_tray_callback_) {
        // tray icon messages
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            // show popup menu
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            // populate menu
            self->id_to_cb_.clear();
            int id = self->next_id_;
            for (auto &it : self->items_) {
                if (it.separator) {
                    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
                } else {
                    AppendMenuA(menu, MF_STRING, id, it.text.c_str());
                    self->id_to_cb_[id] = it.cb;
                    ++id;
                }
            }
            // set window to foreground to make TrackPopupMenu work correctly
            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);
            if (cmd != 0) {
                auto found = self->id_to_cb_.find(cmd);
                if (found != self->id_to_cb_.end() && found->second) {
                    // execute callback on separate thread
                    std::thread cbthread(found->second);
                    cbthread.detach();
                }
            }
            DestroyMenu(menu);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            // handle double click
            if (!self->items_.empty() && self->items_[0].cb) {
                std::thread t(self->items_[0].cb);
                t.detach();
            }
        }
    }

    switch (message) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void TrayMenu::run_windows() {
    // associate pointer to window
    SetWindowLongPtr(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    // standard message loop
    MSG msg;
    while (running_.load() && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

#endif
#if defined(TRAY_X11)

bool TrayMenu::init_x11() {
    disp_ = XOpenDisplay(NULL);
    if (!disp_) return false;
    _NET_SYSTEM_TRAY_S0 = XInternAtom(disp_, "_NET_SYSTEM_TRAY_S0", False);
    _NET_SYSTEM_TRAY_OPCODE = XInternAtom(disp_, "_NET_SYSTEM_TRAY_OPCODE", False);
    WM_PROTOCOLS = XInternAtom(disp_, "WM_PROTOCOLS", False);
    WM_DELETE_WINDOW = XInternAtom(disp_, "WM_DELETE_WINDOW", False);

    // simple override-redirect window
    int screen = DefaultScreen(disp_);
    Window root = RootWindow(disp_, screen);
    icon_win_ = XCreateSimpleWindow(disp_, root, 0, 0, 24, 24, 0, 0, 0);

    // select events
    XSelectInput(disp_, icon_win_, ExposureMask | ButtonPressMask);

    // request docking
    Window manager = find_system_tray_owner();
    if (!manager) {
        // try with screen index 0
        // if not found, still map the window
    } else {
        if (!dock_to_tray(manager)) {
            // continue, but docking may fail
        }
    }

    XMapWindow(disp_, icon_win_);
    XFlush(disp_);
    return true;
}

Window TrayMenu::find_system_tray_owner() {
    Atom sel = _NET_SYSTEM_TRAY_S0;
    if (!sel) return 0;
    Window owner = XGetSelectionOwner(disp_, sel);
    return owner;
}

bool TrayMenu::dock_to_tray(Window manager) {
    if (!manager) return false;
    XClientMessageEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.window = manager;
    ev.message_type = _NET_SYSTEM_TRAY_OPCODE;
    ev.format = 32;
    ev.data.l[0] = CurrentTime;
    ev.data.l[1] = 0; // SYSTEM_TRAY_REQUEST_DOCK = 0
    ev.data.l[2] = (long)icon_win_;
    ev.data.l[3] = 0;
    ev.data.l[4] = 0;
    XSendEvent(disp_, manager, False, 0, (XEvent*)&ev);
    XFlush(disp_);
    return true;
}

void TrayMenu::destroy_x11() {
    if (!disp_) return;
    if (icon_win_) {
        XDestroyWindow(disp_, icon_win_);
        icon_win_ = 0;
    }
    XCloseDisplay(disp_);
    disp_ = nullptr;
}

void TrayMenu::run_x11() {
    while (running_.load()) {
        while (XPending(disp_)) {
            XEvent ev;
            XNextEvent(disp_, &ev);
            if (ev.type == Expose) {
                // draw a simple placeholder icon
                GC gc = XCreateGC(disp_, icon_win_, 0, NULL);
                XFillRectangle(disp_, icon_win_, gc, 0, 0, 24, 24);
                XFreeGC(disp_, gc);
            } else if (ev.type == ButtonPress) {
                XButtonEvent bev = ev.xbutton;
                if (bev.button == Button3) { // right click
                    // compute menu position
                    int mx = bev.x_root;
                    int my = bev.y_root;
                    int item_h = 20;
                    int menu_w = 200;
                    int menu_h = (int)items_.size() * item_h;
                    Window menu = XCreateSimpleWindow(disp_, RootWindow(disp_, DefaultScreen(disp_)),
                                                      mx, my, menu_w, menu_h, 1, 0, 0xffffff);
                    XSelectInput(disp_, menu, ExposureMask | ButtonPressMask);
                    XMapRaised(disp_, menu);
                    XFlush(disp_);

                    bool menu_open = true;
                    while (menu_open) {
                        XEvent mev;
                        XNextEvent(disp_, &mev);
                        if (mev.type == Expose) {
                            GC gc = XCreateGC(disp_, menu, 0, NULL);
                            int i = 0;
                            for (auto &it : items_) {
                                if (it.separator) {
                                    // draw separator
                                    XDrawLine(disp_, menu, gc, 4, i*item_h + item_h/2, menu_w-4, i*item_h + item_h/2);
                                } else {
                                    XDrawString(disp_, menu, gc, 8, i*item_h + 14, it.text.c_str(), it.text.size());
                                }
                                ++i;
                            }
                            XFreeGC(disp_, gc);
                        } else if (mev.type == ButtonPress) {
                            XButtonEvent b = mev.xbutton;
                            if (b.button == Button1) {
                                int idx = b.y / item_h;
                                if (idx >= 0 && idx < (int)items_.size()) {
                                    auto &it = items_[idx];
                                    if (!it.separator && it.cb) {
                                        // run callback in separate thread to avoid blocking X loop
                                        std::thread t(it.cb);
                                        t.detach();
                                    }
                                }
                                menu_open = false;
                            } else if (b.button == Button3) {
                                menu_open = false;
                            }
                        }
                    } // menu loop
                    XDestroyWindow(disp_, menu);
                    XFlush(disp_);
                } else if (bev.button == Button1) {
                    // left click: run first item if exists
                    if (!items_.empty() && items_[0].cb) {
                        std::thread t(items_[0].cb);
                        t.detach();
                    }
                }
            } // events
        } // pending
        // small sleep to avoid busy loop
        usleep(10000);
    } // running
}

#endif // TRAY_X11

