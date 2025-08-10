#include "SpeechToText.h"
#include "TextToSpeech.h"
#include "Interface.h"
#include "curl.h"
#include <atomic>
#include <thread>
#include <sys/stat.h> // for mkdir
#include <limits.h>
#include "Tray.h"

// object declaration
Settings& cfg = Settings::GetInstance();
SpeechToText stt; 
TextToSpeech tts;
Interface ui;

std::atomic<bool> uiRunning{true};

static void shutdown(struct tray_menu *item) {
    Interface::Minimize();
    tts.Initialize();
    stt.Initialize();
    tts.Shutdown(); // fix konamask (virt input) not destroying
    // add thread destruction
}

static void openui(struct tray_menu *item) {

}

struct tray tray = {
    .icon = TRAY_ICON1,
    .menu = (struct tray_menu[]){{"Toggle me", 0, 0, openui, NULL},
                                 {"-", 0, 0, NULL, NULL},
                                 {"Quit", 0, 0, shutdown, NULL},
                                 {NULL, 0, 0, NULL, NULL}},
};

static bool first() {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", getenv("HOME"), ".config/konacode/konamask/config.ini");
    struct stat info;

    if (stat(path, &info) != 0) {
        std::cerr << "[INFO] Configuration file does not exist: " << strerror(errno) << std::endl;
    }
    else if (info.st_mode & S_IFREG) {
        return true;
    } else {
        std::cerr << "[ERROR] Path \"" << path << "\" is not a file." << std::endl;
    }

    // create config dir
    char config_dir[PATH_MAX];
    snprintf(config_dir, sizeof(config_dir), "%s/%s", getenv("HOME"), ".config/konacode/konamask/");
    size_t len = strlen(config_dir);
    if (config_dir[len - 1] == '/')
        config_dir[len - 1] = '\0';

    for (char *p = config_dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(config_dir, 0777); // ignore errors for existing dirs
            *p = '/';
        }
    }
    std::cout << "[INFO] Directory \"" << config_dir << "\" was checked/created successfully!" << std::endl;

    std::cout << "[INFO] First-time launch detected! Downloading config.ini..." << std::endl;
    if (Fetch::Download("https://cdn.nightvoid.com/software/konacode/konamask/config.ini", path)) {
        std::cout << "[INFO] Configuration downloaded successfully!" << std::endl;
    }
    else if (Fetch::Download("https://console.nightvoid.com/konacode/konamask/config.ini", path)) {
        std::cout << "[INFO] Configuration downloaded successfully from fallback server!" << std::endl;
    } 
    else {
        std::cerr << "[INFO] Unable to download configuration file!\n[INFO] Download it from:"
                     "[INFO] konamask github repository: https://github.com/kona-code/konamask\n"
                                                                    "[INFO] NightVoid archives: https://archive.nightvoid.com/Development/konamask/Backup%20configuration/ (URL might change in the future)" << std::endl;
    }
    
    return true;
}

int main() {

    // developer notice
    std::cout << 
"\n\n     occ.                            .klccc.                \n"
    "    dcc                           o0xlccccd00O00KXNk.       \n"
    "   ,cl'              .OXK00OOOOOkoccccccc'......;ccc::clxO, \n"
    "   .ld.         .KOxocccccccccllccccccccc:...               \n"
    "    'd,       0xlccccccccccccclccccccccc:....;kKx           \n"
    "      k.   .0occcccccccccccccc:cccclccc;..',:ccccoO0                 __                                    __           \n"
    "        l xocccccccccccccccccc:;:lcclc;;:cccccccccccoOc             / /______  ____  ____ __________  ____/ /__         \n"
    "         xccccccccccccccccccccc:';lcclcccccccccccclcccckd          / //_/ __ \\/ __ \\/ __ `/ ___/ __ \\/ __  / _ \\    \n"
    "        occcccccccccccccccclcc:c:.;lccdlcccccccccccllcccck,       / ,< / /_/ / / / / /_/ / /__/ /_/ / /_/ /  __/        \n"
    "       ,cccccc;.,;;,,',:ccllccc:c:.cllc:;ccccllcccccl,  .clO     /_/|_|\\____/_/ /_/\\__,_/\\___/\\____/\\__,_/\\___/   \n"
    "       lcccc;.......,:clccl':colcc,,c.:c...;cccooccccl:    '\n"
    "       lcc:.....,;ccccclcc;..;cdocc,c,.,:....,:cd .cccl:                   Software developed by kona                    \n"
    "       cc;.',;lccclccccocc....;lldc::c..::cox.l 'x  ,cco.          ┌─────────────────────────────────────────┐           \n"
    "       ll:clcclccoccccccc:.....:;;occc'oO00dc.dl      cco              website: https://konacode.com/                    \n"
    "      olclocclccolcccc,:c.......l.,occ,,kOxk..dd;      .l.             my projects: https://nightvoid.com/               \n"
    "     k.,clccclcldcccc:.l;:ox0o..',.'oc;..::l .doo'                     github: https://github.com/kona-code/             \n"
    "   '.  ,lccccocddcccc;o000Okk:......,l;..:;l..cdlo,         \n"
    "       ,dcccoxldocccc.:oOxxkld ......:;.......'do:oc                   contact me here:                                  \n"
    "       :lccdddodoccdd;... ,:,d,...............cddc:lk                  Discord: konacode                                 \n"
    "       llcddddxddcdddo,...coc;........,,..... lddd::.x                 Email: kona@nightvoid.com                         \n"
    "      'lldddddxddlddddl,...........;,'...';:    ddo:o.      \n"
    "      olddddddddxloodddl;..............,c:c;     :dlcl                 more information:                                 \n"
    "     dccdddddddxoooolodl...:,,,''';,;ldlllc       .ocl                 https://konacode.com/                             \n"
    "    k:ccc:clodlllllllcl'..,ll:;,,,,,,odl:oo.       .cc                 https://nightvoid.com/                            \n"
    "  'd;::.......',;::::c;...doccllc;,,'.dl;o:,        :c             └─────────────────────────────────────────┘           \n"
    " ll,:odl::;,'.............:ldolloo:.  :cco,.        .'      \n"
    "Oc:loddoddxc,';'....'.....ccclolclod:.:cdc'                 \n"
    "llldxdoxo:,'..;''',;;'''':ddllooodllllldl;.                 \n"
    "dodkxdko'''''':;;;;;;,,;xkkxo:clllodooxoc;x                 \n"
    "ddxxdkO;,,,,,;cc:cc;;;;lOkOxl,;lllloollllllo                \n";

    first();
    std::thread trayThread([&](){
        tray_init(&tray);
            while (tray_loop(1) == 0);
        tray_exit();
    });
    cfg.Initialize();
    if (cfg.get<int>("enable_user_interface", true)) {
    std::cout << "[INFO] User interface has been enabled." << std::endl;
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
                std::cout << "[INFO] uiThread destroyed successfully!" << std::endl;
            }
        } catch (...) { std::cout << "[ERROR] Interface thread \"uiThread\" is unjoinable!" << std::endl; }
        try {
            if (trayThread.joinable()) {
                trayThread.join();
                std::cout << "[INFO] trayThread destroyed successfully!" << std::endl;
            }
        } catch (...) { std::cout << "[ERROR] Tray thread \"trayThread\" is unjoinable!" << std::endl; }
    }
    else {
    std::cout << "[INFO] User interface has been disabled." << std::endl;
        tts.Initialize();
        stt.Initialize();
        tts.Shutdown(); // fix konamask (virt input) not destroying
        if (trayThread.joinable()) {
            trayThread.join();
            std::cout << "[INFO] trayThread destroyed successfully!" << std::endl;
        }
    }
    // cfg.Initialize();
    // ui.Initialize();
    // ui.Render(&uiRunning);
}
