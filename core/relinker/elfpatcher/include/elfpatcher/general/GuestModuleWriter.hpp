#ifndef ELFPATCHER_GUESTMODULEWRITER_HPP
#define ELFPATCHER_GUESTMODULEWRITER_HPP

#include <relinker/guest/GuestImage.hpp>

namespace Elfpatcher {

class GuestModuleWriter {
public:
    // A deferred module gets no DT_INIT/DT_FINI: its initializer is recorded under DeferredInitTag and
    // run by the runtime's dlopen the first time the game loads the module, as on Windows.
    std::vector<std::uint8_t> WriteLinux(const Relinker::GuestImage& image, const std::vector<std::string>& dependencies, const std::string& runPath, bool deferInit = false) const;
    std::vector<std::uint8_t> WriteWindows(const Relinker::GuestImage& image, Domain::GuestRuntime& runtime) const;
    // OS-specific dynamic tag (DT_LOOS range) that holds a deferred module's initializer address.
    static constexpr std::uint64_t DeferredInitTag = 0x6a505335;
};

}

#endif
