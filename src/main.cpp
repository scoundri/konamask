#include "SpeechToText.h"
#include "TextToSpeech.h"
#include "Interface.h"
#include <atomic>
#include <thread>

// object declaration
Settings& cfg = Settings::GetInstance();
SpeechToText stt; 
TextToSpeech tts;
Interface ui;

std::atomic<bool> uiRunning{true};

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
    
    cfg.Initialize();
    /*if (cfg.UI_ENABLED) {
        ui.Initialize();
        std::thread uiThread([&](){
            try {
                ui.Render(&uiRunning);
            }
            catch (const std::exception& e) {
                std::cerr << "UI exception: " << e.what() << "\n"
                          << "Falling back to console mode.\n";
                uiRunning = false;
            }
            catch (...) {
                std::cerr << "Unexpected UI exception! Falling back to console mode.\n";
                uiRunning = false;
            }
        });
        ui.Render(&uiRunning);
        tts.Initialize();
        stt.Initialize();
        tts.Shutdown(); // fix konamask (virt input) not destroying
        if (uiThread.joinable()) {
            uiRunning = false;
            uiThread.join();
        }
    }
    else {
        tts.Initialize();
        stt.Initialize();
        tts.Shutdown(); // fix konamask (virt input) not destroying
    }*/
    cfg.Initialize();
    ui.Initialize();
    ui.Render(&uiRunning);
}
