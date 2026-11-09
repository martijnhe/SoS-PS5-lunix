#include <elfpatcher/linux/LinuxElfPatcher.hpp>
#include <elfpatcher/general/ElfConstants.hpp>
#include <elfpatcher/general/ProgramHeaderLayoutRequest.hpp>
#include <elfpatcher/general/SectionHeaderTableRequest.hpp>
#include <codegen/x86/Amd64OnlySubstitutionTable.hpp>
#include <algorithm>
#include <iterator>
#include <map>
#include <limits>
#include <string>

namespace Elfpatcher::Linux {

namespace {

// Linux executables are placed at this address, as in upstream AnyPS5: the console loads the executable
// inside the 40-bit address window that a title's own sceLibcMspaceCreate checks, and a position
// independent executable lands far above it (0x55..., or 0x7f... under ld.so).
constexpr std::uint64_t kFixedBase = 0x400000;

std::uint64_t Read64(const std::vector<std::uint8_t>& b, std::uint64_t o) {
    if (o + 8 > b.size()) throw Domain::RelinkerException("Fixed base: read outside the image", o);
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[o + i];
    return v;
}
std::uint32_t Read32(const std::vector<std::uint8_t>& b, std::uint64_t o) { return static_cast<std::uint32_t>(Read64(b, o) & 0xffffffffu); }
std::uint16_t Read16(const std::vector<std::uint8_t>& b, std::uint64_t o) {
    if (o + 2 > b.size()) throw Domain::RelinkerException("Fixed base: read outside the image", o);
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
void Write64(std::vector<std::uint8_t>& b, std::uint64_t o, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void Write16(std::vector<std::uint8_t>& b, std::uint64_t o, std::uint16_t v) {
    b[o] = static_cast<std::uint8_t>(v);
    b[o + 1] = static_cast<std::uint8_t>(v >> 8);
}

// Turns the finished position-independent executable into an ET_EXEC whose virtual address 0 is at base:
// every address the image describes (program headers, entry, dynamic pointers, defined symbols, relocation
// offsets and RELATIVE targets, section addresses) moves by base. Code refers to itself PC-relative or
// through those relocations, so the bytes themselves stay as they are.
void RebaseToFixedAddress(std::vector<std::uint8_t>& elf, const std::uint64_t base) {
    constexpr std::uint32_t PtLoad = 1, PtDynamic = 2, PtGnuStack = 0x6474e551;
    const std::uint64_t phOff = Read64(elf, 0x20);
    const std::uint16_t phEntSize = Read16(elf, 0x36);
    const std::uint16_t phNum = Read16(elf, 0x38);
    struct Load { std::uint64_t vaddr, offset, fileSize; };
    std::vector<Load> loads;
    std::uint64_t dynOff = 0, dynSize = 0;
    for (std::uint16_t i = 0; i < phNum; ++i) {
        const auto h = phOff + std::uint64_t{i} * phEntSize;
        const auto type = Read32(elf, h);
        if (type == PtLoad) {
            const auto align = Read64(elf, h + 0x30);
            if (align > 1 && base % align != 0) throw Domain::RelinkerException("Fixed base is not a multiple of a PT_LOAD alignment");
            loads.push_back({Read64(elf, h + 0x10), Read64(elf, h + 0x08), Read64(elf, h + 0x20)});
        }
        if (type == PtDynamic) { dynOff = Read64(elf, h + 0x08); dynSize = Read64(elf, h + 0x20); }
    }
    const auto offsetOf = [&](std::uint64_t vaddr) {
        for (const auto& load : loads)
            if (vaddr >= load.vaddr && vaddr < load.vaddr + load.fileSize) return vaddr - load.vaddr + load.offset;
        throw Domain::RelinkerException("Fixed base: address is not file-backed", vaddr);
    };

    // Dynamic section: pointer-valued tags.
    constexpr std::int64_t PointerTags[] = {3, 4, 5, 6, 7, 12, 13, 17, 23, 25, 26, 32, 36, 0x6ffffef5, 0x6ffffff0, 0x6ffffffc, 0x6ffffffe};
    std::map<std::int64_t, std::uint64_t> tags;
    for (std::uint64_t e = dynOff; e + 16 <= dynOff + dynSize; e += 16) {
        const auto tag = static_cast<std::int64_t>(Read64(elf, e));
        if (tag == 0) break;
        const auto value = Read64(elf, e + 8);
        tags.emplace(tag, value);
        if (tag == 0x6ffffffb && (value & 0x08000000u)) Write64(elf, e + 8, value & ~std::uint64_t{0x08000000u});  // drop DF_1_PIE
        if (std::find(std::begin(PointerTags), std::end(PointerTags), tag) != std::end(PointerTags)) Write64(elf, e + 8, value + base);
    }

    // Defined dynamic symbols. The count comes from the .dynsym section header (offsets in this image's
    // section table are file offsets), else from the gap up to the string table.
    if (tags.contains(6)) {
        const auto symOff = offsetOf(tags[6]);
        std::uint64_t count = 0;
        const auto shOff = Read64(elf, 0x28);
        const auto shNum = Read16(elf, 0x3c), shEntSize = Read16(elf, 0x3a);
        for (std::uint16_t i = 0; shOff != 0 && i < shNum; ++i) {
            const auto sh = shOff + std::uint64_t{i} * shEntSize;
            if (Read32(elf, sh + 4) == 11) { count = Read64(elf, sh + 0x20) / 24; break; }
        }
        if (count == 0 && tags.contains(5) && tags[5] > tags[6]) count = (tags[5] - tags[6]) / 24;
        for (std::uint64_t i = 1; i < count; ++i) {
            const auto sym = symOff + i * 24;
            const auto section = Read16(elf, sym + 6);
            if (section == 0 || section == 0xfff1 || (elf[sym + 4] & 0xf) == 6) continue;  // UNDEF, ABS, TLS
            Write64(elf, sym + 8, Read64(elf, sym + 8) + base);
        }
    }

    // Relocations.
    const auto rebaseRelocations = [&](std::int64_t tableTag, std::int64_t sizeTag) {
        if (!tags.contains(tableTag) || !tags.contains(sizeTag)) return;
        const auto table = offsetOf(tags[tableTag]);
        for (std::uint64_t e = table; e + 24 <= table + tags[sizeTag]; e += 24) {
            const auto type = static_cast<std::uint32_t>(Read64(elf, e + 8));
            if (type == 0) continue;
            if (type == 8 || type == 37) Write64(elf, e + 16, Read64(elf, e + 16) + base);  // RELATIVE, IRELATIVE
            else if (type != 1 && type != 2 && type != 6 && type != 7 && type != 10 && type != 16 && type != 17 && type != 18)
                throw Domain::RelinkerException("Relocation type " + std::to_string(type) + " cannot be placed at a fixed Linux base");
            Write64(elf, e, Read64(elf, e) + base);
        }
    };
    rebaseRelocations(7, 8);    // DT_RELA, DT_RELASZ
    rebaseRelocations(23, 2);   // DT_JMPREL, DT_PLTRELSZ

    // Program headers, entry point, type.
    for (std::uint16_t i = 0; i < phNum; ++i) {
        const auto h = phOff + std::uint64_t{i} * phEntSize;
        if (Read32(elf, h) == PtGnuStack) continue;
        Write64(elf, h + 0x10, Read64(elf, h + 0x10) + base);
        Write64(elf, h + 0x18, Read64(elf, h + 0x18) + base);
    }
    Write64(elf, 0x18, Read64(elf, 0x18) + base);
    Write16(elf, 0x10, 2);  // ET_EXEC

    // Section addresses, for tools; the loader does not read them.
    const auto shOff = Read64(elf, 0x28);
    const auto shNum = Read16(elf, 0x3c), shEntSize = Read16(elf, 0x3a);
    for (std::uint16_t i = 0; shOff != 0 && i < shNum; ++i) {
        const auto sh = shOff + std::uint64_t{i} * shEntSize;
        if (const auto address = Read64(elf, sh + 0x10); address != 0) Write64(elf, sh + 0x10, address + base);
    }
}

}

LinuxElfPatcher::LinuxElfPatcher(
    std::shared_ptr<IEntryStubBuilder> entryStubBuilder,
    std::shared_ptr<IProgramHeaderLayoutBuilder> programHeaderLayoutBuilder,
    std::shared_ptr<ISectionHeaderTableBuilder> sectionHeaderTableBuilder,
    std::shared_ptr<Io::IByteWriter> byteWriter
)
    : _entryStubBuilder(std::move(entryStubBuilder))
    , _programHeaderLayoutBuilder(std::move(programHeaderLayoutBuilder))
    , _sectionHeaderTableBuilder(std::move(sectionHeaderTableBuilder))
    , _byteWriter(std::move(byteWriter))
{
}

void LinuxElfPatcher::_appendDynEntry(std::vector<std::uint8_t>& buf, std::int64_t tag, std::uint64_t val) const {
    _byteWriter->AppendI64(buf, tag);
    _byteWriter->AppendU64(buf, val);
}

void LinuxElfPatcher::_appendTrampoline(
    std::vector<std::uint8_t>& buf,
    const Codegen::TrampolineSite& site,
    const std::uint64_t extraBlockOffset,
    const std::function<std::uint64_t(std::uint64_t)>& vaddrOfExtraBlockOffset) const
{
    using namespace Codegen::Amd64OnlySubstitutionTable;
    if (site.Length < kJmpRel32.Size || site.OriginalBytes.size() != site.Length || site.Body.size() < kJmpRel32.Size || site.ReturnBranchOffset > site.Body.size() - kJmpRel32.Size || site.Body[site.ReturnBranchOffset] != kJmpRel32.Bytes[0])
        throw Domain::RelinkerException("Invalid AMD-only trampoline site", site.Offset);
    if (site.Offset > extraBlockOffset || site.Length > extraBlockOffset - site.Offset)
        throw Domain::RelinkerException("AMD-only instruction is outside the original image", site.Offset);
    if (!std::equal(site.OriginalBytes.begin(), site.OriginalBytes.end(), buf.begin() + static_cast<std::ptrdiff_t>(site.Offset)))
        throw Domain::RelinkerException("AMD-only site bytes changed before patching", site.Offset);

    while (buf.size() % kStubAlignment != 0)
        buf.push_back(kTrapFill);
    const auto bodyOff = static_cast<std::uint64_t>(buf.size());
    const auto bodyVaddr = vaddrOfExtraBlockOffset(bodyOff);
    for (const std::uint8_t b : site.Body)
        buf.push_back(b);

    const auto inRange = [](const std::int64_t displacement) {
        return displacement >= std::numeric_limits<std::int32_t>::min() && displacement <= std::numeric_limits<std::int32_t>::max();
    };
    const auto returnDisplacement = static_cast<std::int64_t>(site.Address + site.Length) - static_cast<std::int64_t>(bodyVaddr + site.ReturnBranchOffset + kJmpRel32.Size);
    const auto jumpDisplacement = static_cast<std::int64_t>(bodyVaddr) - static_cast<std::int64_t>(site.Address + kJmpRel32.Size);
    if (!inRange(returnDisplacement) || !inRange(jumpDisplacement))
        throw Domain::RelinkerException("AMD-only stub exceeds rel32 range", site.Offset);
    _byteWriter->WriteU32(buf, static_cast<std::size_t>(bodyOff + site.ReturnBranchOffset + 1), static_cast<std::uint32_t>(returnDisplacement));

    std::fill_n(buf.begin() + static_cast<std::ptrdiff_t>(site.Offset), site.Length, kNop1.Bytes[0]);
    buf[static_cast<std::size_t>(site.Offset)] = kJmpRel32.Bytes[0];
    _byteWriter->WriteU32(buf, static_cast<std::size_t>(site.Offset + 1), static_cast<std::uint32_t>(jumpDisplacement));
}

std::vector<std::uint8_t> LinuxElfPatcher::Patch(
    const std::vector<std::uint8_t>& sourceElf,
    const std::vector<Domain::ProgramHeader>& originalHeaders,
    const Domain::SysVDynamicSection& dynSection,
    const std::uint64_t originalPltGotVaddr,
    const std::string& runPath,
    const bool lazyBinding,
    const bool dependencyDiagnostics,
    const std::vector<Codegen::TrampolineSite>& trampolines)
{
    if (dependencyDiagnostics)
        throw Domain::RelinkerException("Linux target does not support --windows-diagnostics");

    std::vector<std::uint8_t> buf = sourceElf;

    buf[kEhdrOsAbiOffset] = 0;
    buf[kEhdrAbiVersionOffset] = 0;
    buf[kEhdrTypeOffset] = static_cast<std::uint8_t>(ET_DYN & 0xFF);
    buf[kEhdrTypeOffset + 1] = static_cast<std::uint8_t>((ET_DYN >> 8) & 0xFF);
    _byteWriter->WriteU64(buf, kEhdrShOffOffset, 0);
    _byteWriter->WriteU16(buf, kEhdrShNumOffset, 0);
    _byteWriter->WriteU16(buf, kEhdrShStrNdxOffset, 0);

    const std::uint64_t phOff = *reinterpret_cast<const std::uint64_t*>(buf.data() + kEhdrPhOffOffset);
    const std::uint16_t phEntSize = *reinterpret_cast<const std::uint16_t*>(buf.data() + kEhdrPhEntSizeOffset);
    const std::uint16_t phNum = *reinterpret_cast<const std::uint16_t*>(buf.data() + kEhdrPhNumOffset);

    const auto alignBuf = [](std::vector<std::uint8_t>& b, std::size_t alignment) {
        while (b.size() % alignment != 0)
            b.push_back(0);
    };

    const auto dynStrOff = static_cast<std::uint64_t>(buf.size());
    for (std::uint8_t b : dynSection.DynStrData)
        buf.push_back(b);
    const std::uint64_t runPathStrOff = static_cast<std::uint64_t>(buf.size()) - dynStrOff;
    for (char c : runPath)
        buf.push_back(static_cast<std::uint8_t>(c));
    buf.push_back(0);
    alignBuf(buf, kDynStrAlignment);

    const auto dynSymOff = static_cast<std::uint64_t>(buf.size());
    for (std::uint8_t b : dynSection.DynSymData)
        buf.push_back(b);
    alignBuf(buf, kDynSymAlignment);

    const auto relaOff = static_cast<std::uint64_t>(buf.size());
    for (std::uint8_t b : dynSection.RelaData)
        buf.push_back(b);
    alignBuf(buf, kRelaAlignment);

    const auto relaPltOff = static_cast<std::uint64_t>(buf.size());
    for (std::uint8_t b : dynSection.RelaPltData)
        buf.push_back(b);
    alignBuf(buf, kRelaPltAlignment);

    const std::uint64_t extraBlockOff = dynStrOff;
    const std::uint64_t extraBlockVaddr = _programHeaderLayoutBuilder->ComputeExtraBlockVaddr(originalHeaders, extraBlockOff);

    const auto vaddrOfExtraBlockOffset = [extraBlockOff, extraBlockVaddr](std::uint64_t fileOffset) -> std::uint64_t {
        if (fileOffset < extraBlockOff)
            throw Domain::RelinkerException("Extra block member offset lies before the extra block");
        return extraBlockVaddr + (fileOffset - extraBlockOff);
    };

    std::vector<std::uint8_t> dynSegBuf;
    for (std::uint8_t b : dynSection.DynamicSegmentData)
        dynSegBuf.push_back(b);
    _appendDynEntry(dynSegBuf, DT_STRTAB, vaddrOfExtraBlockOffset(dynStrOff));
    _appendDynEntry(dynSegBuf, DT_STRSZ, dynSection.DynStrData.size());
    _appendDynEntry(dynSegBuf, DT_SYMTAB, vaddrOfExtraBlockOffset(dynSymOff));
    _appendDynEntry(dynSegBuf, DT_SYMENT, kSymEntrySize);
    if (!dynSection.RelaData.empty()) {
        _appendDynEntry(dynSegBuf, DT_RELA, vaddrOfExtraBlockOffset(relaOff));
        _appendDynEntry(dynSegBuf, DT_RELASZ, dynSection.RelaData.size());
        _appendDynEntry(dynSegBuf, DT_RELAENT, kRelaEntrySize);
    }
    if (!dynSection.RelaPltData.empty()) {
        _appendDynEntry(dynSegBuf, DT_JMPREL, vaddrOfExtraBlockOffset(relaPltOff));
        _appendDynEntry(dynSegBuf, DT_PLTRELSZ, dynSection.RelaPltData.size());
        _appendDynEntry(dynSegBuf, DT_PLTREL, static_cast<std::uint64_t>(DT_RELA));
        _appendDynEntry(dynSegBuf, DT_PLTGOT, originalPltGotVaddr);
    }
    if (!lazyBinding)
        _appendDynEntry(dynSegBuf, DT_FLAGS, DF_BIND_NOW);
    _appendDynEntry(dynSegBuf, DT_RUNPATH, runPathStrOff);
    _appendDynEntry(dynSegBuf, DT_NULL, 0);

    const auto dynSegOff = static_cast<std::uint64_t>(buf.size());
    for (std::uint8_t b : dynSegBuf)
        buf.push_back(b);

    const std::uint64_t realEntryVaddr = *reinterpret_cast<const std::uint64_t*>(buf.data() + kEhdrEntryOffset);
    const auto stubOff = static_cast<std::uint64_t>(buf.size());
    const auto stubVaddr = vaddrOfExtraBlockOffset(stubOff);
    const auto stubBytes = _entryStubBuilder->BuildEntryStub(stubVaddr, realEntryVaddr);
    for (std::uint8_t b : stubBytes)
        buf.push_back(b);
    _byteWriter->WriteU64(buf, kEhdrEntryOffset, stubVaddr);

    for (const auto& site : trampolines)
        _appendTrampoline(buf, site, extraBlockOff, vaddrOfExtraBlockOffset);

    static constexpr char kInterp[] = "/lib64/ld-linux-x86-64.so.2";
    const auto interpOff = static_cast<std::uint64_t>(buf.size());
    constexpr std::uint64_t interpSize = sizeof(kInterp);
    for (char i : kInterp)
        buf.push_back(static_cast<std::uint8_t>(i));

    const std::uint64_t extraBlockSize = static_cast<std::uint64_t>(buf.size()) - extraBlockOff;

    ProgramHeaderLayoutRequest layoutRequest{};
    layoutRequest.PhOff = phOff;
    layoutRequest.PhEntSize = phEntSize;
    layoutRequest.PhNum = phNum;
    layoutRequest.OriginalHeaders = originalHeaders;
    layoutRequest.ExtraBlockOffset = extraBlockOff;
    layoutRequest.ExtraBlockVaddr = extraBlockVaddr;
    layoutRequest.ExtraBlockSize = extraBlockSize;
    layoutRequest.DynamicSegmentOffset = dynSegOff;
    layoutRequest.DynamicSegmentSize = dynSegBuf.size();
    layoutRequest.InterpOffset = interpOff;
    layoutRequest.InterpSize = interpSize;

    const std::uint16_t writtenPh = _programHeaderLayoutBuilder->WriteLayout(buf, layoutRequest);
    _byteWriter->WriteU16(buf, kEhdrPhNumOffset, writtenPh);

    SectionHeaderTableRequest sectionRequest{};
    sectionRequest.DynStrOffset = dynStrOff;
    sectionRequest.DynStrSize = dynSection.DynStrData.size();
    sectionRequest.DynSymOffset = dynSymOff;
    sectionRequest.DynSymSize = dynSection.DynSymData.size();
    sectionRequest.DynamicSegmentOffset = dynSegOff;
    sectionRequest.DynamicSegmentSize = dynSegBuf.size();
    sectionRequest.StubOffset = stubOff;
    sectionRequest.StubSize = stubBytes.size();

    _sectionHeaderTableBuilder->WriteTable(buf, sectionRequest);

    // Lazy binding keeps the executable position independent, as upstream does.
    if (!lazyBinding) RebaseToFixedAddress(buf, kFixedBase);
    return buf;
}

}
