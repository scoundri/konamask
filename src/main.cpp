#include "SpeechToText.h"
#include "TextToSpeech.h"
#include "Tray.h"
#include "Interface.h"
#include <atomic>
#include <thread>

// object declaration
Settings& cfg = Settings::GetInstance();
SpeechToText stt; 
TextToSpeech tts;
Interface ui;
TrayMenu tray("./img/tray.png", "konamask utility");

std::atomic<bool> uiRunning{true};

static void shutdown() {
    Interface::Minimize();
    tts.Initialize();
    stt.Initialize();
    tts.Shutdown(); // fix konamask (virt input) not destroying
    tray.Shutdown();
}

static void openui() {

}



int main() {

    // developer notice
    std::cout << 
"\n\n    ##%                       #####             \n"
    "   ##%                     ######+++++####%                 __                                    __   \n"
    "   ##           %################+++++##%                  / /______  ____  ____ __________  ____/ /__ \n"
    "   ##       %####################+++#                     / //_/ __ \\/ __ \\/ __ `/ ___/ __ \\/ __  / _ \\\n"
    "    #     ######################++######                 / ,< / /_/ / / / / /_/ / /__/ /_/ / /_/ /  __/\n"
    "     %%  ##################################             /_/|_|\\____/_/ /_/\\__,_/\\___/\\____/\\__,_/\\___/ \n"
    "       %##################+##################   \n"
    "       #################+##+##################                   Software developed by kona         \n"
    "      #####+++++++#######+#+#=#%=%#########  ###         ┌─────────────────────────────────────────┐\n"
    "      ###++++++######==#######=#===+##%####%   #             website: https://konacode.com/         \n"
    "     %##+++##########===#%##+#==#=#==%# %###%                my projects: https://nightvoid.com/    \n"
    "     %###############====#+###+#@%##=% #  ###                github: https://github.com/kona-code/  \n"
    "     #############+#=====+==##%=###==#     ##%  \n"
    "    %############=##=@@%==#=###==#+==##      #               contact me here:                       \n"
    "   #  ###########+%%###%==+==%#==##==###                     Discord: konacode                      \n"
    "      %#########%=+###+#======#======%###                    Email: kona@nightvoid.com              \n"
    "      %##########+=+==+#+===========+#####               └─────────────────────────────────────────┘\n"  
    "      ############+==========#+#==== ######     \n"
    "     ##############+=+==========#=+=  %###%     \n"
    "     #%#####%##%###===#++====####%++    ###     \n"
    "    ##############%==%%+++++++###+=      ##     \n"
    "   ##=======######+==#####++#+%#+#+      %#     \n"
    " #+#####%+===========#####%#===###+       #     \n"
    "########%==+++++#+=++####+##%#+%%+              \n"
    "######%+++++++++#+++%#%####+###%#+              \n"
    "##%#%%+++++####++++%%%#+##########              \n\n";
    
    tray.AddItem("Open Dashboard", [](){ openui(); });
    tray.AddItem("Quit", [&](){ shutdown(); });
    if (!tray.Show()) {
        std::cerr << "[ERROR] Failed to start tray!" << std::endl;
    }
    std::thread trayThread([&](){
        try {
            for (int i = 0; i < 10; ++i) {
            std::cout << "main loop tick " << i << "\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        }
        catch (const std::exception& e) {
            std::cerr << "Tray exception: " << e.what() << std::endl;
            uiRunning = false;
        }
        catch (...) {
            std::cerr << "Unexpected tray exception!" << std::endl;
            uiRunning = false;
        }
    });
    cfg.Initialize();
    if (cfg.get<int>("enable_user_interface", true)) {
    std::cout << "[INFO] UI has been enabled." << std::endl;
        ui.Initialize();
        std::thread uiThread([&](){
            try {
                ui.Render(&uiRunning);
            }
            catch (const std::exception& e) {
                std::cerr << "UI exception: " << e.what() << "\n"
                          << "Falling back to console mode." << std::endl;
                uiRunning = false;
            }
            catch (...) {
                std::cerr << "Unexpected UI exception! Falling back to console mode." << std::endl;
                uiRunning = false;
            }
        });
        tts.Initialize();
        stt.Initialize();
        tts.Shutdown(); // fix konamask (virt input) not destroying
        try {
            if (uiThread.joinable()) {
                uiRunning = false;
                uiThread.join();
            }
        } catch (...) { std::cout << "[ERROR] Interface thread \"uiThread\" is unjoinable!" << std::endl; }
    }
    else {
    std::cout << "[INFO] UI has been disabled." << std::endl;
        tts.Initialize();
        stt.Initialize();
        tts.Shutdown(); // fix konamask (virt input) not destroying
    }
    // cfg.Initialize();
    // ui.Initialize();
    // ui.Render(&uiRunning);
}
