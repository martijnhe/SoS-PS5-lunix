#pragma once

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

// Triangle state shared by libScePad and libSceAudioIn through process-local shared memory; the two PRXs do not link to each other.
namespace BlowInput {

struct Flags {
    std::uint32_t active;
};

inline Flags* Shared() {
    static Flags* const flags = [] {
#ifdef _WIN32
        const std::wstring name = L"Local\\SoS-BlowInput-" + std::to_wstring(GetCurrentProcessId());
        const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Flags), name.c_str());
        return mapping != nullptr ? static_cast<Flags*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Flags))) : nullptr;
#else
        // POSIX shared memory named after the process, so both PRXs map the same page.
        // The name is removed again at exit; the mappings stay valid until then.
        const std::string name = "/SoS-BlowInput-" + std::to_string(static_cast<long>(getpid()));
        const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT, 0600);
        if (fd < 0) return static_cast<Flags*>(nullptr);
        Flags* mapped = nullptr;
        if (ftruncate(fd, sizeof(Flags)) == 0) {
            void* view = mmap(nullptr, sizeof(Flags), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (view != MAP_FAILED) mapped = static_cast<Flags*>(view);
        }
        close(fd);
        static const struct Cleanup { std::string name; ~Cleanup() { shm_unlink(name.c_str()); } } cleanup{name};
        return mapped;
#endif
    }();
    return flags;
}

inline void SetActive(bool active) {
    if (Flags* const flags = Shared()) std::atomic_ref(flags->active).store(active ? 1 : 0);
}

inline bool IsActive() {
    Flags* const flags = Shared();
    return flags != nullptr && std::atomic_ref(flags->active).load() != 0;
}

}
