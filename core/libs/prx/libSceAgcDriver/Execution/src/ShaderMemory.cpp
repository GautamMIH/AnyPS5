#include <cstdio>
#include <array>
#include <chrono>
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "CacheKey.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace AgcDriver {

ShaderMemory::ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> initial) {
    const ShaderRecompiler::RequestMemoryView validated(initial);
    for (const auto& region : initial) {
        regions.emplace(region.guestAddress, Region(std::vector<std::byte>(region.bytes.begin(), region.bytes.end())));
        initialRanges.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
    }
}

ShaderMemory::ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> initial, std::vector<std::shared_ptr<const void>> owners) : owners(std::move(owners)) {
    const ShaderRecompiler::RequestMemoryView validated(initial);
    for (const auto& region : initial) {
        regions.emplace(region.guestAddress, Region(region.bytes));
        initialRanges.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
    }
}

namespace {
thread_local ShaderMemory::LineReader lineReader = nullptr;
thread_local void* lineReaderContext = nullptr;
}

void ShaderMemory::SetLineReader(LineReader reader, void* context) {
    lineReader = reader;
    lineReaderContext = context;
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
    if (lineReader == nullptr || !lineReader(lineReaderContext, begin, bytes)) GuestMemory::Read(begin, bytes, 1);
    std::memcpy(value, bytes.data() + (address - begin), sizeof(*value));
    self.regions.emplace(begin, Region(std::move(bytes)));
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
    result.accessible = &accessible;
    return result;
}

// A read through a null nested pointer (a table entry the shader's taken paths never use, as in
// Astro Bot's first dispatches) is inaccessible: the evaluator poisons it (zero words), as it does
// reads through a null user-data root, instead of the capture failing the submission.
// So is a pointer into a mapped but unreadable range (a guard or a reservation, as upstream's
// guarded-pointer captures): the check is the one the read itself makes next, so a readable range
// costs nothing extra, and only a refused one pays for the exception.
bool ShaderMemory::accessible(void*, std::uint64_t address, std::uint64_t bytes) {
    constexpr std::uint64_t NullPageEnd = 0x10000;
    if (address < NullPageEnd || bytes == 0) return false;
    try {
        GuestMemory::CheckRange(reinterpret_cast<const void*>(address), static_cast<std::size_t>(bytes), 1u);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::Capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle) {
    if (handle != nullptr) return ShaderRecompiler::CaptureResources(request, runtime(request), *handle);
    return ShaderRecompiler::CaptureResources(request, runtime(request));
}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::Regions() const {
    std::vector<ShaderRecompiler::MemoryRegion> result;
    result.reserve(regions.size());
    for (const auto& [address, bytes] : regions) {
        result.push_back({address, bytes.view});
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
    regions.emplace(address, Region(std::move(bytes)));
}

void ShaderMemory::InsertRun(std::span<const std::pair<std::uint64_t, std::uint32_t>> words) {
    if (words.empty()) return;
    const auto begin = words.front().first;
    const auto end = words.back().first + sizeof(std::uint32_t);
    const auto next = regions.upper_bound(begin);
    const bool overlapsPrevious = next != regions.begin() && std::prev(next)->first + std::prev(next)->second.size() > begin;
    const bool overlapsNext = next != regions.end() && next->first < end;
    if (overlapsPrevious || overlapsNext) {
        for (const auto& [address, value] : words) Insert(address, value);
        return;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end - begin));
    for (const auto& [address, value] : words) std::memcpy(bytes.data() + (address - begin), &value, sizeof(value));
    regions.emplace_hint(next, begin, Region(std::move(bytes)));
}

std::uint64_t CaptureMemo::programKey(const std::shared_ptr<const void>& owner, const ShaderRecompiler::RecompileRequest& request) {
    key.clear();
    key.push_back(reinterpret_cast<std::uintptr_t>(owner.get()));
    key.push_back(request.shader.codeAddress);
    key.push_back(request.shader.code.size());
    key.push_back(request.shader.headerAddress);
    ShaderRecompiler::RecompileCacheKey::BuildContext(request, key);
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : key) hash = (hash ^ word) * 0x100000001b3ull;
    return hash;
}

void CaptureMemo::storeProgram(std::uint64_t hash, const std::shared_ptr<const void>& owner, std::size_t programWords, const ShaderRecompiler::ResourceCapture& capture) {
    if (programs.size() >= 4096) programs.clear();
    auto handle = capture.source != nullptr ? std::make_shared<const ShaderRecompiler::SourceHandle>(ShaderRecompiler::SourceHandle{capture.source, nullptr, {}}) : nullptr;
    programs[hash] = Program{owner, std::vector<std::uint64_t>(key.begin(), key.begin() + static_cast<std::ptrdiff_t>(programWords)), std::move(handle), capture.plan ? ShaderRecompiler::PlanUserDataSlots(*capture.plan) : std::vector<std::uint32_t>{}, capture.plan != nullptr};
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> CaptureMemo::CaptureProgram(const std::shared_ptr<const void>& owner, const ShaderRecompiler::RecompileRequest& request, ShaderMemory& memory, bool* prepared) {
    if (prepared != nullptr) *prepared = false;
    if (owner == nullptr) return memory.Capture(request);
    const auto hash = programKey(owner, request);
    const auto program = programs.find(hash);
    if (program != programs.end() && program->second.owner == owner && program->second.key == key && program->second.handle != nullptr) {
        if (prepared != nullptr) *prepared = true;
        return memory.Capture(request, program->second.handle.get());
    }
    auto capture = memory.Capture(request);
    storeProgram(hash, owner, key.size(), *capture);
    return capture;
}

namespace {

// Debug aid: APS5_TRACE_CAPTURE_MEMO=1 counts the memo's outcomes every 10 s: hits, hits with patched
// user data, walks (known program, new user data), misses, changed entries (same key, other words) and
// clears (the memo full).
void noteMemo(int outcome) {
    static const bool trace = std::getenv("APS5_TRACE_CAPTURE_MEMO") != nullptr;
    if (!trace) return;
    static std::array<std::uint64_t, 6> counts{};
    static auto window = std::chrono::steady_clock::now();
    ++counts[static_cast<std::size_t>(outcome)];
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(10)) return;
    std::fprintf(stderr, "[capture-memo] hit %llu patched %llu walk %llu miss %llu changed %llu clears %llu\n", static_cast<unsigned long long>(counts[0]), static_cast<unsigned long long>(counts[1]), static_cast<unsigned long long>(counts[2]), static_cast<unsigned long long>(counts[3]), static_cast<unsigned long long>(counts[4]), static_cast<unsigned long long>(counts[5]));
    counts = {};
    window = now;
}

}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> CaptureMemo::Capture(const std::shared_ptr<const void>& owner, const ShaderRecompiler::RecompileRequest& request, ShaderMemory& memory) {
    if (owner == nullptr) return memory.Capture(request);
    std::uint64_t hash = programKey(owner, request);
    const auto programHash = hash;
    const auto programWords = key.size();
    PerformanceTimer timing("Driver.CaptureMemo");
    // The program's resolved source (same code, context and owner), whatever its user data.
    const auto program = programs.find(programHash);
    const bool sameProgram = program != programs.end() && program->second.owner == owner && program->second.key.size() == programWords && std::equal(program->second.key.begin(), program->second.key.end(), key.begin());
    // A program whose captures rarely repeat is walked without a lookup or a stored entry (copying its
    // words and keeping the entry cost more than the rare hit saves); after 4096 such draws it is
    // probed again. ANYPS5_CAPTURE_MEMO_ALWAYS=1 stores every capture.
    static const bool always = std::getenv("ANYPS5_CAPTURE_MEMO_ALWAYS") != nullptr;
    constexpr std::uint32_t ProbeLookups = 64;
    if (sameProgram && !always) {
        auto& stats = program->second;
        if (stats.lookups >= ProbeLookups && stats.hits * 8u < stats.lookups) {
            if (++stats.skipped < 4096u) {
                noteMemo(2);
                timing.Mark("cold_program");
                return memory.Capture(request, stats.handle.get());
            }
            stats.lookups = stats.hits = stats.skipped = 0;
        }
    }
    static const bool allUserData = std::getenv("ANYPS5_CAPTURE_MEMO_ALL_USER_DATA") != nullptr;
    const auto& userData = request.context.userData;
    // Keyed on the user-data slots the plan reads once the program is known (a slot count marks
    // the form), else on every word.
    if (sameProgram && !allUserData && program->second.masked) {
        const auto& slots = program->second.slots;
        key.push_back(0x51075u + slots.size());
        for (const auto slot : slots) {
            const auto word = slot < userData.size() ? static_cast<std::uint64_t>(userData[slot]) : ~0ull;
            key.push_back(word);
        }
    } else {
        key.insert(key.end(), userData.begin(), userData.end());
    }
    for (auto index = programWords; index < key.size(); ++index) hash = (hash ^ key[index]) * 0x100000001b3ull;
    const auto found = entries.find(hash);
    const bool known = found != entries.end() && found->second.key == key && found->second.owner == owner;
    if (sameProgram) ++program->second.lookups;
    if (known && reusable(found->second, memory)) {
        if (sameProgram) ++program->second.hits;
        timing.Mark("hit");
        const auto& capture = found->second.capture;
        // Only the snapshot's copy of the user data may differ from this draw's.
        const auto& copied = capture->snapshot.userData;
        const bool same = copied.size() <= userData.size() && std::equal(copied.begin(), copied.end(), userData.begin());
        noteMemo(same ? 0 : 1);
        if (same) return capture;
        auto patched = std::make_shared<ShaderRecompiler::ResourceCapture>(*capture);
        patched->snapshot.userData.assign(userData.begin(), userData.begin() + static_cast<std::ptrdiff_t>(copied.size()));
        timing.Mark("hit_patched");
        return patched;
    }
    // Taken before the capture reads, so a mapping change during it invalidates the entry.
    const auto mappingGeneration = GuestMemoryBacking::GuestMemoryBackingGeneration_nid_postfix();
    // The same program and context captured with other user data: its source and plan are reused.
    timing.Mark("program_lookup");
    auto capture = sameProgram ? memory.Capture(request, program->second.handle.get()) : memory.Capture(request);
    timing.Mark("materialize");
    if (entries.size() >= 16384) {
        noteMemo(5);
        if (retireDropped) retireDropped(std::make_shared<decltype(entries)>(std::move(entries)));
        entries = {};
    }
    auto words = memory.CapturedWords();
    timing.Mark("captured_words");
    auto& slot = entries[hash];
    if (retireDropped && slot.capture) retireDropped(std::make_shared<Entry>(std::move(slot)));
    slot = Entry{owner, key, capture, std::move(words), mappingGeneration};
    noteMemo(known ? 4 : sameProgram ? 2 : 3);
    timing.Mark("store");
    if (!sameProgram) storeProgram(programHash, owner, programWords, *capture);
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
    // Runs of adjacent words become one region each (a node and a buffer per word made hits slow).
    for (std::size_t first = 0; first < words.size();) {
        auto last = first + 1;
        while (last < words.size() && words[last].first == words[last - 1].first + sizeof(std::uint32_t)) ++last;
        memory.InsertRun(std::span(words).subspan(first, last - first));
        first = last;
    }
    return true;
}

void CaptureMemo::Clear() {
    if (retireDropped && !entries.empty()) retireDropped(std::make_shared<decltype(entries)>(std::move(entries)));
    entries = {};
    programs.clear();
}

}
