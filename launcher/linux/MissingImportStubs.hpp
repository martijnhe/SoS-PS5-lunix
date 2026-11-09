#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Linux counterpart of the Windows relinker's "soft stubs": on Windows every guest import that no
// host library exports is bound to a function that returns 0. On Linux the dynamic loader binds the
// imports itself and refuses to start when one is missing, so the launcher writes a small shared
// library that defines each missing import (functions return 0, data is zero-filled) and preloads it.
namespace MissingImportStubs {

struct Result {
    bool ok = false;
    std::string error;
    std::vector<std::string> missing;  // "<module> <symbol>" for the log
    std::size_t stubCount = 0;
};

// Follows the runtime's dependencies (guest modules and host libraries in libsDirectory), then writes the
// stub library to stubPath. When nothing is missing it is still written (empty), so it can always be preloaded.
Result Write(const std::filesystem::path& runtime, const std::filesystem::path& libsDirectory, const std::filesystem::path& stubPath);

}
