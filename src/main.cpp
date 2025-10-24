#include "SpeechToText.h"
#include "TextToSpeech.h"
#include "Interface.h"
#include "Logger.h"
#include "curl.h"
#include <atomic>
#include <cstdlib>
#include <thread>
#include <sys/stat.h> // for mkdir
#include <limits.h>
#include <fstream>
#include "Tray.h"

// object declaration
Settings& cfg = Settings::GetInstance();
SpeechToText stt; 
TextToSpeech tts;
Interface ui;

std::atomic<bool> uiRunning{true};

#if defined(_WIN32) || defined(_WIN64)
  #include <windows.h>
  #include <shellapi.h>
  // convert utf-8 string to wstring
  static std::wstring utf8_to_wstring(const std::string &s) {
      if (s.empty()) return std::wstring();
      int size_needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
      std::wstring w(size_needed, 0);
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], size_needed);
      return w;
  }
#else
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <errno.h>
#endif

static bool open_url(const std::string &url) {
    if (url.empty()) return false;

#if defined(_WIN32) || defined(_WIN64)
    // use ShellExecuteW using a utf-16 string
    std::wstring wurl = utf8_to_wstring(url);
    HINSTANCE result = ShellExecuteW(NULL, L"open", wurl.c_str(), NULL, NULL, SW_SHOWNORMAL);
    // ShellExecute returns value > 32 for success
    return reinterpret_cast<intptr_t>(result) > 32;
#else
    // prefer xdg-open (widely available)
    pid_t pid = fork();
    if (pid == -1) return false; // fork failed
    if (pid == 0) {
        // child
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
        // if xdg-open isn't available, try sensible-browser (rare) then exit
        execlp("sensible-browser", "sensible-browser", url.c_str(), static_cast<char*>(nullptr));
        _exit(EXIT_FAILURE);
    }
    return true;
#endif
}

static void shutdown(struct tray_menu *item) {
    Interface::Minimize();
    tts.Initialize();
    stt.Initialize();
    tts.Shutdown(); // fix konamask (virt input) not destroying
}
// kind of wasteful, but can't find another way 
static inline void support(struct tray_menu *item) {
    if (!open_url("https://nightvoid.com/support")) {
        std::cout << "[ERROR] Could not open support email URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] Email URL oppened!" << std::endl;  }
}

static inline void konacode(struct tray_menu *item) {
    if (!open_url("https://konacode.com/")) {
        std::cout << "[ERROR] Could not open URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] URL oppened successfully!" << std::endl;  }
}

static inline void github(struct tray_menu *item) {
    if (!open_url("https://github.com/kona-code")) {
        std::cout << "[ERROR] Could not open URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] URL oppened successfully!" << std::endl;  }
}

static inline void nightvoid(struct tray_menu *item) {
    if (!open_url("https://nightvoid.com/")) {
        std::cout << "[ERROR] Could not open URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] URL oppened successfully!" << std::endl;  }
}

static inline void software(struct tray_menu *item) {
    if (!open_url("https://software.nightvoid.com/")) {
        std::cout << "[ERROR] Could not open URL!" << std::endl;
    } 
    else {  std::cout << "[INFO] URL oppened successfully!" << std::endl;  }
}

static inline void openui(struct tray_menu *item) {
    Interface::Show();
}
static inline void kill(struct tray_menu *item) {
    std::exit(EXIT_SUCCESS);
}

static struct tray tray = {
    .icon = TRAY_ICON1,
    .menu =
        (struct tray_menu[]){
            {.text = "Show Window", .cb = openui},
            {.text = "-"},
            {.text = "konacode",
             .submenu =
                 (struct tray_menu[]){
                     {.text = "website", .cb = konacode},
                     {.text = "GitHub", .cb = github},
                     {.text = NULL}}},
              {.text = "NightVoid",
               .submenu =
                 (struct tray_menu[]){
                     {.text = "website", .cb = nightvoid},
                     {.text = "software", .cb = software},
                     {.text = "support", .cb = support},
              {.text = NULL}}},
            {.text = "-"},
            {.text = "Kill", .cb = kill},
            {.text = "Quit", .cb = shutdown},
            {.text = NULL}},
};

static std::string now = [](){
    std::time_t t = std::time(nullptr);
    char b[20];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return std::string(b);
}();

static bool first() {
    #ifdef _WIN32 // not yet tested
    PWSTR knownPath = nullptr;
    std::wstring base;
    std::string path;
    std::string backup;
    std::wstring confpath = L"konacode\\konamask\\config.ini"
    std::wstring backupconfpath = L"konacode\\konamask\\backup-config.ini"
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &knownPath);
    if (SUCCEEDED(hr) && knownPath) {
        base.assign(knownPath);
        CoTaskMemFree(knownPath);
    } 
    else {
        // LOCALAPPDATA env var
        wchar_t buf[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            base.assign(buf, len);
        } 
        else {
            // USERPROFILE + "\\AppData\\Local"
            len = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                base.assign(buf, len);
                base += L"\\AppData\\Local";
            } else {
                // return relative path under current working directory
                std::wstring fallback = confpath;
                return wide_to_utf8(fallback);
            }
        }
    }


    std::filesystem::path dir = std::filesystem::path(base) / confpath;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // ignore errors

    std::filesystem::path full = dir;
    path = wide_to_utf8(full.wstring());

    dir = std::filesystem::path(base) / backupconfpath;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // ignore errors

    full = dir;
    backup = wide_to_utf8(full.wstring());

#else
    char path[PATH_MAX];
    char backup[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", getenv("HOME"), ".config/konacode/konamask/config.ini");
    snprintf(backup, sizeof(backup), "%s/%s", getenv("HOME"), ".config/konacode/konamask/config_backup.ini");
#endif
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
        if (mkdir(config_dir, 0755) != 0 && errno != EEXIST) {
        perror("mkdir final");
        std::cout << "[ERROR] Directory \"" << config_dir << "\" could not be created!" << std::endl;
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
    if (Settings::CopyFile(path, backup)) {
        std::cout << "[INFO] Configuration successfully backed-up!" << std::endl;
    }
#ifdef _WIN32
    std::cout << "[INFO] Windows platform detected! Downloading tray icon..." << std::endl;
    if (Fetch::Download("https://cdn.nightvoid.com/software/konacode/konamask/tray.ico", path)) {
        std::cout << "[INFO] Configuration downloaded successfully!" << std::endl;
    }
    else if (Fetch::Download("https://console.nightvoid.com/konacode/konamask/tray.ico", path)) {
        std::cout << "[INFO] Configuration downloaded successfully from fallback server!" << std::endl;
    } 
    else {
        std::cerr << "[INFO] Unable to download configuration file!\n[INFO] Download it from:"
                     "[INFO] konamask github repository: https://github.com/kona-code/konamask\n"
                     "[INFO] NightVoid archives: https://archive.nightvoid.com/Development/konamask/Backup%20configuration/ (URL might change in the future)" << std::endl;
    }
#endif

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
    "       lcc:.....,;ccccclcc;..;cdocc,c,.,:....,:cd .cccl:                   Software developed by kona                   \n"
    "       cc;.',;lccclccccocc....;lldc::c..::cox.l 'x  ,cco.          ┌─────────────────────────────────────────┐          \n"
    "       ll:clcclccoccccccc:.....:;;occc'oO00dc.dl      cco              website: https://konacode.com/                   \n"
    "      olclocclccolcccc,:c.......l.,occ,,kOxk..dd;      .l.             my projects: https://nightvoid.com/              \n"
    "     k.,clccclcldcccc:.l;:ox0o..',.'oc;..::l .doo'                     github: https://github.com/kona-code/            \n"
    "   '.  ,lccccocddcccc;o000Okk:......,l;..:;l..cdlo,         \n"
    "       ,dcccoxldocccc.:oOxxkld ......:;.......'do:oc                   contact me here:                                 \n"
    "       :lccdddodoccdd;... ,:,d,...............cddc:lk                  Discord: konacode                                \n"
    "       llcddddxddcdddo,...coc;........,,..... lddd::.x                 Email: kona@nightvoid.com                        \n"
    "      'lldddddxddlddddl,...........;,'...';:    ddo:o.             └─────────────────────────────────────────┘          \n"
    "      olddddddddxloodddl;..............,c:c;     :dlcl      \n"
    "     dccdddddddxoooolodl...:,,,''';,;ldlllc       .ocl      \n"
    "    k:ccc:clodlllllllcl'..,ll:;,,,,,,odl:oo.       .cc      \n"
    "  'd;::.......',;::::c;...doccllc;,,'.dl;o:,        :c      \n"
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
    Logger::Initialize();
    if (cfg.get<bool>("enable_logging_to_file", false)) {
        std::cout << "[INFO] Logger has been enabled!" << std::endl;
        Logger::GetInstance().log("\n\n\n\n\n\n\n\n>--------------------[KONAMASK STARTED @ ");
            std::time_t t = std::time(nullptr); char b[20];
        Logger::GetInstance().log(now);
        Logger::GetInstance().log("]>------------------------------------------------[<\n\n     occ.                            .klccc.                \n"
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
    "       lcc:.....,;ccccclcc;..;cdocc,c,.,:....,:cd .cccl:                   Software developed by kona                   \n"
    "       cc;.',;lccclccccocc....;lldc::c..::cox.l 'x  ,cco.          ┌─────────────────────────────────────────┐          \n"
    "       ll:clcclccoccccccc:.....:;;occc'oO00dc.dl      cco              website: https://konacode.com/                   \n"
    "      olclocclccolcccc,:c.......l.,occ,,kOxk..dd;      .l.             my projects: https://nightvoid.com/              \n"
    "     k.,clccclcldcccc:.l;:ox0o..',.'oc;..::l .doo'                     github: https://github.com/kona-code/            \n"
    "   '.  ,lccccocddcccc;o000Okk:......,l;..:;l..cdlo,         \n"
    "       ,dcccoxldocccc.:oOxxkld ......:;.......'do:oc                   contact me here:                                 \n"
    "       :lccdddodoccdd;... ,:,d,...............cddc:lk                  Discord: konacode                                \n"
    "       llcddddxddcdddo,...coc;........,,..... lddd::.x                 Email: kona@nightvoid.com                        \n"
    "      'lldddddxddlddddl,...........;,'...';:    ddo:o.             └─────────────────────────────────────────┘          \n"
    "      olddddddddxloodddl;..............,c:c;     :dlcl      \n"
    "     dccdddddddxoooolodl...:,,,''';,;ldlllc       .ocl      \n"
    "    k:ccc:clodlllllllcl'..,ll:;,,,,,,odl:oo.       .cc      \n"
    "  'd;::.......',;::::c;...doccllc;,,'.dl;o:,        :c      \n"
    " ll,:odl::;,'.............:ldolloo:.  :cco,.        .'      \n"
    "Oc:loddoddxc,';'....'.....ccclolclod:.:cdc'                 \n"
    "llldxdoxo:,'..;''',;;'''':ddllooodllllldl;.                 \n"
    "dodkxdko'''''':;;;;;;,,;xkkxo:clllodooxoc;x                 \n"
    "ddxxdkO;,,,,,;cc:cc;;;;lOkOxl,;lllloollllllo                \n\n[INFO] Logger has been enabled!\n");
    }
    //tts.Initialize();
    if (cfg.get<bool>("enable_user_interface", true)) {
        // ui.Initialize();
        // ui.Render(&uiRunning);
        std::thread uiThread([&](){
            try {
                ui.Initialize();
                ui.Render(&uiRunning);
            }
            catch (const std::exception& e) {
                std::cerr << "[ERROR] UI exception occured: " << e.what() << "\n"
                          << "[INFO] Falling back to console mode!" << std::endl;
                Logger::GetInstance().log("UI exception occured: ");
                Logger::GetInstance().log(e.what());
                Logger::GetInstance().log("\n[INFO] Falling back to console mode!\n");
                uiRunning = false;
            }
            catch (...) {
                std::cerr << "[ERROR] Unexpected UI exception! Falling back to console mode." << std::endl;
                Logger::GetInstance().log("[ERROR] A user interface exception occured!\n[INFO] Falling back to console mode!");
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
                Logger::GetInstance().log("[INFO] uiThread destroyed successfully!");
            }
        } 
        catch (...) { 
            std::cout << "[ERROR] Interface thread \"uiThread\" is unjoinable!" << std::endl; 
            Logger::GetInstance().log("[ERROR] Interface thread \"uiThread\" is unjoinable!"); 
        }
        try {
            if (trayThread.joinable()) {
                trayThread.join();
                std::cout << "[INFO] trayThread destroyed successfully!" << std::endl;
                Logger::GetInstance().log("[INFO] trayThread destroyed successfully!");
            }
        } 
        catch (...) { 
            std::cout << "[ERROR] Tray thread \"trayThread\" is unjoinable!" << std::endl;
            Logger::GetInstance().log("[ERROR] Interface thread \"trayThread\" is unjoinable!"); 
        }
    }
    else {
        std::cout << "[INFO] User interface has been disabled." << std::endl;
            Logger::GetInstance().log("[INFO] User interface has been disabled."); 
            tts.Initialize();
            stt.Initialize();    
            tts.Shutdown();
            try {
                if (trayThread.joinable()) {
                    trayThread.join();
                    std::cout << "[INFO] trayThread destroyed successfully!" << std::endl;
                    Logger::GetInstance().log("[INFO] trayThread destroyed successfully!");
                }
            } 
            catch (...) { 
                std::cout << "[ERROR] Tray thread \"trayThread\" is unjoinable!" << std::endl; 
                Logger::GetInstance().log("[ERROR] Interface thread \"trayThread\" is unjoinable!"); 
            }
    }
    // cfg.Initialize();
    // ui.Initialize();
    // ui.Render(&uiRunning);*/
}
