// Native Linux launcher for SonsOfSparta-PS5.
//
// Does what launcher.cpp does on Windows:
//   1. finds the player's own decrypted game files next to this program,
//   2. shows the settings window (GTK 3) unless [launcher] show=0 in launcher.ini,
//   3. on the first start unpacks SELF-wrapped eboot.bin / modules into prep/ and runs
//      tools/relinker in its Linux mode to build sos_runtime, with a progress window,
//   4. links Media and sce_sys into app0/, and
//   5. starts the game with the chosen settings.
//
// Settings are stored in launcher.ini with the same sections and keys as on Windows,
// and the resolution in the game's own settings save, so a folder can move between
// the two systems. Without a display the launcher runs from the terminal only.

#include "../GameSettingsSeed.hpp"
#include "MissingImportStubs.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace {

constexpr const char* kTitle = "SonsOfSparta-PS5 Native";
constexpr const char* kName = "God of War Sons of Sparta";
constexpr const char* kTitleId = "PPSA28997";
constexpr const char* kRuntime = "sos_runtime";
constexpr const char* kRequired[] = {"sce_sys", "Media"};
constexpr const char* kSettingsFile = "_sd/GOWSOSSAVE999/GoW_SoS999.dat";
constexpr const char* kResolutionKey = "resolutionIndex";
// Increase when the preparation steps change, so older prepared files are rebuilt.
constexpr int kPrepareVersion = 1;

// resolutionIndex picks from the list the game builds at run time from the PS5 display manager: the 16:9 sizes
// 1920 to 3200 wide in steps of 320, then to 3840 in steps of 160, sorted from the widest. Only the common ones are offered.
struct Resolution { int index; const char* label; };
constexpr Resolution kResolutions[] = {{0, "3840 x 2160 (4K)"}, {6, "2560 x 1440 (2K / QHD)"}, {8, "1920 x 1080 (Full HD)"}};
constexpr const char* kButtonIcons[] = {"automatic", "playstation", "xbox", "switch"};

bool gui = false;

// --- messages ---------------------------------------------------------------------------------

void ShowMessage(GtkMessageType type, const std::string& message) {
    std::cerr << "\n" << kName << ": " << message << "\n";
    if (!gui) return;
    GtkWidget* dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", message.c_str());
    gtk_window_set_title(GTK_WINDOW(dialog), kTitle);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    while (gtk_events_pending()) gtk_main_iteration();
}

void Fail(const std::string& message) { ShowMessage(GTK_MESSAGE_ERROR, message); }
void Warn(const std::string& message) { ShowMessage(GTK_MESSAGE_WARNING, message); }
void Status(const std::string& message) { std::cout << "[launcher] " << message << std::endl; }

// --- files ------------------------------------------------------------------------------------

bool Exists(const fs::path& path) {
    std::error_code ignored;
    return fs::exists(path, ignored);
}

std::uintmax_t FileSize(const fs::path& path) {
    std::error_code ignored;
    const auto size = fs::file_size(path, ignored);
    return ignored ? 0 : size;
}

std::string ReadStamp(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    return line;
}

void WriteStamp(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::trunc);
    out << text;
}

bool IsElf(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    return in.read(magic, 4) && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

// Unpacks a PS5 SELF whose segments are stored uncompressed and unencrypted (a decrypted dump).
bool ExtractSelf(const fs::path& selfPath, const fs::path& elfPath) {
    std::ifstream in(selfPath, std::ios::binary);
    if (!in) return false;
    const auto readAt = [&](std::uint64_t offset, void* buffer, std::size_t size) {
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset));
        return static_cast<bool>(in.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size)));
    };
    struct Entry { std::uint64_t props, offset, fileSize, memSize; };
    struct Header { std::uint32_t type, flags; std::uint64_t offset, vaddr, paddr, fileSize, memSize, align; };
    static_assert(sizeof(Entry) == 32 && sizeof(Header) == 56);
    unsigned char head[0x20] = {};
    if (!readAt(0, head, sizeof(head)) || head[0] != 0x4f || head[1] != 0x15 || head[2] != 0x3d || head[3] != 0x1d) return false;
    std::uint16_t entryCount = 0;
    std::memcpy(&entryCount, head + 0x18, 2);
    if (entryCount == 0 || entryCount >= 4096) return false;
    std::vector<Entry> entries(entryCount);
    unsigned char elfHead[64] = {};
    const std::uint64_t elfStart = 0x20 + entryCount * sizeof(Entry);
    if (!readAt(0x20, entries.data(), entryCount * sizeof(Entry)) || !readAt(elfStart, elfHead, sizeof(elfHead)) || elfHead[0] != 0x7f || elfHead[1] != 'E')
        return false;
    std::uint64_t tableOffset = 0;
    std::uint16_t count = 0;
    std::memcpy(&tableOffset, elfHead + 0x20, 8);
    std::memcpy(&count, elfHead + 0x38, 2);
    if (count == 0 || count >= 512) return false;
    std::vector<Header> headers(count);
    if (!readAt(elfStart + tableOffset, headers.data(), count * sizeof(Header))) return false;
    for (const auto& entry : entries)
        if ((entry.props & 0x800) != 0 && (entry.props & 0xA) != 0) return false;  // compressed or encrypted

    std::fstream out(elfPath, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!out) return false;
    const auto writeAt = [&](std::uint64_t offset, const void* buffer, std::size_t size) {
        out.seekp(static_cast<std::streamoff>(offset));
        return static_cast<bool>(out.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size)));
    };
    bool ok = writeAt(0, elfHead, sizeof(elfHead)) && writeAt(tableOffset, headers.data(), headers.size() * sizeof(Header));
    std::uint64_t end = 0;
    for (const auto& header : headers) end = std::max(end, header.offset + header.fileSize);
    std::vector<char> chunk(1 << 22);
    for (const auto& entry : entries) {
        if (!ok || (entry.props & 0x800) == 0) continue;
        const auto index = static_cast<unsigned>((entry.props >> 20) & 0xfff);
        if (index >= headers.size()) continue;
        std::uint64_t remaining = std::min(entry.fileSize, headers[index].fileSize);
        std::uint64_t from = entry.offset, to = headers[index].offset;
        while (ok && remaining != 0) {
            const auto part = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, chunk.size()));
            ok = readAt(from, chunk.data(), part) && writeAt(to, chunk.data(), part);
            from += part;
            to += part;
            remaining -= part;
        }
    }
    out.close();
    if (ok) {
        std::error_code error;
        fs::resize_file(elfPath, end, error);
        ok = !error;
    }
    if (!ok) {
        std::error_code ignored;
        fs::remove(elfPath, ignored);
    }
    return ok;
}

bool ConvertOrCopy(const fs::path& from, const fs::path& to) {
    if (IsElf(from)) {
        std::error_code error;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
        return !error;
    }
    return ExtractSelf(from, to);
}

void PrepareModules(const fs::path& directory, const fs::path& prep, std::string& failed) {
    std::error_code error;
    fs::create_directories(prep / "sce_module", error);
    for (const char* sub : {"sce_module", "Media/Modules", "Media/Plugins"}) {
        for (const auto& item : fs::directory_iterator(directory / sub, error)) {
            if (!item.is_regular_file() || item.path().extension() != ".prx") continue;
            if (!ConvertOrCopy(item.path(), prep / "sce_module" / item.path().filename()))
                failed += (failed.empty() ? "" : ", ") + item.path().filename().string();
        }
    }
}

// Runs a program and waits for it. Output goes to logPath when given.
int Run(const std::vector<std::string>& args, const fs::path& logPath, const fs::path& errPath = {}) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (!logPath.empty()) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (errPath.empty()) posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        else posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    std::vector<std::string> copy = args;
    std::vector<char*> argv;
    for (auto& arg : copy) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// Links app0/<name> in the program folder (the guest's root) to <name> in the game folder.
void LinkIntoApp0(const fs::path& directory, const fs::path& gameFolder, const char* name) {
    const fs::path target = gameFolder / name;
    const fs::path link = directory / "app0" / name;
    if (!Exists(target)) return;
    std::error_code error;
    if (fs::is_symlink(link, error)) {
        if (fs::equivalent(link, target, error)) return;
        fs::remove(link, error);
    } else if (Exists(link)) {
        return;  // a real directory the player put there; leave it alone
    }
    fs::create_directories(directory / "app0", error);
    fs::create_directory_symlink(target, link, error);
    if (error) Status(std::string("Could not link app0/") + name + ": " + error.message());
}

bool IsSelf(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char magic[4] = {};
    return in.read(reinterpret_cast<char*>(magic), 4) && magic[0] == 0x4f && magic[1] == 0x15 && magic[2] == 0x3d && magic[3] == 0x1d;
}

fs::path GameExecutable(const fs::path& gameFolder) {
    for (const char* name : {"eboot.bin", "eboot.elf"})
        if (Exists(gameFolder / name)) return gameFolder / name;
    return {};
}

// Empty when the folder holds a usable dump, otherwise what is wrong with it.
std::string GameFolderProblem(const fs::path& gameFolder) {
    std::error_code error;
    if (gameFolder.empty() || !fs::is_directory(gameFolder, error)) return "Choose the folder of your PS5 game dump.";
    const fs::path executable = GameExecutable(gameFolder);
    if (executable.empty()) return "No eboot.bin or eboot.elf in this folder. Choose the folder that contains eboot.bin, sce_sys and Media.";
    if (!IsElf(executable) && !IsSelf(executable)) return executable.filename().string() + " is not a decrypted PS5 executable.";
    std::string missing;
    for (const char* item : kRequired)
        if (!Exists(gameFolder / item)) missing += (missing.empty() ? "" : ", ") + std::string(item);
    if (!missing.empty()) return "Missing in this folder: " + missing + ". Choose the complete game folder.";
    return "";
}

// --- launcher.ini (same file, sections and keys as the Windows launcher) ----------------------

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

using Ini = std::map<std::string, std::map<std::string, std::string>>;

Ini ReadIni(const fs::path& path) {
    Ini ini;
    std::ifstream in(path);
    std::string line, section;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') { section = Lower(Trim(line.substr(1, line.size() - 2))); continue; }
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        ini[section][Lower(Trim(line.substr(0, equals)))] = Trim(line.substr(equals + 1));
    }
    return ini;
}

// Canonical name first, then the historical one, as on Windows.
bool FindSetting(const Ini& ini, std::initializer_list<const char*> sections, std::initializer_list<const char*> keys, std::string& value) {
    for (const char* key : keys)
        for (const char* section : sections) {
            const auto found = ini.find(Lower(section));
            if (found == ini.end()) continue;
            const auto entry = found->second.find(Lower(key));
            if (entry != found->second.end()) { value = entry->second; return true; }
        }
    return false;
}

std::string Setting(const Ini& ini, std::initializer_list<const char*> sections, std::initializer_list<const char*> keys, const std::string& fallback) {
    std::string value;
    return FindSetting(ini, sections, keys, value) ? value : fallback;
}

// Sets one key the way WritePrivateProfileString does: the line is replaced in place, a missing key is added at the
// end of its section and a missing section at the end of the file. Everything else in the file is kept as it is.
bool WriteIniValue(const fs::path& path, const std::string& section, const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    const std::string entry = key + "=" + value;
    bool inSection = false, done = false;
    std::size_t sectionEnd = std::string::npos;
    for (std::size_t index = 0; index < lines.size() && !done; ++index) {
        const std::string trimmed = Trim(lines[index]);
        if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']') {
            if (inSection) break;
            inSection = Lower(Trim(trimmed.substr(1, trimmed.size() - 2))) == Lower(section);
            if (inSection) sectionEnd = index + 1;
            continue;
        }
        if (!inSection) continue;
        if (!trimmed.empty()) sectionEnd = index + 1;
        const auto equals = trimmed.find('=');
        if (equals != std::string::npos && Lower(Trim(trimmed.substr(0, equals))) == Lower(key)) {
            lines[index] = entry;
            done = true;
        }
    }
    if (!done) {
        if (sectionEnd == std::string::npos) {
            if (!lines.empty() && !Trim(lines.back()).empty()) lines.emplace_back();
            lines.push_back("[" + section + "]");
            lines.push_back(entry);
        } else {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), entry);
        }
    }
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc | std::ios::binary);
        for (const auto& line : lines) out << line << "\r\n";  // CRLF like the Windows profile API, so both launchers share the file
        if (!out) return false;
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    return !error;
}

// --- the game's settings save (resolution) ----------------------------------------------------

bool LoadSettings(const fs::path& directory, std::vector<unsigned char>& data) {
    const fs::path path = directory / kSettingsFile;
    if (!Exists(path)) {
        // Before the game has saved its settings once, the seeded defaults stand in for the file.
        data.assign(kSettingsHeader, kSettingsHeader + sizeof(kSettingsHeader));
        for (std::size_t i = 0; kSettingsBody[i] != 0; ++i) {
            data.push_back(static_cast<unsigned char>(kSettingsBody[i]));
            data.insert(data.end(), 3, 0);
        }
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

bool StoreSettings(const fs::path& directory, const std::vector<unsigned char>& data) {
    const fs::path path = directory / kSettingsFile;
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) { fs::remove(temporary, error); return false; }
    }
    fs::rename(temporary, path, error);
    return !error;
}

// Offset of the unsigned integer stored under a key that must occur exactly once, or npos.
std::size_t FindInteger(const std::vector<unsigned char>& data, const char* key, std::size_t& digits, int& value) {
    const std::string text = std::string("\"") + key + "\":";
    std::vector<unsigned char> pattern;
    for (const char character : text) {
        pattern.push_back(static_cast<unsigned char>(character));
        pattern.insert(pattern.end(), 3, 0);
    }
    const auto found = std::search(data.begin(), data.end(), pattern.begin(), pattern.end());
    if (found == data.end() || (found - data.begin()) % 4 != 0 || std::search(found + 1, data.end(), pattern.begin(), pattern.end()) != data.end())
        return std::string::npos;
    const std::size_t at = static_cast<std::size_t>(found - data.begin()) + pattern.size();
    const auto unit = [&](std::size_t index) {
        const std::size_t offset = at + index * 4;
        return offset + 4 <= data.size() && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 0 ? data[offset] : 0;
    };
    value = 0;
    for (digits = 0; digits < 9 && unit(digits) >= '0' && unit(digits) <= '9'; ++digits) value = value * 10 + (unit(digits) - '0');
    return digits != 0 && (unit(digits) == ',' || unit(digits) == '}') ? at : std::string::npos;
}

int ReadIntSetting(const fs::path& directory, const char* key) {
    std::vector<unsigned char> data;
    std::size_t digits = 0;
    int value = -1;
    return LoadSettings(directory, data) && FindInteger(data, key, digits, value) != std::string::npos ? value : -1;
}

// Rewrites only the digits of one value, in place and with the same length, so the rest of the file stays byte for byte
// the same. Nothing is written when the value is already set, so a missing file is seeded only when something changes.
bool WriteIntSetting(const fs::path& directory, const char* key, int value) {
    std::vector<unsigned char> data;
    std::size_t digits = 0;
    int current = -1;
    if (!LoadSettings(directory, data)) return false;
    const std::size_t at = FindInteger(data, key, digits, current);
    if (at == std::string::npos) return false;
    if (current == value) return true;
    const std::string text = std::to_string(value);
    if (value < 0 || text.size() != digits) return false;
    for (std::size_t i = 0; i < digits; ++i) data[at + i * 4] = static_cast<unsigned char>(text[i]);
    return StoreSettings(directory, data);
}

// resolution.txt was the earlier way to choose the resolution; its value only preselects the choice.
int ReadResolutionFile(const fs::path& directory) {
    std::ifstream in(directory / "resolution.txt");
    std::string value;
    if (!std::getline(in, value)) return -1;
    value = Lower(Trim(value));
    if (value.rfind("1080", 0) == 0) return 8;
    if (value.rfind("1440", 0) == 0) return 6;
    if (value.rfind("4k", 0) == 0 || value.rfind("2160", 0) == 0) return 0;
    return -1;
}

// --- settings ---------------------------------------------------------------------------------

struct Options {
    fs::path gameFolder;
    bool borderless = false;
    int frameRate = 0;    // 0 = 60, 1 = 120, 2 = unlocked
    int buttonIcons = 0;  // index into kButtonIcons
    bool showLauncher = true;
    bool showInGameFps = false;
    bool skipIntro = false;
    bool skipDialogue = false;
    bool revealFullMap = false;
    int resolution = -1;  // resolutionIndex to write, or -1 to keep the game's setting
};

Options ReadOptions(const fs::path& directory) {
    const Ini ini = ReadIni(directory / "launcher.ini");
    Options options;
    // Files copied next to the program, as on Windows, win over a saved game folder.
    const std::string saved = Setting(ini, {"game"}, {"folder"}, "");
    options.gameFolder = GameFolderProblem(directory).empty() || saved.empty() ? directory : fs::path(saved);
    options.borderless = Lower(Setting(ini, {"display", "Display"}, {"mode"}, "windowed")) == "borderless";
    const std::string rate = Lower(Setting(ini, {"display", "Display"}, {"frame_rate"}, "60"));
    options.frameRate = rate == "120" ? 1 : rate == "unlocked" ? 2 : 0;
    const std::string icons = Lower(Setting(ini, {"controller"}, {"button_icons"}, "automatic"));
    for (int index = 0; index < 4; ++index)
        if (icons == kButtonIcons[index]) options.buttonIcons = index;
    options.showLauncher = Setting(ini, {"launcher"}, {"show"}, "1") != "0";
    options.showInGameFps = Setting(ini, {"quality_of_life", "QualityOfLife"}, {"show_in_game_fps"}, "0") == "1";
    options.skipIntro = Setting(ini, {"startup", "Startup"}, {"skip_intro"}, "0") == "1";
    options.skipDialogue = Setting(ini, {"quality_of_life", "QualityOfLife"}, {"skip_dialogue", "SkipDialogue"}, "0") == "1";
    options.revealFullMap = Setting(ini, {"quality_of_life", "QualityOfLife"}, {"reveal_full_map", "RevealFullMap"}, "0") == "1";
    return options;
}

// Writes what changed, like the Windows launcher's Play button. A setting that cannot be saved is still used for this start.
void SaveOptions(const fs::path& directory, const Options& chosen) {
    const fs::path path = directory / "launcher.ini";
    const Ini ini = ReadIni(path);
    const Options stored = ReadOptions(directory);
    const auto save = [&](const char* section, const char* key, const std::string& value, const char* label) {
        if (!WriteIniValue(path, section, key, value)) Warn(std::string("The ") + label + " setting could not be saved. It is used for this start only.");
    };
    std::string value;
    if (chosen.gameFolder != stored.gameFolder) {
        std::error_code error;
        const bool beside = fs::equivalent(chosen.gameFolder, directory, error);
        save("game", "folder", beside ? "" : chosen.gameFolder.string(), "Game folder");
    }
    if (!FindSetting(ini, {"launcher"}, {"show"}, value) || value != (chosen.showLauncher ? "1" : "0"))
        save("launcher", "show", chosen.showLauncher ? "1" : "0", "Show launcher at startup");
    const char* mode = chosen.borderless ? "borderless" : "windowed";
    if (FindSetting(ini, {"display", "Display"}, {"mode"}, value) ? value != mode : chosen.borderless) save("display", "mode", mode, "Display mode");
    if (chosen.frameRate != stored.frameRate)
        save("display", "frame_rate", chosen.frameRate == 1 ? "120" : chosen.frameRate == 2 ? "unlocked" : "60", "Frame rate");
    if (chosen.buttonIcons != stored.buttonIcons) save("controller", "button_icons", kButtonIcons[chosen.buttonIcons], "Controller Button Icons");
    if (chosen.skipIntro != stored.skipIntro) save("startup", "skip_intro", chosen.skipIntro ? "1" : "0", "Skip intro");
    if (chosen.skipDialogue != stored.skipDialogue) save("quality_of_life", "skip_dialogue", chosen.skipDialogue ? "1" : "0", "Skip dialogue");
    if (chosen.revealFullMap != stored.revealFullMap) save("quality_of_life", "reveal_full_map", chosen.revealFullMap ? "1" : "0", "Reveal full Map");
    if (chosen.showInGameFps != stored.showInGameFps) save("quality_of_life", "show_in_game_fps", chosen.showInGameFps ? "1" : "0", "Show in-game FPS");
    if (chosen.resolution >= 0) {
        if (!WriteIntSetting(directory, kResolutionKey, chosen.resolution)) {
            Warn("The resolution could not be saved. The game starts with its current setting.");
        } else if (Exists(directory / "resolution.txt")) {
            std::error_code error;
            fs::rename(directory / "resolution.txt", directory / "resolution.txt.old", error);
        }
    }
}

// --- settings window --------------------------------------------------------------------------

struct SettingsWindow {
    GtkWidget* window = nullptr;
    GtkWidget* folder = nullptr;
    GtkWidget* folderStatus = nullptr;
    GtkWidget* playButton = nullptr;
    fs::path gameFolder;
    GtkWidget* mode = nullptr;
    GtkWidget* resolution = nullptr;
    GtkWidget* frameRate = nullptr;
    GtkWidget* buttonIcons = nullptr;
    GtkWidget* showLauncher = nullptr;
    GtkWidget* showInGameFps = nullptr;
    GtkWidget* skipIntro = nullptr;
    GtkWidget* skipDialogue = nullptr;
    GtkWidget* revealFullMap = nullptr;
    std::vector<int> resolutionValues;
    Options* chosen = nullptr;
    bool play = false;

    void UpdateFolder() {
        const std::string problem = GameFolderProblem(gameFolder);
        if (problem.empty()) {
            const std::string text = "Found " + GameExecutable(gameFolder).filename().string() + ", sce_sys and Media.";
            gtk_label_set_text(GTK_LABEL(folderStatus), text.c_str());
        } else {
            gtk_label_set_text(GTK_LABEL(folderStatus), problem.c_str());
        }
        GtkStyleContext* style = gtk_widget_get_style_context(folderStatus);
        if (problem.empty()) gtk_style_context_remove_class(style, "error");
        else gtk_style_context_add_class(style, "error");
        gtk_widget_set_sensitive(playButton, problem.empty());
    }

    void Collect() {
        chosen->gameFolder = gameFolder;
        chosen->borderless = gtk_combo_box_get_active(GTK_COMBO_BOX(mode)) == 1;
        chosen->frameRate = std::max(0, gtk_combo_box_get_active(GTK_COMBO_BOX(frameRate)));
        chosen->buttonIcons = std::max(0, gtk_combo_box_get_active(GTK_COMBO_BOX(buttonIcons)));
        chosen->showLauncher = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(showLauncher));
        chosen->showInGameFps = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(showInGameFps));
        const int item = gtk_combo_box_get_active(GTK_COMBO_BOX(resolution));
        chosen->resolution = item >= 0 && item < static_cast<int>(resolutionValues.size()) ? resolutionValues[item] : -1;
    }

    static GtkWidget* Note(const char* text) {
        GtkWidget* label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 52);
        gtk_style_context_add_class(gtk_widget_get_style_context(label), "dim-label");
        return label;
    }

    static GtkWidget* Group(GtkWidget* parent, const char* title) {
        GtkWidget* frame = gtk_frame_new(title);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(box), 12);
        gtk_container_add(GTK_CONTAINER(frame), box);
        gtk_box_pack_start(GTK_BOX(parent), frame, FALSE, FALSE, 0);
        return box;
    }

    static GtkWidget* Row(GtkWidget* grid, int row, const char* label, std::initializer_list<const char*> items, int selected) {
        GtkWidget* text = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
        GtkWidget* combo = gtk_combo_box_text_new();
        for (const char* item : items) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), item);
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), selected);
        gtk_widget_set_hexpand(combo, TRUE);
        gtk_grid_attach(GTK_GRID(grid), text, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), combo, 1, row, 1, 1);
        return combo;
    }

    static GtkWidget* Check(GtkWidget* box, const char* label, bool active, bool available = true) {
        GtkWidget* check = gtk_check_button_new_with_label(label);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), active);
        gtk_widget_set_sensitive(check, available);
        if (!available) gtk_widget_set_tooltip_text(check, "This option uses Windows-only code and is not available on Linux yet.");
        gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);
        return check;
    }

    // Returns true when Play was pressed; options then holds the choices.
    bool Run(const fs::path& directory, Options& options) {
        chosen = &options;
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), kTitle);
        gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
        g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

        GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(outer), 18);
        gtk_container_add(GTK_CONTAINER(window), outer);

        GtkWidget* heading = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(heading), "<span size='x-large' weight='bold'>God of War: Sons of Sparta</span>");
        gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
        gtk_box_pack_start(GTK_BOX(outer), heading, FALSE, FALSE, 0);

        // Game
        GtkWidget* game = Group(outer, "Game");
        GtkWidget* gameGrid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(gameGrid), 12);
        gtk_box_pack_start(GTK_BOX(game), gameGrid, FALSE, FALSE, 0);
        GtkWidget* folderLabel = gtk_label_new("Game folder");
        gtk_label_set_xalign(GTK_LABEL(folderLabel), 0.0f);
        folder = gtk_file_chooser_button_new("Choose the folder of your PS5 game dump", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
        gtk_widget_set_hexpand(folder, TRUE);
        gameFolder = options.gameFolder;
        if (!gameFolder.empty() && fs::is_directory(gameFolder)) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(folder), gameFolder.c_str());
        gtk_grid_attach(GTK_GRID(gameGrid), folderLabel, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(gameGrid), folder, 1, 0, 1, 1);
        folderStatus = Note("");
        gtk_box_pack_start(GTK_BOX(game), folderStatus, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(game), Note("The folder with eboot.bin (or eboot.elf), sce_sys, sce_module and Media from your own decrypted dump. "
                                               "Your files are not changed."), FALSE, FALSE, 0);
        g_signal_connect(folder, "selection-changed", G_CALLBACK(+[](GtkFileChooser* chooser, gpointer self) {
            auto* settings = static_cast<SettingsWindow*>(self);
            gchar* name = gtk_file_chooser_get_filename(chooser);
            settings->gameFolder = name ? fs::path(name) : fs::path();
            g_free(name);
            settings->UpdateFolder();
        }), this);

        // Display
        GtkWidget* display = Group(outer, "Display");
        GtkWidget* grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_box_pack_start(GTK_BOX(display), grid, FALSE, FALSE, 0);
        mode = Row(grid, 0, "Display mode", {"Windowed", "Borderless Fullscreen"}, options.borderless ? 1 : 0);
        resolution = Row(grid, 1, "Resolution", {}, -1);
        frameRate = Row(grid, 2, "Frame rate", {"60 FPS", "120 FPS", "Unlocked (experimental)"}, options.frameRate);
        {
            const int fromFile = ReadResolutionFile(directory);
            const int current = fromFile >= 0 ? fromFile : ReadIntSetting(directory, kResolutionKey);
            int selected = -1;
            for (const auto& option : kResolutions) {
                if (option.index == current) selected = static_cast<int>(resolutionValues.size());
                resolutionValues.push_back(option.index);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(resolution), option.label);
            }
            if (selected < 0) {
                gtk_combo_box_text_prepend_text(GTK_COMBO_BOX_TEXT(resolution), "Keep current setting");
                resolutionValues.insert(resolutionValues.begin(), -1);
                selected = 0;
            }
            gtk_combo_box_set_active(GTK_COMBO_BOX(resolution), selected);
        }
        gtk_box_pack_start(GTK_BOX(display), Note("Lower resolutions improve GPU performance."), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(display), Note("F11 switches between Windowed and Borderless Fullscreen while the game is running."), FALSE, FALSE, 0);

        // Controller
        GtkWidget* controller = Group(outer, "Controller");
        GtkWidget* controllerGrid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(controllerGrid), 12);
        gtk_box_pack_start(GTK_BOX(controller), controllerGrid, FALSE, FALSE, 0);
        buttonIcons = Row(controllerGrid, 0, "Controller Button Icons", {"Automatic (Default)", "PlayStation", "Xbox", "Nintendo Switch"}, options.buttonIcons);
        gtk_box_pack_start(GTK_BOX(controller), Note("Automatic follows the controller in use. The Steam Deck uses the Xbox icons, the same letters as its buttons."), FALSE, FALSE, 0);

        // Launcher
        GtkWidget* launcher = Group(outer, "Launcher");
        showLauncher = Check(launcher, "Show launcher at startup", options.showLauncher);
        gtk_box_pack_start(GTK_BOX(launcher), Note("When disabled, the game starts directly.\nTo show this launcher again, edit launcher.ini and set [launcher] show=1, "
                                                   "or start it with --settings."), FALSE, FALSE, 0);

        // Quality of Life
        GtkWidget* quality = Group(outer, "Quality of Life");
        showInGameFps = Check(quality, "Show in-game FPS (F9)", options.showInGameFps);
        skipIntro = Check(quality, "Skip intro (Windows only for now)", options.skipIntro, false);
        skipDialogue = Check(quality, "Skip dialogue (Windows only for now)", options.skipDialogue, false);
        revealFullMap = Check(quality, "Reveal full Map (Windows only for now)", options.revealFullMap, false);

        // Buttons
        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
        playButton = gtk_button_new_with_mnemonic("_Play");
        GtkWidget* exitButton = gtk_button_new_with_mnemonic("E_xit");
        gtk_widget_set_size_request(playButton, 110, -1);
        gtk_widget_set_size_request(exitButton, 110, -1);
        gtk_style_context_add_class(gtk_widget_get_style_context(playButton), "suggested-action");
        gtk_box_pack_start(GTK_BOX(buttons), playButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(buttons), exitButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(outer), buttons, FALSE, FALSE, 4);
        g_signal_connect(playButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer self) {
            auto* settings = static_cast<SettingsWindow*>(self);
            settings->Collect();
            settings->play = true;
            gtk_widget_destroy(settings->window);
        }), this);
        g_signal_connect_swapped(exitButton, "clicked", G_CALLBACK(gtk_widget_destroy), window);
        // Escape closes the window like Exit; Enter on a focused control presses Play.
        g_signal_connect(window, "key-press-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventKey* event, gpointer) -> gboolean {
            if (event->keyval == GDK_KEY_Escape) { gtk_widget_destroy(widget); return TRUE; }
            return FALSE;
        }), nullptr);
        gtk_widget_set_can_default(playButton, TRUE);
        gtk_window_set_default(GTK_WINDOW(window), playButton);

        gtk_widget_show_all(window);
        UpdateFolder();
        gtk_widget_grab_focus(gtk_widget_get_sensitive(playButton) ? playButton : folder);
        gtk_main();
        while (gtk_events_pending()) gtk_main_iteration();
        return play;
    }
};

// --- first-start progress window --------------------------------------------------------------

struct Progress {
    std::mutex lock;
    std::string status = "Unpacking the game files...";
    std::atomic<bool> done{false};
};

// Runs work on a worker thread; with a display, a small window explains the wait like the Windows splash.
template <class Work>
void RunWithProgress(Progress& progress, Work work) {
    std::thread worker([&] {
        work();
        progress.done.store(true);
    });
    if (gui) {
        GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), kTitle);
        gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
        gtk_window_set_deletable(GTK_WINDOW(window), FALSE);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(box), 20);
        gtk_container_add(GTK_CONTAINER(window), box);
        GtkWidget* title = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>Preparing the game</span>");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        GtkWidget* intro = gtk_label_new("The first start prepares the game from your files. This takes about two minutes.\nLater starts are fast. Your own files are not changed.");
        gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
        GtkWidget* status = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
        GtkWidget* bar = gtk_progress_bar_new();
        GtkWidget* elapsed = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(elapsed), 0.0f);
        gtk_style_context_add_class(gtk_widget_get_style_context(elapsed), "dim-label");
        for (GtkWidget* widget : {title, intro, status, bar, elapsed}) gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
        gtk_widget_show_all(window);

        struct Tick { Progress* progress; GtkWidget* status; GtkWidget* bar; GtkWidget* elapsed; std::chrono::steady_clock::time_point started; } tick{
            &progress, status, bar, elapsed, std::chrono::steady_clock::now()};
        const guint timer = g_timeout_add(100, +[](gpointer data) -> gboolean {
            auto* tick = static_cast<Tick*>(data);
            {
                std::lock_guard guard(tick->progress->lock);
                gtk_label_set_text(GTK_LABEL(tick->status), tick->progress->status.c_str());
            }
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(tick->bar));
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - tick->started).count();
            const std::string text = "Elapsed: " + std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
            gtk_label_set_text(GTK_LABEL(tick->elapsed), text.c_str());
            if (tick->progress->done.load()) { gtk_main_quit(); return G_SOURCE_REMOVE; }
            return G_SOURCE_CONTINUE;
        }, &tick);
        gtk_main();
        if (!progress.done.load()) g_source_remove(timer);
        gtk_widget_destroy(window);
        while (gtk_events_pending()) gtk_main_iteration();
    }
    worker.join();
}

void Usage() {
    std::cout << "Usage: SonsOfSparta-PS5 [options]\n"
                 "  --settings                  show the settings window even if it is turned off\n"
                 "  --play                      start directly with the saved settings\n"
                 "  --game-folder <folder>      the folder of your PS5 game dump\n"
                 "  --windowed | --borderless   display mode\n"
                 "  --fps 60|120|unlocked       frame rate\n"
                 "  --show-fps                  draw the frame rate in the game (F9 toggles)\n"
                 "  --prepare                   only prepare the game files, then exit\n"
                 "  --reprepare                 rebuild the prepared files even if they are current\n"
                 "Settings are kept in launcher.ini (same format as the Windows launcher).\n"
                 "Extra environment variables can be listed in debug_env.txt (NAME=value per line).\n";
}

}

// A Steam Deck (LCD: Jupiter, OLED: Galileo), or the Deck build of the AppImage (SOS_STEAMDECK=1).
bool OnSteamDeck() {
    const char* forced = std::getenv("SOS_STEAMDECK");
    if (forced != nullptr) return std::strcmp(forced, "0") != 0;
    const char* steam = std::getenv("SteamDeck");
    if (steam != nullptr && std::strcmp(steam, "1") == 0) return true;
    std::ifstream board("/sys/class/dmi/id/board_name");
    std::string name;
    std::getline(board, name);
    return name == "Jupiter" || name == "Galileo";
}

int main(int argc, char** argv) {
    std::error_code error;
    const fs::path self = fs::read_symlink("/proc/self/exe", error);
    const fs::path directory = error ? fs::current_path() : self.parent_path();
    fs::current_path(directory, error);

    Options options = ReadOptions(directory);
    bool forceSettings = false, skipSettings = false, prepareOnly = false, forcePrepare = false, overridden = false;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--help" || arg == "-h") { Usage(); return 0; }
        else if (arg == "--settings") forceSettings = true;
        else if (arg == "--play") skipSettings = true;
        else if (arg == "--windowed") { options.borderless = false; overridden = true; }
        else if (arg == "--borderless" || arg == "--fullscreen") { options.borderless = true; overridden = true; }
        else if (arg == "--show-fps") { options.showInGameFps = true; overridden = true; }
        else if (arg == "--prepare") prepareOnly = true;
        else if (arg == "--reprepare") forcePrepare = true;
        else if (arg == "--game-folder" && index + 1 < argc) {
            options.gameFolder = fs::absolute(argv[++index], error);
            overridden = true;
        }
        else if (arg == "--fps" && index + 1 < argc) {
            const std::string value = Lower(argv[++index]);
            options.frameRate = value == "120" ? 1 : value == "unlocked" ? 2 : 0;
            overridden = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            Usage();
            return 2;
        }
    }
    gui = gtk_init_check(&argc, &argv) == TRUE;

    // One instance at a time.
    const fs::path lockPath = directory / ".launcher.lock";
    const int lock = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
        Fail("The game is already running.");
        return 0;
    }

    const fs::path relinker = directory / "tools" / "relinker";
    if (!fs::is_directory(directory / "libs", error) || !Exists(relinker)) {
        Fail("Program files are missing (libs and tools folders). Please extract the whole download again.");
        return 1;
    }
    fs::create_directories(directory / "logs", error);

    // Steam Deck defaults, once, so later changes in the settings window stay: fullscreen, 1920x1080 (the smallest size
    // the game offers), 60 FPS, straight into the game. The menu and Steam controls work without the settings window.
    if (OnSteamDeck() && Setting(ReadIni(directory / "launcher.ini"), {"steam_deck"}, {"applied"}, "0") != "1" && !overridden) {
        options.borderless = true;
        options.frameRate = 0;
        options.showLauncher = false;
        options.resolution = 8;
        if (GameFolderProblem(options.gameFolder).empty()) {
            SaveOptions(directory, options);
            WriteIniValue(directory / "launcher.ini", "steam_deck", "applied", "1");
        }
    }

    // The settings window, like the Windows launcher. It also opens when no usable game folder is set, so the
    // player can choose one. Command-line choices start directly unless --settings is given.
    const bool folderOk = GameFolderProblem(options.gameFolder).empty();
    const bool showSettings = gui && !skipSettings && (forceSettings || !folderOk || (!prepareOnly && options.showLauncher && !overridden));
    if (showSettings) {
        const fs::path icon = options.gameFolder / "sce_sys" / "icon0.png";
        if (Exists(icon)) gtk_window_set_default_icon_from_file(icon.c_str(), nullptr);
        SettingsWindow window;
        if (!window.Run(directory, options)) return 0;
        SaveOptions(directory, options);
    }
    fs::path gameFolder = fs::weakly_canonical(options.gameFolder, error);
    if (error) gameFolder = options.gameFolder;
    if (const std::string problem = GameFolderProblem(gameFolder); !problem.empty()) {
        Fail(std::string("The game files were not found.\n\n") + problem + "\n\nFolder: " + gameFolder.string() +
             "\n\nStart the launcher and choose the game folder, or pass --game-folder <folder>.");
        return 1;
    }

    if (!showSettings) {
        // Remember a folder given with --game-folder, so the next start finds it too.
        const std::string saved = Setting(ReadIni(directory / "launcher.ini"), {"game"}, {"folder"}, "");
        std::error_code ignored;
        const bool beside = fs::equivalent(gameFolder, directory, ignored);
        if (!beside && saved != gameFolder.string()) WriteIniValue(directory / "launcher.ini", "game", "folder", gameFolder.string());
    }

    const fs::path prep = directory / "prep";
    const fs::path elf = prep / "eboot.elf";
    const fs::path bin = GameExecutable(gameFolder);
    fs::create_directories(prep, error);
    {
        // Rebuilt when the source is newer or comes from another folder.
        const fs::path sourceFile = prep / "eboot.source";
        const bool stale = !Exists(elf) || ReadStamp(sourceFile) != bin.string() || fs::last_write_time(elf, error) < fs::last_write_time(bin, error);
        if (stale) {
            if (ConvertOrCopy(bin, elf)) WriteStamp(sourceFile, bin.string());
            else fs::remove(elf, error);
        }
    }
    const fs::path executable = elf;
    if (!Exists(elf) || !IsElf(elf)) {
        Fail("The game executable could not be unpacked. Please provide a decrypted eboot.bin or eboot.elf from your own copy of the game.");
        return 1;
    }

    const fs::path runtime = directory / kRuntime;
    const fs::path stampFile = prep / "prepared.stamp";
    const std::string stampNow = "linux " + std::to_string(kPrepareVersion) + " " + std::to_string(FileSize(executable)) + " " + std::to_string(FileSize(relinker)) + " " +
                                 std::to_string(fs::last_write_time(relinker, error).time_since_epoch().count()) + " " + gameFolder.string();
    if (forcePrepare || !Exists(runtime) || ReadStamp(stampFile) != stampNow) {
        Status("First start: preparing the game from your files. This takes about two minutes...");
        Progress progress;
        std::string failed;
        int code = 0;
        const auto started = std::chrono::steady_clock::now();
        RunWithProgress(progress, [&] {
            std::error_code ignored;
            fs::remove_all(prep / "sce_module", ignored);  // modules of a previously chosen folder
            PrepareModules(gameFolder, prep, failed);
            if (!failed.empty()) return;
            {
                std::lock_guard guard(progress.lock);
                progress.status = "Building the game program...";
            }
            fs::remove(runtime, ignored);
            fs::remove(directory / (std::string(kRuntime) + ".registry.json"), ignored);
            code = Run({relinker.string(), "--registry", "--skip-syscall-check",
                        "--exclude-sce-module", "CommonDialog.prx", "--exclude-sce-module", "libSceNpCppWebApi.prx",
                        "--defer-sce-module", "PSN.prx", "--defer-sce-module", "SaveData.prx",
                        "--defer-sce-module", "VoiceInputPlugin.prx", "--defer-sce-module", "lib_burst_generated.prx",
                        executable.string(), runtime.string()},
                       directory / "logs" / "prepare.log");
        });
        if (!failed.empty()) {
            Fail("Some game modules could not be converted (" + failed + ").\n\nThey must be decrypted files from your own copy of the game.");
            return 1;
        }
        if (code != 0 || !Exists(runtime)) {
            Fail("The game could not be prepared from your files (exit code " + std::to_string(code) +
                 ").\n\nSee logs/prepare.log. Make sure eboot.elf is the decrypted executable of " + kName + " (" + kTitleId + ").");
            return 1;
        }
        fs::permissions(runtime, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, error);
        WriteStamp(stampFile, stampNow);
        Status("Prepared in " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count()) + " s.");
    }
    for (const char* name : {"Media", "sce_sys"}) LinkIntoApp0(directory, gameFolder, name);

    // PS5 functions the game imports but no library provides return 0, as on Windows (see MissingImportStubs.hpp).
    const fs::path stubs = prep / "libsos_missing_imports.so";
    {
        const auto written = MissingImportStubs::Write(runtime, directory / "libs", stubs);
        if (!written.ok) {
            Fail("The game could not be prepared: " + written.error + "\n\nTry starting again with --reprepare.");
            return 1;
        }
        std::ofstream log(directory / "logs" / "missing-imports.txt", std::ios::trunc);
        log << written.stubCount << " imported PS5 functions have no implementation and return 0:\n";
        for (const auto& line : written.missing) log << line << "\n";
        Status(std::to_string(written.stubCount) + " unimplemented PS5 functions are stubbed (logs/missing-imports.txt).");
    }
    if (prepareOnly) {
        Status("Game files are prepared.");
        return 0;
    }

    // Extra variables for troubleshooting, one NAME=value per line.
    {
        std::ifstream debug(directory / "debug_env.txt");
        std::string line;
        while (std::getline(debug, line)) {
            line = Trim(line);
            const auto equals = line.find('=');
            if (line.empty() || line[0] == '#' || equals == std::string::npos || equals == 0) continue;
            setenv(line.substr(0, equals).c_str(), line.substr(equals + 1).c_str(), 1);
        }
    }
    // The runtime reads these from its environment; an option that is off is unset or 0.
    const auto set = [](const char* name, const char* value) { value ? setenv(name, value, 1) : unsetenv(name); };
    set("SOS_START_BORDERLESS", options.borderless ? "1" : "0");
    set("SOS_SKIP_INTRO", options.skipIntro ? "1" : nullptr);
    set("SOS_SKIP_DIALOGUE", options.skipDialogue ? "1" : nullptr);
    set("SOS_REVEAL_FULL_MAP", options.revealFullMap ? "1" : nullptr);
    set("APS5_VBLANK_HZ", options.frameRate >= 1 ? "1000" : nullptr);
    set("APS5_FPS_CAP", options.frameRate == 1 ? "120" : nullptr);
    set("SOS_SHOW_IN_GAME_FPS", options.showInGameFps ? "1" : nullptr);
    set("SOS_CONTROLLER_BUTTON_ICONS", options.buttonIcons != 0 ? kButtonIcons[options.buttonIcons] : nullptr);
    {
        const char* preload = std::getenv("LD_PRELOAD");
        const std::string value = stubs.string() + (preload && *preload ? std::string(":") + preload : std::string());
        setenv("LD_PRELOAD", value.c_str(), 1);
    }
    // SDL2 is built for X11 only; on a Wayland desktop the game runs through XWayland.
    if (!std::getenv("SDL_VIDEODRIVER")) setenv("SDL_VIDEODRIVER", "x11", 1);

    Status("Starting the game. Logs: logs/game.log and logs/game.err");
    const auto started = std::chrono::steady_clock::now();
    const int code = Run({runtime.string()}, directory / "logs" / "game.log", directory / "logs" / "game.err");
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
    {
        std::ofstream journal(directory / "logs" / "launcher.log", std::ios::app);
        journal << "ended, exit code " << code << " after " << seconds << " s\n";
    }
    if (code != 0) {
        fs::copy_file(directory / "logs" / "game.err", directory / "logs" / "game.err.last", fs::copy_options::overwrite_existing, error);
        if (seconds < 60) Fail("The game stopped unexpectedly (code " + std::to_string(code) + ").\n\nDetails are in logs/game.err.");
    }
    return code;
}
