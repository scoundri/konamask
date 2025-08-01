#include "Settings.h"
#include "SimpleIni.h"
#include <iostream>

int Settings::Initialize() {
    std::cout << "\n>─────────────────────[LOADING CONFIGURATION]─────────────────────<\n" << std::endl;
	ini.SetUnicode();
    
    SI_Error err = ini.LoadFile("./config.ini");
    if (err < 0) {
        std::cout << "[ERROR] Unable to load \"config.ini\", using default settings!" << std::endl;
    } 
    else {
        std::cout << "[INFO] Setting values..." << std::endl;
        if (ini.GetDoubleValue("audio", "speech_rate")) {
            SPEECH_RATE = ini.GetDoubleValue("audio", "speech_rate");
            std::cout << "[INFO] Speech rate has been set to \"" << SPEECH_RATE << "\"." << std::endl;
        }
        else { std::cout << "[INFO] Speech rate has been set to default (" << SPEECH_RATE << ") due to invalid configuration inside config.ini!" << std::endl; }
        if (ini.GetDoubleValue("audio", "speech_pitch")) {
        SPEECH_PITCH = ini.GetDoubleValue("audio", "speech_pitch");
        std::cout << "[INFO] Speech pitch has been set to \"" << SPEECH_PITCH << "\"." << std::endl;
        } 
        SPEECH_VOLUME = ini.GetDoubleValue("audio", "speech_volume");
        std::cout << "[INFO] Speech volume has been set to \"" << SPEECH_VOLUME << "\"." << std::endl;
        std::cout << "[INFO] Values set!" << std::endl;
    }

    std::cout << "\n>────────────────[SUCCESSULLY LOADED CONFIGURATION]───────────────<\n" << std::endl;
    return 0;
}