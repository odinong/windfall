#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include <shlobj.h>
#include <windows.h>

#include "core/config.h"
#include "core/game_build.h"
#include "core/game_module.h"
#include "core/guest.h"
#include "core/log.h"
#include "core/rpx_loader.h"
#include "core/setup.h"
#include "debug/profiler.h"
#include "debug/watchdog.h"
#include "gpu/gpu.h"
#include "gpu/window.h"
#include "hle/cemu_bridge.h"
#include "hle/coreinit/sync.h"
#include "hle/hle.h"
#include "enhancements/graphic_packs.h"
#include "enhancements/playtest.h"
#include "net/discord_rpc.h"
#include "net/multiplayer.h"
#include "enhancements/randomizer.h"
#include "mods/mods.h"
#include "os/sched.h"
#include "ui/settings.h"
#include "wf/recomp_api.h"

namespace wf {
void init_function_table();
namespace hle { void heaps_init(uint32_t mem2Start, uint32_t mem2End); }

Config& config()
{
    static Config c;
    return c;
}

namespace {

std::filesystem::path appdata_dir()
{
    PWSTR p = nullptr;
    std::filesystem::path r;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p) == S_OK)
        r = std::filesystem::path(p) / L"WWHDRecomp";
    CoTaskMemFree(p);
    return r;
}

bool parse_args(int argc, wchar_t** argv)
{
    Config& c = config();
    std::filesystem::path given;
    for (int i = 1; i < argc; i++)
    {
        std::wstring a = argv[i];
        if (a == L"--game" && i + 1 < argc) given = argv[++i];
        else if (a == L"--playtest") playtest::set_enabled();
        else if (!a.starts_with(L"-") && given.empty()) given = a;
    }
    if (given.empty())
        if (const wchar_t* e = _wgetenv(L"WF_GAME_DIR")) given = e;
    c.dataDir = appdata_dir();
    c.saveDir = c.dataDir / "save";
    if (const wchar_t* e = _wgetenv(L"WF_SAVE_DIR")) c.saveDir = e;
    if (playtest::enabled())
    {
        if (!_wgetenv(L"WF_SAVE_DIR")) c.saveDir = c.dataDir / "playtest_save";
        std::error_code ec;
        std::filesystem::remove_all(c.saveDir, ec);
        log_info("playtest: new game in {}, going to Outset Island", c.saveDir.string());
    }
    std::filesystem::create_directories(c.saveDir);
    settings::load();
    c.gameDir = setup::find_game_dir(given);
    if (c.gameDir.empty())
    {
        log_error("No usable game folder. Run Windfall and choose your Wind Waker HD dump (the folder with code/, content/, meta/), "
                  "or pass --game <folder>.");
        return false;
    }
    return true;
}

void start_main_thread()
{
    uint32_t stackSize = std::max(g_image.stackSize, 0x10000u);
    uint32_t thread = sys_alloc(0x6A0, 8);
    uint32_t stack = sys_alloc(stackSize, 16);
    std::string exe = "cking.rpx";
    uint32_t argStr = sys_alloc((uint32_t)exe.size() + 1, 4);
    memcpy(gptr(argStr), exe.c_str(), exe.size() + 1);
    uint32_t argv = sys_alloc(8, 4);
    wr32(argv, argStr);
    os::LockGuard g;
    os::Thread* t = os::create_thread(thread, g_image.entry, 1, argv, stack + stackSize, stackSize, 16, 0x2);
    t->name = "Main thread";
    os::resume(*t);
}

}
}

int wmain(int argc, wchar_t** argv)
{
    using namespace wf;
    log_info("Windfall {}", WINDFALL_VERSION);
    if (!parse_args(argc, argv))
        return 1;
    std::filesystem::path rpx = config().gameDir / "code" / "cking.rpx";
    if (!std::filesystem::exists(rpx))
        fatal("{} not found. Point --game at the folder that contains code/, content/ and meta/.", rpx.string());
    if (std::string why = game_module::load(game_module::path()); !why.empty())
    {
        if (!game_build::available())
            fatal("{}. In a developer build, build.bat builds windfall_game.dll from your dump (or set WF_GAME_MODULE).", why);
        log_info("game module: {}; building it", why);
        if (!setup::build_screen(config().gameDir, why))
            return 0;
        if (std::string again = game_module::load(game_module::built_dir() / "windfall_game.dll"); !again.empty())
            fatal("The game code built for this PC doesn't work: {}", again);
    }
    std::filesystem::path randoGame = rando::game_dir();
    if (!randoGame.empty())
    {
        config().gameDir = randoGame;
        if (!_wgetenv(L"WF_SAVE_DIR") && !playtest::enabled())
        {
            config().saveDir = config().dataDir / "save_rando";
            std::filesystem::create_directories(config().saveDir);
        }
    }

    memory_init();
    debug::watchdog_start();
    init_function_table();
    RpxLoadResult img = randoGame.empty() ? load_rpx(rpx) : load_rpx_patched(rpx, randoGame / "code" / "cking.rpx");
    if (img.dataEnd > gmem::kSysAreaBase)
        fatal("RPX data overlaps the runtime system area");
    os::init(g_image.sdaBase, g_image.sda2Base);
    hle::heaps_init(gmem::kMem2HeapBase, gmem::kMem2End);
    cemu::register_modules();
    hle::bind_imports();
    hle::start_alarm_thread();
    start_main_thread();

    settings::apply();
    window::create("The Legend of Zelda: The Wind Waker HD (Windfall)");
    gp::activate();
    gp::apply_data_patches();
    mods::start();
    rando::start();
    if (const char* g = std::getenv("WF_RANDO_GENERATE"))
        rando::generate(g, {});
    mp::start();
    discord::start();
    gpu::start();
    os::start_cores();
    debug::profiler_start();
    window::run_loop();
    log_info("window closed, exiting");
    std::_Exit(0);
}
