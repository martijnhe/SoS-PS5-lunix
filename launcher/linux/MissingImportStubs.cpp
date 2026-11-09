#include "MissingImportStubs.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>

#include <dlfcn.h>
#include <elf.h>

namespace fs = std::filesystem;

namespace MissingImportStubs {

namespace {

struct Symbol {
    std::string name;
    bool defined = false;
    bool weak = false;
    bool object = false;
};

struct Image {
    std::vector<Symbol> symbols;
    std::vector<std::string> needed;  // DT_NEEDED entries, in order
};

std::optional<std::vector<std::uint8_t>> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

template <class T>
bool ReadAt(const std::vector<std::uint8_t>& data, std::uint64_t offset, T& value) {
    if (offset > data.size() || data.size() - offset < sizeof(T)) return false;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return true;
}

// Reads the dynamic symbol table and dependencies through the program headers, so it also works for the
// relinker's guest modules, which carry no section headers.
std::optional<Image> ReadImage(const fs::path& path) {
    const auto file = ReadFile(path);
    if (!file) return std::nullopt;
    const auto& data = *file;
    Elf64_Ehdr header{};
    if (!ReadAt(data, 0, header) || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64) return std::nullopt;
    std::vector<Elf64_Phdr> loads;
    std::optional<Elf64_Phdr> dynamic;
    for (unsigned index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr program{};
        if (!ReadAt(data, header.e_phoff + index * sizeof(Elf64_Phdr), program)) return std::nullopt;
        if (program.p_type == PT_LOAD) loads.push_back(program);
        if (program.p_type == PT_DYNAMIC) dynamic = program;
    }
    if (!dynamic) return Image{};
    const auto toOffset = [&](std::uint64_t address) -> std::optional<std::uint64_t> {
        for (const auto& load : loads)
            if (address >= load.p_vaddr && address < load.p_vaddr + load.p_filesz) return address - load.p_vaddr + load.p_offset;
        return std::nullopt;
    };
    std::map<std::int64_t, std::uint64_t> tags;
    std::vector<std::uint64_t> neededOffsets;
    for (std::uint64_t offset = dynamic->p_offset; offset + sizeof(Elf64_Dyn) <= dynamic->p_offset + dynamic->p_filesz; offset += sizeof(Elf64_Dyn)) {
        Elf64_Dyn entry{};
        if (!ReadAt(data, offset, entry) || entry.d_tag == DT_NULL) break;
        if (entry.d_tag == DT_NEEDED) neededOffsets.push_back(entry.d_un.d_val);
        tags.emplace(entry.d_tag, entry.d_un.d_val);
    }
    if (!tags.contains(DT_SYMTAB) || !tags.contains(DT_STRTAB)) return Image{};
    const auto symbols = toOffset(tags[DT_SYMTAB]);
    const auto strings = toOffset(tags[DT_STRTAB]);
    if (!symbols || !strings) return std::nullopt;
    const auto readString = [&](std::uint64_t offset) {
        const std::uint64_t at = *strings + offset;
        if (at >= data.size()) return std::string();
        const auto* begin = reinterpret_cast<const char*>(data.data() + at);
        return std::string(begin, strnlen(begin, data.size() - at));
    };
    Image image;
    for (const auto offset : neededOffsets) image.needed.push_back(readString(offset));

    // Symbol count from the .dynsym section header when there is one, else DT_HASH, else DT_GNU_HASH,
    // else up to the string table.
    std::uint64_t count = 0;
    for (unsigned index = 0; header.e_shoff != 0 && index < header.e_shnum; ++index) {
        Elf64_Shdr section{};
        if (ReadAt(data, header.e_shoff + index * sizeof(Elf64_Shdr), section) && section.sh_type == SHT_DYNSYM && section.sh_entsize == sizeof(Elf64_Sym)) {
            count = section.sh_size / sizeof(Elf64_Sym);
            break;
        }
    }
    if (count != 0) {
    } else if (const auto hash = tags.contains(DT_HASH) ? toOffset(tags[DT_HASH]) : std::nullopt) {
        std::uint32_t chains = 0;
        ReadAt(data, *hash + 4, chains);
        count = chains;
    } else if (const auto gnu = tags.contains(DT_GNU_HASH) ? toOffset(tags[DT_GNU_HASH]) : std::nullopt) {
        std::uint32_t buckets = 0, base = 0, bloomWords = 0;
        ReadAt(data, *gnu, buckets);
        ReadAt(data, *gnu + 4, base);
        ReadAt(data, *gnu + 8, bloomWords);
        const std::uint64_t bucketTable = *gnu + 16 + std::uint64_t{bloomWords} * 8;
        std::uint32_t last = 0;
        for (std::uint32_t index = 0; index < buckets; ++index) {
            std::uint32_t value = 0;
            ReadAt(data, bucketTable + index * 4, value);
            last = std::max(last, value);
        }
        count = base;
        if (last >= base) {
            const std::uint64_t chainTable = bucketTable + std::uint64_t{buckets} * 4;
            for (std::uint32_t index = last;; ++index) {
                std::uint32_t value = 0;
                if (!ReadAt(data, chainTable + (index - base) * 4, value)) break;
                if (value & 1) { count = index + 1; break; }
            }
        }
    } else if (*strings > *symbols) {
        count = (*strings - *symbols) / sizeof(Elf64_Sym);
    }

    std::vector<Symbol>& result = image.symbols;
    for (std::uint64_t index = 1; index < count; ++index) {
        Elf64_Sym symbol{};
        if (!ReadAt(data, *symbols + index * sizeof(Elf64_Sym), symbol)) break;
        const std::uint64_t nameOffset = *strings + symbol.st_name;
        if (symbol.st_name == 0 || nameOffset >= data.size()) continue;
        Symbol entry;
        entry.name = readString(symbol.st_name);
        entry.defined = symbol.st_shndx != SHN_UNDEF;
        entry.weak = ELF64_ST_BIND(symbol.st_info) == STB_WEAK;
        entry.object = ELF64_ST_TYPE(symbol.st_info) == STT_OBJECT || ELF64_ST_TYPE(symbol.st_info) == STT_TLS;
        result.push_back(std::move(entry));
    }
    return image;
}

std::uint32_t ElfHash(const std::string& name) {
    std::uint32_t hash = 0;
    for (const unsigned char character : name) {
        hash = (hash << 4) + character;
        const std::uint32_t high = hash & 0xf0000000u;
        if (high) hash ^= high >> 24;
        hash &= ~high;
    }
    return hash;
}

template <class T>
void Append(std::vector<std::uint8_t>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void AlignTo(std::vector<std::uint8_t>& out, std::size_t alignment) {
    out.resize((out.size() + alignment - 1) / alignment * alignment, 0);
}

// A minimal x86-64 shared library: functions point at "xor eax, eax; ret", data symbols at
// zero-filled 256-byte blocks. Uses DT_HASH, which glibc's loader supports.
bool WriteStubLibrary(const fs::path& path, const std::vector<std::pair<std::string, bool>>& stubs) {
    constexpr std::uint64_t Page = 0x1000;
    constexpr std::uint64_t ObjectSize = 256;
    constexpr int ProgramHeaders = 4;

    std::vector<std::uint8_t> strings{0};
    const auto addString = [&](const std::string& text) {
        const auto offset = static_cast<std::uint32_t>(strings.size());
        strings.insert(strings.end(), text.begin(), text.end());
        strings.push_back(0);
        return offset;
    };
    const std::uint32_t soname = addString("libsos_missing_imports.so");
    std::vector<std::uint32_t> names;
    for (const auto& stub : stubs) names.push_back(addString(stub.first));

    const std::uint32_t symbolCount = static_cast<std::uint32_t>(stubs.size() + 1);
    const std::uint32_t bucketCount = std::max<std::uint32_t>(1, symbolCount / 2 + 1);
    std::vector<std::uint32_t> buckets(bucketCount, 0), chains(symbolCount, 0);
    for (std::uint32_t index = 1; index < symbolCount; ++index) {
        const auto bucket = ElfHash(stubs[index - 1].first) % bucketCount;
        chains[index] = buckets[bucket];
        buckets[bucket] = index;
    }

    // Read-only + executable segment: headers, hash, symbols, strings, code.
    const std::uint64_t hashOffset = sizeof(Elf64_Ehdr) + ProgramHeaders * sizeof(Elf64_Phdr);
    const std::uint64_t hashSize = (2 + bucketCount + symbolCount) * 4;
    const std::uint64_t symbolOffset = (hashOffset + hashSize + 7) & ~std::uint64_t{7};
    const std::uint64_t stringOffset = symbolOffset + symbolCount * sizeof(Elf64_Sym);
    const std::uint64_t codeOffset = (stringOffset + strings.size() + 15) & ~std::uint64_t{15};
    const std::uint64_t textEnd = codeOffset + 16;
    // Writable segment: the dynamic section, then the zero-filled data blocks.
    const std::uint64_t dataOffset = (textEnd + Page - 1) & ~(Page - 1);
    constexpr int DynamicEntries = 7;
    const std::uint64_t dynamicSize = DynamicEntries * sizeof(Elf64_Dyn);
    const std::uint64_t objectsOffset = (dataOffset + dynamicSize + 63) & ~std::uint64_t{63};
    std::uint64_t objectCount = 0;
    for (const auto& stub : stubs) objectCount += stub.second ? 1 : 0;
    const std::uint64_t dataMemoryEnd = objectsOffset + objectCount * ObjectSize;

    std::vector<std::uint8_t> out;
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_ident[EI_OSABI] = ELFOSABI_SYSV;
    header.e_type = ET_DYN;
    header.e_machine = EM_X86_64;
    header.e_version = EV_CURRENT;
    header.e_phoff = sizeof(Elf64_Ehdr);
    header.e_ehsize = sizeof(Elf64_Ehdr);
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = ProgramHeaders;
    Append(out, header);

    Elf64_Phdr text{PT_LOAD, PF_R | PF_X, 0, 0, 0, textEnd, textEnd, Page};
    Elf64_Phdr data{PT_LOAD, PF_R | PF_W, dataOffset, dataOffset, dataOffset, dynamicSize, dataMemoryEnd - dataOffset, Page};
    Elf64_Phdr dynamic{PT_DYNAMIC, PF_R | PF_W, dataOffset, dataOffset, dataOffset, dynamicSize, dynamicSize, 8};
    Elf64_Phdr stack{PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 0, 16};
    Append(out, text);
    Append(out, data);
    Append(out, dynamic);
    Append(out, stack);

    out.resize(hashOffset, 0);
    Append(out, bucketCount);
    Append(out, symbolCount);
    for (const auto value : buckets) Append(out, value);
    for (const auto value : chains) Append(out, value);

    out.resize(symbolOffset, 0);
    Append(out, Elf64_Sym{});
    std::uint64_t nextObject = objectsOffset;
    for (std::size_t index = 0; index < stubs.size(); ++index) {
        Elf64_Sym symbol{};
        symbol.st_name = names[index];
        // Any section index other than UNDEF and ABS makes the loader add the load base to the value.
        symbol.st_shndx = 1;
        if (stubs[index].second) {
            symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
            symbol.st_value = nextObject;
            symbol.st_size = ObjectSize;
            nextObject += ObjectSize;
        } else {
            symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
            symbol.st_value = codeOffset;
            symbol.st_size = 3;
        }
        Append(out, symbol);
    }

    out.resize(stringOffset, 0);
    out.insert(out.end(), strings.begin(), strings.end());
    out.resize(codeOffset, 0);
    const std::uint8_t returnZero[] = {0x31, 0xc0, 0xc3};  // xor eax, eax; ret
    out.insert(out.end(), std::begin(returnZero), std::end(returnZero));
    out.resize(textEnd, 0xcc);

    out.resize(dataOffset, 0);
    const Elf64_Dyn entries[DynamicEntries] = {
        {DT_SONAME, {soname}}, {DT_HASH, {hashOffset}}, {DT_SYMTAB, {symbolOffset}}, {DT_STRTAB, {stringOffset}},
        {DT_STRSZ, {strings.size()}}, {DT_SYMENT, {sizeof(Elf64_Sym)}}, {DT_NULL, {0}}};
    for (const auto& entry : entries) Append(out, entry);

    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!file) return false;
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    return !error;
}

}

Result Write(const fs::path& runtime, const fs::path& libsDirectory, const fs::path& stubPath) {
    Result result;
    // Follow DT_NEEDED from the runtime like the loader does: only the libraries that are actually loaded
    // provide symbols. "$ORIGIN/..." names are guest modules, plain names are host libraries in libs/;
    // anything else is a system library, which dlsym below covers.
    std::vector<std::pair<fs::path, Image>> loaded;
    std::set<fs::path> seen;
    std::vector<fs::path> queue{runtime};
    for (std::size_t next = 0; next < queue.size(); ++next) {
        const fs::path path = queue[next];
        auto image = ReadImage(path);
        if (!image) { result.error = "cannot read " + path.string(); return result; }
        for (const auto& name : image->needed) {
            fs::path dependency;
            if (name.rfind("$ORIGIN/", 0) == 0) dependency = path.parent_path() / name.substr(8);
            else if (std::error_code error; fs::exists(libsDirectory / name, error)) dependency = libsDirectory / name;
            else continue;
            dependency = dependency.lexically_normal();
            if (seen.insert(dependency).second) queue.push_back(dependency);
        }
        loaded.emplace_back(path, std::move(*image));
    }

    std::unordered_set<std::string> defined;
    for (const auto& [path, image] : loaded)
        for (const auto& symbol : image.symbols)
            if (symbol.defined) defined.insert(symbol.name);

    std::map<std::string, bool> stubs;  // name -> is data
    std::set<std::string> logged;
    for (const auto& [path, image] : loaded) {
        for (const auto& symbol : image.symbols) {
            if (symbol.defined || symbol.weak || defined.contains(symbol.name)) continue;
            // Provided by the system libraries the runtime loads anyway (libc, libm, libstdc++, ld.so).
            if (dlsym(RTLD_DEFAULT, symbol.name.c_str()) != nullptr) continue;
            stubs.emplace(symbol.name, symbol.object);
            logged.insert(path.filename().string() + " " + symbol.name + (symbol.object ? " (data)" : ""));
        }
    }
    std::vector<std::pair<std::string, bool>> list(stubs.begin(), stubs.end());
    if (!WriteStubLibrary(stubPath, list)) { result.error = "cannot write " + stubPath.string(); return result; }
    result.missing.assign(logged.begin(), logged.end());
    result.stubCount = list.size();
    result.ok = true;
    return result;
}

}
