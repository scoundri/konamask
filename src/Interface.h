#pragma once
#include "Settings.h"
#include <atomic>

class Interface {
public:
    int Initialize();
    int Render(std::atomic<bool>* runningFlag);
    static void Minimize();

private:
    Settings& cfg = Settings::GetInstance();
};