#include <elfpatcher/linux/LinuxElfPatcher.hpp>
#include <elfpatcher/general/ElfConstants.hpp>
#include <elfpatcher/general/ProgramHeaderLayoutRequest.hpp>
#include <elfpatcher/general/SectionHeaderTableRequest.hpp>
#include <string>

namespace Elfpatcher::Linux {

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

std::vector<std::uint8_t> LinuxElfPatcher::Patch(
    const std::vector<std::uint8_t>& sourceElf,
    const std::vector<Domain::ProgramHeader>& originalHeaders,
    const Domain::SysVDynamicSection& dynSection,
    const std::uint64_t originalPltGotVaddr,
    const std::string& runPath,
    const bool lazyBinding,
    const bool dependencyDiagnostics,
    const Domain::ModuleLinkInfo& linkInfo)
{
    const bool isLibrary = linkInfo.Kind == Domain::ModuleKind::Library;
    if (isLibrary && linkInfo.SoName.empty())
        throw Domain::RelinkerException("Library modules require a DT_SONAME");

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
    const std::uint64_t soNameStrOff = static_cast<std::uint64_t>(buf.size()) - dynStrOff;
    if (isLibrary) {
        for (char c : linkInfo.SoName)
            buf.push_back(static_cast<std::uint8_t>(c));
        buf.push_back(0);
    }
    const std::uint64_t dynStrSize = static_cast<std::uint64_t>(buf.size()) - dynStrOff;
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

    if (dynSection.HashData.empty())
        throw Domain::RelinkerException("Dynamic symbol hash table was not built");
    alignBuf(buf, kHashAlignment);
    const auto hashOff = static_cast<std::uint64_t>(buf.size());
    for (std::uint8_t b : dynSection.HashData)
        buf.push_back(b);

    const std::uint64_t extraBlockOff = dynStrOff;
    const std::uint64_t extraBlockVaddr = _programHeaderLayoutBuilder->ComputeExtraBlockVaddr(originalHeaders, extraBlockOff);

    const auto vaddrOfExtraBlockOffset = [extraBlockOff, extraBlockVaddr](std::uint64_t fileOffset) -> std::uint64_t {
        if (fileOffset < extraBlockOff)
            throw Domain::RelinkerException("Extra block member offset lies before the extra block");
        return extraBlockVaddr + (fileOffset - extraBlockOff);
    };

    std::uint64_t initStubVaddr = 0;
    std::uint64_t finiStubVaddr = 0;
    std::uint64_t moduleStubOff = static_cast<std::uint64_t>(buf.size());
    if (isLibrary) {
        alignBuf(buf, kStubAlignment);
        moduleStubOff = static_cast<std::uint64_t>(buf.size());
        const auto appendCallStub = [&](const std::uint64_t target) -> std::uint64_t {
            alignBuf(buf, kStubAlignment);
            const std::uint64_t stubVaddr = vaddrOfExtraBlockOffset(static_cast<std::uint64_t>(buf.size()));
            for (std::uint8_t b : _entryStubBuilder->BuildNullArgumentCallStub(stubVaddr, target))
                buf.push_back(b);
            return stubVaddr;
        };
        if (linkInfo.InitAddress != 0)
            initStubVaddr = appendCallStub(linkInfo.InitAddress);
        if (linkInfo.FiniAddress != 0)
            finiStubVaddr = appendCallStub(linkInfo.FiniAddress);
        alignBuf(buf, kDynSymAlign);
    }
    const std::uint64_t moduleStubSize = static_cast<std::uint64_t>(buf.size()) - moduleStubOff;

    std::vector<std::uint8_t> dynSegBuf;
    for (std::uint8_t b : dynSection.DynamicSegmentData)
        dynSegBuf.push_back(b);
    if (isLibrary)
        _appendDynEntry(dynSegBuf, DT_SONAME, soNameStrOff);
    else
        _appendDynEntry(dynSegBuf, DT_DEBUG, 0);
    _appendDynEntry(dynSegBuf, DT_HASH, vaddrOfExtraBlockOffset(hashOff));
    _appendDynEntry(dynSegBuf, DT_STRTAB, vaddrOfExtraBlockOffset(dynStrOff));
    _appendDynEntry(dynSegBuf, DT_STRSZ, dynStrSize);
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
    if (initStubVaddr != 0)
        _appendDynEntry(dynSegBuf, DT_INIT, initStubVaddr);
    if (finiStubVaddr != 0)
        _appendDynEntry(dynSegBuf, DT_FINI, finiStubVaddr);
    const std::uint64_t dynFlags = (lazyBinding ? 0 : DF_BIND_NOW) | (isLibrary ? DF_SYMBOLIC : 0);
    if (dynFlags != 0)
        _appendDynEntry(dynSegBuf, DT_FLAGS, dynFlags);
    _appendDynEntry(dynSegBuf, DT_RUNPATH, runPathStrOff);
    _appendDynEntry(dynSegBuf, DT_NULL, 0);

    const auto dynSegOff = static_cast<std::uint64_t>(buf.size());
    for (std::uint8_t b : dynSegBuf)
        buf.push_back(b);

    std::uint64_t stubOff = moduleStubOff;
    std::uint64_t stubSize = moduleStubSize;
    std::uint64_t interpOff = 0;
    std::uint64_t interpSize = 0;
    if (isLibrary) {
        _byteWriter->WriteU64(buf, kEhdrEntryOffset, 0);
    } else {
        const std::uint64_t realEntryVaddr = *reinterpret_cast<const std::uint64_t*>(buf.data() + kEhdrEntryOffset);
        stubOff = static_cast<std::uint64_t>(buf.size());
        const auto stubVaddr = vaddrOfExtraBlockOffset(stubOff);
        const auto stubBytes = _entryStubBuilder->BuildEntryStub(stubVaddr, realEntryVaddr);
        for (std::uint8_t b : stubBytes)
            buf.push_back(b);
        stubSize = stubBytes.size();
        _byteWriter->WriteU64(buf, kEhdrEntryOffset, stubVaddr);

        static constexpr char kInterp[] = "/lib64/ld-linux-x86-64.so.2";
        interpOff = static_cast<std::uint64_t>(buf.size());
        interpSize = sizeof(kInterp);
        for (char i : kInterp)
            buf.push_back(static_cast<std::uint8_t>(i));
    }

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
    layoutRequest.RequireNonExecutableStack = isLibrary;

    const std::uint16_t writtenPh = _programHeaderLayoutBuilder->WriteLayout(buf, layoutRequest);
    _byteWriter->WriteU16(buf, kEhdrPhNumOffset, writtenPh);

    SectionHeaderTableRequest sectionRequest{};
    sectionRequest.DynStrOffset = dynStrOff;
    sectionRequest.DynStrSize = dynStrSize;
    sectionRequest.DynSymOffset = dynSymOff;
    sectionRequest.DynSymSize = dynSection.DynSymData.size();
    sectionRequest.DynamicSegmentOffset = dynSegOff;
    sectionRequest.DynamicSegmentSize = dynSegBuf.size();
    sectionRequest.StubOffset = stubOff;
    sectionRequest.StubSize = stubSize;

    _sectionHeaderTableBuilder->WriteTable(buf, sectionRequest);

    return buf;
}

}
