#pragma once

#include <domain/Types.hpp>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace Elfpatcher {

// The host loader maps PT_LOAD segments in whole pages straight from the file, while the console's
// copies each segment's bytes exactly. A segment that starts inside a page holding an earlier
// segment's memory (for example right after its BSS) would map file bytes over that memory. Such a
// segment's data moves to the end of the file, after the bytes the earlier segments hold in the
// shared page (their file bytes, zero for BSS), so the page maps as on the console.
inline void SeparateSharedPages(std::vector<std::uint8_t>& bytes, std::vector<Domain::ProgramHeader>& headers, std::uint64_t pageSize = 0x1000) {
    constexpr std::uint32_t load = 1;
    std::vector<std::size_t> order;
    for (std::size_t index = 0; index < headers.size(); ++index)
        if (headers[index].Type == load && headers[index].MemorySize != 0) order.push_back(index);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return headers[a].MappedAddress < headers[b].MappedAddress; });
    for (std::size_t position = 1; position < order.size(); ++position) {
        auto& segment = headers[order[position]];
        const auto pageStart = segment.MappedAddress & ~(pageSize - 1);
        if (pageStart == segment.MappedAddress) continue;
        // What the earlier segments hold at [pageStart, segment start).
        std::vector<std::uint8_t> prefix(segment.MappedAddress - pageStart, 0);
        bool shared = false;
        for (std::size_t earlier = 0; earlier < position; ++earlier) {
            const auto& other = headers[order[earlier]];
            const auto begin = std::max(other.MappedAddress, pageStart);
            const auto end = std::min(other.MappedAddress + other.MemorySize, segment.MappedAddress);
            if (begin >= end) continue;
            shared = true;
            const auto fileEnd = std::min(end, other.MappedAddress + other.FileSize);
            for (auto address = begin; address < fileEnd; ++address) {
                const auto offset = other.Offset + (address - other.MappedAddress);
                if (offset < bytes.size()) prefix[address - pageStart] = bytes[offset];
            }
        }
        if (!shared) continue;
        if (segment.Offset > bytes.size() || segment.FileSize > bytes.size() - segment.Offset)
            throw Domain::RelinkerException("Segment file range exceeds the image", segment.MappedAddress);
        const std::vector<std::uint8_t> data(bytes.begin() + static_cast<std::ptrdiff_t>(segment.Offset), bytes.begin() + static_cast<std::ptrdiff_t>(segment.Offset + segment.FileSize));
        // The file offset keeps the address's remainder modulo the segment's alignment.
        const auto alignment = std::max<std::uint64_t>(segment.Alignment, pageSize);
        auto pageOffset = (bytes.size() + alignment - 1) / alignment * alignment + (pageStart % alignment);
        bytes.resize(pageOffset, 0);
        bytes.insert(bytes.end(), prefix.begin(), prefix.end());
        segment.Offset = bytes.size();
        bytes.insert(bytes.end(), data.begin(), data.end());
        // The moved segment now covers the shared page itself, so later segments see the prefix too.
        segment.Offset -= prefix.size();
        segment.MappedAddress = pageStart;
        segment.PhysicalAddress = pageStart;
        segment.FileSize += prefix.size();
        segment.MemorySize += prefix.size();
    }
}

}
