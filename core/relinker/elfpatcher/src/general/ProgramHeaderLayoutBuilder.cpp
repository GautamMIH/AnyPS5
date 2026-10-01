#include <elfpatcher/general/ProgramHeaderLayoutBuilder.hpp>
#include <elfpatcher/general/ElfConstants.hpp>
#include <domain/Types.hpp>
#include <algorithm>
#include <cstdint>
#include <string>

namespace Elfpatcher {

ProgramHeaderLayoutBuilder::ProgramHeaderLayoutBuilder(
    std::shared_ptr<ISegmentFilter> segmentFilter,
    std::shared_ptr<Io::IByteWriter> byteWriter
)
    : _segmentFilter(std::move(segmentFilter))
    , _byteWriter(std::move(byteWriter))
{
}

void ProgramHeaderLayoutBuilder::_writeProgramHeader(std::vector<std::uint8_t>& buf, std::size_t offset, const Domain::ProgramHeader& ph) const {
    _byteWriter->WriteU32(buf, offset + kPhdrTypeOffset, ph.Type);
    _byteWriter->WriteU32(buf, offset + kPhdrFlagsOffset, ph.Flags);
    _byteWriter->WriteU64(buf, offset + kPhdrOffsetOffset, ph.Offset);
    _byteWriter->WriteU64(buf, offset + kPhdrVaddrOffset, ph.MappedAddress);
    _byteWriter->WriteU64(buf, offset + kPhdrPaddrOffset, ph.PhysicalAddress);
    _byteWriter->WriteU64(buf, offset + kPhdrFileSizeOffset, ph.FileSize);
    _byteWriter->WriteU64(buf, offset + kPhdrMemSizeOffset, ph.MemorySize);
    _byteWriter->WriteU64(buf, offset + kPhdrAlignOffset, ph.Alignment);
}

Domain::ProgramHeader ProgramHeaderLayoutBuilder::_makeLoadHeader(std::uint64_t offset, std::uint64_t vaddr, std::uint64_t size) const {
    Domain::ProgramHeader ph{};
    ph.Type = PT_LOAD;
    ph.Flags = PF_R | PF_W | PF_X;
    ph.Offset = offset;
    ph.MappedAddress = vaddr;
    ph.PhysicalAddress = vaddr;
    ph.FileSize = size;
    ph.MemorySize = size;
    ph.Alignment = kDefaultLoadAlignment;
    return ph;
}

Domain::ProgramHeader ProgramHeaderLayoutBuilder::_makeHeaderBlockLoad(std::uint64_t vaddr, std::uint64_t size, std::uint64_t align) const {
    Domain::ProgramHeader ph{};
    ph.Type = PT_LOAD;
    ph.Flags = PF_R;
    ph.Offset = 0;
    ph.MappedAddress = vaddr;
    ph.PhysicalAddress = vaddr;
    ph.FileSize = size;
    ph.MemorySize = size;
    ph.Alignment = align;
    return ph;
}

Domain::ProgramHeader ProgramHeaderLayoutBuilder::_makePhdrHeader(std::uint64_t offset, std::uint64_t vaddr, std::uint64_t size) const {
    Domain::ProgramHeader ph{};
    ph.Type = PT_PHDR;
    ph.Flags = PF_R;
    ph.Offset = offset;
    ph.MappedAddress = vaddr;
    ph.PhysicalAddress = vaddr;
    ph.FileSize = size;
    ph.MemorySize = size;
    ph.Alignment = kPhdrHeaderAlignment;
    return ph;
}

Domain::ProgramHeader ProgramHeaderLayoutBuilder::_makeDynamicHeader(std::uint64_t offset, std::uint64_t vaddr, std::uint64_t size) const {
    Domain::ProgramHeader ph{};
    ph.Type = PT_DYNAMIC;
    ph.Flags = PF_R | PF_W;
    ph.Offset = offset;
    ph.MappedAddress = vaddr;
    ph.PhysicalAddress = vaddr;
    ph.FileSize = size;
    ph.MemorySize = size;
    ph.Alignment = kDynamicHeaderAlignment;
    return ph;
}

Domain::ProgramHeader ProgramHeaderLayoutBuilder::_makeInterpHeader(std::uint64_t offset, std::uint64_t vaddr, std::uint64_t size) const {
    Domain::ProgramHeader ph{};
    ph.Type = PT_INTERP;
    ph.Flags = PF_R;
    ph.Offset = offset;
    ph.MappedAddress = vaddr;
    ph.PhysicalAddress = vaddr;
    ph.FileSize = size;
    ph.MemorySize = size;
    ph.Alignment = kInterpHeaderAlignment;
    return ph;
}

std::uint32_t ProgramHeaderLayoutBuilder::_fixLoadFlags(std::uint32_t originalFlags) const {
    if (originalFlags == 0) return PF_R | PF_W | PF_X;
    return originalFlags | PF_R;
}

std::uint64_t ProgramHeaderLayoutBuilder::ComputeExtraBlockVaddr(
    const std::vector<Domain::ProgramHeader>& originalHeaders,
    std::uint64_t extraBlockOffset
) const {
    bool foundLoad = false;
    std::uint64_t highestVaddrEnd = 0;
    for (const auto& ph : originalHeaders) {
        if (_segmentFilter->ShouldSkip(ph))
            continue;
        if (ph.Type != PT_LOAD)
            continue;
        foundLoad = true;
        const std::uint64_t vaddrEnd = ph.MappedAddress + ph.MemorySize;
        if (vaddrEnd > highestVaddrEnd)
            highestVaddrEnd = vaddrEnd;
    }
    if (!foundLoad)
        throw Domain::RelinkerException("No kept PT_LOAD segment to anchor the extra block against");

    // The block starts in the first page after the last segment: the loader maps it in whole pages,
    // so sharing the page that holds the end of a segment's BSS would replace those zeroes with
    // file bytes (it corrupted an IL2CPP module's GC thread table).
    const std::uint64_t align = kDefaultLoadAlignment;
    const std::uint64_t offsetRemainder = extraBlockOffset & (align - 1);
    if (highestVaddrEnd > UINT64_MAX - (align - 1))
        throw Domain::RelinkerException("Extra block vaddr computation overflowed");
    const std::uint64_t firstFreePage = (highestVaddrEnd + align - 1) & ~(align - 1);
    const std::uint64_t vaddr = firstFreePage | offsetRemainder;
    if (vaddr < firstFreePage)
        throw Domain::RelinkerException("Extra block vaddr computation overflowed");
    return vaddr;
}

std::uint16_t ProgramHeaderLayoutBuilder::WriteLayout(
    std::vector<std::uint8_t>& buf,
    const ProgramHeaderLayoutRequest& request
) const {
    std::uint16_t keptCount = 0;
    for (const auto& ph : request.OriginalHeaders) {
        if (_segmentFilter->ShouldSkip(ph))
            continue;
        keptCount++;
    }

    const bool hasInterp = request.InterpSize != 0;
    const std::uint16_t syntheticCount = hasInterp ? kSyntheticProgramHeaderCount : static_cast<std::uint16_t>(kSyntheticProgramHeaderCount - 1);
    const std::uint16_t requiredPh = keptCount + syntheticCount;
    if (requiredPh > request.PhNum || (request.RequireNonExecutableStack && requiredPh + 1 > request.PhNum))
        throw Domain::RelinkerException(
            "Not enough program header slots: need " + std::to_string(requiredPh + (request.RequireNonExecutableStack ? 1 : 0)) +
            ", available " + std::to_string(request.PhNum));
    const bool hasGnuStack = requiredPh + 1 <= request.PhNum;
    const std::uint16_t neededPh = hasGnuStack ? static_cast<std::uint16_t>(requiredPh + 1) : requiredPh;

    const std::uint64_t headerBlockSize = request.PhOff + static_cast<std::uint64_t>(neededPh) * request.PhEntSize;
    std::uint64_t headerBlockAlign = kDefaultLoadAlignment;
    for (const auto& ph : request.OriginalHeaders) {
        if (ph.Type == PT_LOAD && !_segmentFilter->ShouldSkip(ph) && ph.Alignment > headerBlockAlign && (ph.Alignment & (ph.Alignment - 1)) == 0)
            headerBlockAlign = ph.Alignment;
    }
    const std::uint64_t headerBlockVaddr =
        (request.ExtraBlockVaddr + request.ExtraBlockSize + headerBlockAlign - 1) & ~(headerBlockAlign - 1);

    if (request.DynamicSegmentOffset < request.ExtraBlockOffset)
        throw Domain::RelinkerException("Dynamic segment offset lies before the extra block");
    if (request.DynamicSegmentOffset + request.DynamicSegmentSize > request.ExtraBlockOffset + request.ExtraBlockSize)
        throw Domain::RelinkerException("Dynamic segment does not fit within the extra block");
    if (hasInterp && request.InterpOffset < request.ExtraBlockOffset)
        throw Domain::RelinkerException("Interp offset lies before the extra block");
    if (hasInterp && request.InterpOffset + request.InterpSize > request.ExtraBlockOffset + request.ExtraBlockSize)
        throw Domain::RelinkerException("Interp data does not fit within the extra block");

    const std::uint64_t dynamicSegmentVaddr = request.ExtraBlockVaddr + (request.DynamicSegmentOffset - request.ExtraBlockOffset);
    const std::uint64_t interpVaddr = hasInterp ? request.ExtraBlockVaddr + (request.InterpOffset - request.ExtraBlockOffset) : 0;

    std::vector<Domain::ProgramHeader> headers;
    headers.push_back(_makePhdrHeader(request.PhOff, headerBlockVaddr + request.PhOff, static_cast<std::uint64_t>(neededPh) * request.PhEntSize));
    headers.push_back(_makeHeaderBlockLoad(headerBlockVaddr, headerBlockSize, headerBlockAlign));

    for (const auto& ph : request.OriginalHeaders) {
        if (_segmentFilter->ShouldSkip(ph))
            continue;
        Domain::ProgramHeader kept = ph;
        if (kept.Type == PT_LOAD)
            kept.Flags = _fixLoadFlags(ph.Flags);
        headers.push_back(kept);
    }

    headers.push_back(_makeLoadHeader(request.ExtraBlockOffset, request.ExtraBlockVaddr, request.ExtraBlockSize));
    headers.push_back(_makeDynamicHeader(request.DynamicSegmentOffset, dynamicSegmentVaddr, request.DynamicSegmentSize));
    if (hasInterp)
        headers.push_back(_makeInterpHeader(request.InterpOffset, interpVaddr, request.InterpSize));
    if (hasGnuStack) {
        Domain::ProgramHeader stack{};
        stack.Type = PT_GNU_STACK;
        stack.Flags = PF_R | PF_W;
        stack.Alignment = kGnuStackAlignment;
        headers.push_back(stack);
    }

    std::vector<std::size_t> loadSlots;
    std::vector<Domain::ProgramHeader> loads;
    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (headers[index].Type != PT_LOAD)
            continue;
        loadSlots.push_back(index);
        loads.push_back(headers[index]);
    }
    std::stable_sort(loads.begin(), loads.end(), [](const Domain::ProgramHeader& left, const Domain::ProgramHeader& right) {
        return left.MappedAddress < right.MappedAddress;
    });
    for (std::size_t index = 0; index < loadSlots.size(); ++index)
        headers[loadSlots[index]] = loads[index];

    std::uint16_t writtenPh = 0;
    for (const auto& ph : headers) {
        _writeProgramHeader(buf, static_cast<std::size_t>(request.PhOff) + static_cast<std::size_t>(writtenPh) * request.PhEntSize, ph);
        writtenPh++;
    }

    return writtenPh;
}

}
