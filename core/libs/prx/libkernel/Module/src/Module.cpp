#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fstream>
#include <link.h>
#endif

extern "C" {
void* APS5_VABI dlopen_nid_postfix(const char* path, int flags);
#ifndef _WIN32
void BeginModuleStart(std::uint64_t args, const void* argp);
int EndModuleStart();
#endif
void* APS5_VABI dlsym_nid_postfix(void* handle, const char* name);
int APS5_VABI dlclose_nid_postfix(void* handle);
}

namespace {
constexpr int kRtldNow = 2;
}

extern "C" {

int APS5_VABI sceKernelDlsym(KernelModule handle, const char* symbol, void** addr) {
 if (!symbol || !addr) return SCE_KERNEL_ERROR_EFAULT;
 void* found = dlsym_nid_postfix(reinterpret_cast<void*>(static_cast<intptr_t>(handle)), symbol);
 if (!found) return SCE_KERNEL_ERROR_ESRCH;
 *addr = found;
 return 0;
}

int APS5_VABI sceKernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info) {
  (void)flags;
  if (!info) return SCE_KERNEL_ERROR_EFAULT;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return SCE_KERNEL_ERROR_ESRCH;
  info->st_size = sizeof(ModuleInfoForUnwind);
  info->eh_frame_hdr_addr = 0;
  info->eh_frame_addr = 0;
  info->eh_frame_size = 0;
  info->seg0_addr = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
  info->seg0_size = mbi.RegionSize;
  char path[4096] = {};
  DWORD len = GetMappedFileNameA(GetCurrentProcess(), mbi.BaseAddress, path, sizeof(path) - 1);
  path[len] = '\0';
  std::strncpy(info->name, path, sizeof(info->name) - 1);
  info->name[sizeof(info->name) - 1] = '\0';
  return 0;
#else
  // The title's own libc (its C++ runtime and unwinder) finds each module's unwind tables through this call.
  // Without eh_frame_hdr every exception was "uncaught", IL2CPP's managed exceptions included. Filled from the
  // loaded module's PT_GNU_EH_FRAME, as upstream AnyPS5 does.
  struct Search {
    std::uint64_t address;
    ModuleInfoForUnwind* info;
    bool found;
  } search{addr, info, false};
  dl_iterate_phdr([](dl_phdr_info* image, std::size_t, void* data) {
    auto& search = *static_cast<Search*>(data);
    const ElfW(Phdr)* code = nullptr;
    const ElfW(Phdr)* first = nullptr;
    const ElfW(Phdr)* frames = nullptr;
    bool contains = false;
    for (std::uint16_t index = 0; index < image->dlpi_phnum; ++index) {
      const auto& header = image->dlpi_phdr[index];
      const std::uint64_t start = image->dlpi_addr + header.p_vaddr;
      if (header.p_type == PT_LOAD) {
        if (first == nullptr) first = &header;
        if (code == nullptr && (header.p_flags & PF_X) != 0) code = &header;
        if (search.address >= start && search.address - start < header.p_memsz) contains = true;
      }
      if (header.p_type == PT_GNU_EH_FRAME) frames = &header;
    }
    if (!contains) return 0;
    const auto loaded = [image](std::uint64_t begin, std::uint64_t size) {
      for (std::uint16_t index = 0; index < image->dlpi_phnum; ++index) {
        const auto& header = image->dlpi_phdr[index];
        const std::uint64_t start = image->dlpi_addr + header.p_vaddr;
        if (header.p_type == PT_LOAD && begin >= start && begin - start <= header.p_memsz && size <= header.p_memsz - (begin - start)) return true;
      }
      return false;
    };
    auto* info = search.info;
    info->st_size = sizeof(ModuleInfoForUnwind);
    std::strncpy(info->name, image->dlpi_name != nullptr ? image->dlpi_name : "", sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->eh_frame_hdr_addr = 0;
    info->eh_frame_addr = 0;
    info->eh_frame_size = 0;
    if (frames != nullptr) {
      const std::uint64_t header = image->dlpi_addr + frames->p_vaddr;
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(header);
      // version 1, then the eh_frame_ptr encoding; the pointer follows the four header bytes.
      if (loaded(header, 8) && bytes[0] == 1) {
        const std::uint8_t encoding = bytes[1];
        const std::uint64_t field = header + 4;
        std::uint64_t value = 0;
        bool known = true;
        switch (encoding & 0x0f) {
          case 0x0b: { std::int32_t v; std::memcpy(&v, bytes + 4, 4); value = static_cast<std::uint64_t>(static_cast<std::int64_t>(v)); break; }
          case 0x03: { std::uint32_t v; std::memcpy(&v, bytes + 4, 4); value = v; break; }
          case 0x04: case 0x0c: { std::memcpy(&value, bytes + 4, 8); break; }
          default: known = false;
        }
        if (known && (encoding & 0x70) == 0x10) value += field;        // pcrel
        else if (known && (encoding & 0x70) == 0x30) value += header;  // datarel
        if (known && loaded(value, 4)) {
          info->eh_frame_hdr_addr = header;
          info->eh_frame_addr = value;
          // Walk the CIE/FDE records to the zero terminator, staying inside the loaded segment.
          std::uint64_t cursor = value;
          while (loaded(cursor, 4)) {
            std::uint32_t length = 0;
            std::memcpy(&length, reinterpret_cast<const void*>(cursor), 4);
            if (length == 0) { cursor += 4; break; }
            std::uint64_t next = cursor + 4 + length;
            if (length == 0xffffffffu) {
              if (!loaded(cursor, 12)) break;
              std::uint64_t extended = 0;
              std::memcpy(&extended, reinterpret_cast<const void*>(cursor + 4), 8);
              next = cursor + 12 + extended;
            }
            if (next <= cursor || !loaded(cursor, next - cursor)) break;
            cursor = next;
          }
          info->eh_frame_size = cursor - value;
        }
      }
    }
    const auto* segment = code != nullptr ? code : first;
    info->seg0_addr = image->dlpi_addr + segment->p_vaddr;
    info->seg0_size = segment->p_memsz;
    search.found = true;
    return 1;
  }, &search);
  if (search.found) return 0;
  return SCE_KERNEL_ERROR_ESRCH;
#endif
}

int APS5_VABI sceKernelGetModuleInfoFromAddr(uint64_t addr, int n, ModuleInfo* r) {
 (void)addr;
 (void)n;
 (void)r;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

KernelModule APS5_VABI sceKernelLoadStartModule(const char* module_file_name, size_t args, const void* argp, uint32_t flags, const KernelLoadModuleOpt* opt, int* res) {
 (void)flags;
 (void)opt;
 if (res) *res = 0;
 if (!module_file_name) return static_cast<KernelModule>(SCE_KERNEL_ERROR_EFAULT);
#ifdef _WIN32
 (void)args;
 (void)argp;
 void* handle = dlopen_nid_postfix(module_file_name, kRtldNow);
#else
 // A deferred module's start function receives these arguments (DynamicLoader.cpp) and its result goes to res.
 BeginModuleStart(args, argp);
 void* handle = dlopen_nid_postfix(module_file_name, kRtldNow);
 const int started = EndModuleStart();
 if (res) *res = started;
#endif
 if (!handle) return static_cast<KernelModule>(SCE_KERNEL_ERROR_ENOENT);
 return static_cast<KernelModule>(reinterpret_cast<intptr_t>(handle));
}

int APS5_VABI sceKernelStopUnloadModule(KernelModule handle, size_t args, const void* argp, uint32_t flags, const KernelUnloadModuleOpt* opt, int* res) {
 (void)args;
 (void)argp;
 (void)flags;
 (void)opt;
 if (res) *res = 0;
 return dlclose_nid_postfix(reinterpret_cast<void*>(static_cast<intptr_t>(handle))) == 0 ? 0 : SCE_KERNEL_ERROR_ESRCH;
}

}

extern "C" {

int APS5_VABI __elf_phdr_match_addr_nid_postfix(ModuleInfo* module, std::uint64_t address) {
    (void)module;
    (void)address;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

std::int32_t APS5_VABI sceKernelInternalMemoryGetModuleSegmentInfo_nid_postfix(void* result) {
    (void)result;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
