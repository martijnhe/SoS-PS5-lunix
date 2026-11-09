#ifndef CORE_LIBS_PRX_LIBSCEPAD_PADCONTROLLERCHANNEL_HPP
#define CORE_LIBS_PRX_LIBSCEPAD_PADCONTROLLERCHANNEL_HPP

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <atomic>
#include <cstdint>
#include <string>
#include "prx/libScePad/include/PadControllerKind.hpp"

// The physical controller kind is written by libScePad under its state lock and read by libSceSystemService hooks on the game thread,
// through process-local shared memory so neither library links to the other.
namespace PadControllerChannel {

struct Shared {
    std::uint32_t kind;
    std::uint32_t icons;  // the in-game menu's button icons + 1, 0 until it has been read
};

inline Shared* View() {
    static Shared* const view = [] {
#ifndef _WIN32
        // POSIX shared memory named after the process, so both PRXs map the same page; removed again at exit.
        const std::string name = "/SoS-PadControllerKind-" + std::to_string(static_cast<long>(getpid()));
        const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT, 0600);
        if (fd < 0) return static_cast<Shared*>(nullptr);
        Shared* mapped = nullptr;
        if (ftruncate(fd, sizeof(Shared)) == 0) {
            void* memory = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (memory != MAP_FAILED) mapped = static_cast<Shared*>(memory);
        }
        close(fd);
        static const struct Cleanup { std::string name; ~Cleanup() { shm_unlink(name.c_str()); } } cleanup{name};
        return mapped;
#else
        const std::wstring name = L"Local\\SoS-PadControllerKind-" + std::to_wstring(GetCurrentProcessId());
        const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
        return mapping != nullptr ? static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
#endif
    }();
    return view;
}

inline void Publish(PadControllerKind kind) {
    if (Shared* shared = View()) std::atomic_ref(shared->kind).store(static_cast<std::uint32_t>(kind), std::memory_order_release);
}

inline void PublishIcons(std::uint32_t icons) {
    if (Shared* shared = View()) std::atomic_ref(shared->icons).store(icons, std::memory_order_release);
}

inline std::uint32_t IconsOverride() {
    Shared* const shared = View();
    return shared != nullptr ? std::atomic_ref(shared->icons).load(std::memory_order_acquire) : 0;
}

inline PadControllerKind Current() {
    Shared* const shared = View();
    return shared != nullptr ? static_cast<PadControllerKind>(std::atomic_ref(shared->kind).load(std::memory_order_acquire)) : PadControllerKind::None;
}

}

#endif
