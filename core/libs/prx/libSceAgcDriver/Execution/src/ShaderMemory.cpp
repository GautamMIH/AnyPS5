#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "CacheKey.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace AgcDriver {

ShaderMemory::ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> initial) {
    const ShaderRecompiler::RequestMemoryView validated(initial);
    for (const auto& region : initial) {
        regions.emplace(region.guestAddress, std::vector<std::byte>(region.bytes.begin(), region.bytes.end()));
        initialRanges.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
    }
}

bool ShaderMemory::initial(std::uint64_t address) const {
    return std::any_of(initialRanges.begin(), initialRanges.end(), [&](const auto& range) { return range.first <= address && address < range.second; });
}

bool ShaderMemory::read(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& self = *static_cast<ShaderMemory*>(context);
    if (address % sizeof(*value) != 0 || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) {
        throw std::runtime_error("AGC driver: invalid shader memory read address");
    }
    if (!self.initial(address)) self.accessed.push_back(address);
    const auto next = self.regions.upper_bound(address);
    if (next != self.regions.begin()) {
        const auto previous = std::prev(next);
        const auto offset = address - previous->first;
        if (offset < previous->second.size()) {
            if (previous->second.size() - offset < sizeof(*value)) {
                throw std::runtime_error("AGC driver: shader memory read crosses a snapshot boundary");
            }
            std::memcpy(value, previous->second.data() + offset, sizeof(*value));
            return true;
        }
    }
    if (next != self.regions.end() && next->first - address < sizeof(*value)) {
        throw std::runtime_error("AGC driver: shader memory read overlaps a snapshot boundary");
    }
    // Resource tables are contiguous: the aligned line around the dword is read at once (one guarded
    // read instead of one per dword), clipped to the neighbouring snapshots. A line never crosses a
    // page, so it is mapped wherever the dword is.
    constexpr std::uint64_t line = 64;
    auto begin = address & ~(line - 1u);
    auto end = begin + line;
    if (next != self.regions.end()) end = std::min(end, next->first);
    if (next != self.regions.begin()) {
        const auto& [base, bytes] = *std::prev(next);
        begin = std::max(begin, base + bytes.size());
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end - begin));
    GuestMemory::Read(begin, bytes, 1);
    std::memcpy(value, bytes.data() + (address - begin), sizeof(*value));
    self.regions.emplace(begin, std::move(bytes));
    return true;
}

ShaderRecompiler::SrtRuntime ShaderMemory::runtime(const ShaderRecompiler::RecompileRequest& request) {
    accessed.clear();
    ShaderRecompiler::SrtRuntime result;
    result.userData = request.context.userData;
    result.shaderBase = request.shader.codeAddress;
    result.userContext = this;
    result.readMemory = &read;
    result.readSpecializationMemory = &read;
    return result;
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::Capture(const ShaderRecompiler::RecompileRequest& request) {
    return ShaderRecompiler::CaptureResources(request, runtime(request));
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::Capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::ResourceCapture& sameProgram) {
    return ShaderRecompiler::CaptureResources(request, runtime(request), sameProgram);
}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::Regions() const {
    std::vector<ShaderRecompiler::MemoryRegion> result;
    result.reserve(regions.size());
    for (const auto& [address, bytes] : regions) {
        result.push_back({address, bytes});
    }
    return result;
}

std::vector<std::pair<std::uint64_t, std::uint32_t>> ShaderMemory::CapturedWords() const {
    auto addresses = accessed;
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    std::vector<std::pair<std::uint64_t, std::uint32_t>> words;
    words.reserve(addresses.size());
    for (const auto address : addresses) {
        const auto next = regions.upper_bound(address);
        if (next == regions.begin()) throw std::runtime_error("AGC driver: captured shader memory word is missing");
        const auto& [base, bytes] = *std::prev(next);
        if (address - base + sizeof(std::uint32_t) > bytes.size()) throw std::runtime_error("AGC driver: captured shader memory word is missing");
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + (address - base), sizeof(value));
        words.emplace_back(address, value);
    }
    return words;
}

void ShaderMemory::Insert(std::uint64_t address, std::uint32_t value) {
    const auto next = regions.upper_bound(address);
    if (next != regions.begin()) {
        const auto& [base, bytes] = *std::prev(next);
        if (address - base < bytes.size()) return;
    }
    std::vector<std::byte> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    regions.emplace(address, std::move(bytes));
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> CaptureMemo::Capture(const std::shared_ptr<const void>& owner, const ShaderRecompiler::RecompileRequest& request, ShaderMemory& memory) {
    if (owner == nullptr) return memory.Capture(request);
    key.clear();
    key.push_back(reinterpret_cast<std::uintptr_t>(owner.get()));
    key.push_back(request.shader.codeAddress);
    key.push_back(request.shader.code.size());
    key.push_back(request.shader.headerAddress);
    ShaderRecompiler::RecompileCacheKey::BuildContext(request, key);
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : key) hash = (hash ^ word) * 0x100000001b3ull;
    const auto programHash = hash;
    const auto programWords = key.size();
    key.insert(key.end(), request.context.userData.begin(), request.context.userData.end());
    for (const auto word : request.context.userData) hash = (hash ^ word) * 0x100000001b3ull;
    PerformanceTimer timing("Driver.CaptureMemo");
    const auto found = entries.find(hash);
    const bool known = found != entries.end() && found->second.key == key && found->second.owner == owner;
    if (known && reusable(found->second, memory)) {
        timing.Mark("hit");
        return found->second.capture;
    }
    // Taken before the capture reads, so a mapping change during it invalidates the entry.
    const auto mappingGeneration = GuestMemoryBacking::GuestMemoryBackingGeneration_nid_postfix();
    // The same program and context captured with other user data: its source and plan are reused.
    const auto program = programs.find(programHash);
    const bool sameProgram = program != programs.end() && program->second.owner == owner && program->second.key.size() == programWords && std::equal(program->second.key.begin(), program->second.key.end(), key.begin());
    auto capture = sameProgram ? memory.Capture(request, *program->second.capture) : memory.Capture(request);
    if (entries.size() >= 16384) entries.clear();
    if (programs.size() >= 4096) programs.clear();
    entries[hash] = Entry{owner, key, capture, memory.CapturedWords(), mappingGeneration};
    if (!sameProgram) programs[programHash] = Program{owner, std::vector<std::uint64_t>(key.begin(), key.begin() + static_cast<std::ptrdiff_t>(programWords)), capture};
    timing.Mark(known ? "changed" : sameProgram ? "walk" : "miss");
    return capture;
}

bool CaptureMemo::reusable(const Entry& entry, ShaderMemory& memory) const {
    if (GuestMemoryBacking::GuestMemoryBackingGeneration_nid_postfix() != entry.mappingGeneration) return false;
    // Runs of adjacent dwords get the guards the capture's reads had (pending GPU writes, resident
    // targets, watches), then every dword must still hold its captured value.
    const auto& words = entry.words;
    for (std::size_t first = 0; first < words.size();) {
        auto last = first + 1;
        while (last < words.size() && words[last].first == words[last - 1].first + sizeof(std::uint32_t)) ++last;
        const auto begin = words[first].first;
        const auto bytes = static_cast<std::size_t>(words[last - 1].first + sizeof(std::uint32_t) - begin);
        GuestMemory::CheckRange(reinterpret_cast<const void*>(begin), bytes, alignof(std::uint32_t));
        for (auto index = first; index < last; ++index) {
            std::uint32_t current = 0;
            std::memcpy(&current, reinterpret_cast<const void*>(words[index].first), sizeof(current));
            if (current != words[index].second) return false;
        }
        first = last;
    }
    for (const auto& [address, value] : words) memory.Insert(address, value);
    return true;
}

void CaptureMemo::Clear() {
    entries.clear();
    programs.clear();
}

}
