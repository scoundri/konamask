#pragma once
#include "Settings.h"

class Interface {
public:
    int Initialize();
    int Render();
private:
    Settings& cfg = Settings::GetInstance();

};