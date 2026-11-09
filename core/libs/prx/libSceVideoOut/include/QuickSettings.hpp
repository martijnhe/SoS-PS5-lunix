#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_QUICKSETTINGS_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_QUICKSETTINGS_HPP

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>
#include "prx/libScePad/include/PadControllerChannel.hpp"

// The L3+R3 / F10 menu's settings, read by the flip loop and the pad code and saved in quick_menu.ini next to the game.
namespace QuickSettings {

inline constexpr const char* kIconNames[] = {"automatic", "playstation", "xbox", "switch"};
inline constexpr int kIconCount = 4;
inline constexpr int kFpsChoices[] = {0, 30, 40, 45, 60, 90, 120};
inline constexpr int kFpsChoicesAtVblank = 5;  // 90 and 120 need the launcher's 120 or Unlocked frame rate
inline constexpr int kFpsChoiceCount = 7;

struct State {
    std::atomic<int> fpsCap{0};  // frames per second, 0 = no limit
    std::atomic<int> icons{0};   // index into kIconNames
    std::atomic<bool> swapConfirm{false}, swapShoulders{false}, invertLook{false};
};

inline int IconIndex(const std::string& name) {
    for (int i = 0; i < kIconCount; ++i)
        if (name == kIconNames[i]) return i;
    return 0;
}

inline std::string LauncherIcons() {
    const char* value = std::getenv("SOS_CONTROLLER_BUTTON_ICONS");
    return value != nullptr ? value : "automatic";
}

inline State& Get() {
    static State* const state = [] {
        auto* loaded = new State;
        if (const char* cap = std::getenv("APS5_FPS_CAP")) loaded->fpsCap = std::atoi(cap);
        loaded->icons = IconIndex(LauncherIcons());
        std::ifstream file("quick_menu.ini");
        std::string line, icons, launcher;
        while (std::getline(file, line)) {
            const auto at = line.find('=');
            if (at == std::string::npos) continue;
            const std::string key = line.substr(0, at), value = line.substr(at + 1);
            if (key == "fps_cap") loaded->fpsCap = std::atoi(value.c_str());
            else if (key == "icons") icons = value;
            else if (key == "icons_launcher") launcher = value;
            else if (key == "swap_confirm") loaded->swapConfirm = value == "1";
            else if (key == "swap_shoulders") loaded->swapShoulders = value == "1";
            else if (key == "invert_look") loaded->invertLook = value == "1";
        }
        // A newer choice in the launcher wins over the one made in the menu.
        if (!icons.empty() && launcher == LauncherIcons()) loaded->icons = IconIndex(icons);
        PadControllerChannel::PublishIcons(static_cast<std::uint32_t>(loaded->icons.load()) + 1);
        return loaded;
    }();
    return *state;
}

inline void Save() {
    const State& state = Get();
    std::ofstream file("quick_menu.ini", std::ios::trunc);
    file << "fps_cap=" << state.fpsCap.load() << "\nicons=" << kIconNames[state.icons.load()] << "\nicons_launcher=" << LauncherIcons()
         << "\nswap_confirm=" << (state.swapConfirm ? 1 : 0) << "\nswap_shoulders=" << (state.swapShoulders ? 1 : 0)
         << "\ninvert_look=" << (state.invertLook ? 1 : 0) << "\n";
    PadControllerChannel::PublishIcons(static_cast<std::uint32_t>(state.icons.load()) + 1);
}

}

#endif
