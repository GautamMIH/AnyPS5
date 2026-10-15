#ifndef CORE_SHADER_RECOMPILER_BDAABI_HPP
#define CORE_SHADER_RECOMPILER_BDAABI_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ShaderRecompiler::BdaAbi {

inline constexpr std::uint32_t Version = 1;
inline constexpr std::uint32_t Read = 1;
inline constexpr std::uint32_t Write = 2;
// Registered memory the host has not imported for the GPU: an access notes the entry (below) and
// reads nothing (stores are dropped) instead of faulting; the host imports it for later work.
inline constexpr std::uint32_t Absent = 4;
// An imported entry the host considers releasing: an access notes the entry, which keeps it.
inline constexpr std::uint32_t Tracked = 8;

struct Header {
    std::uint32_t version;
    std::uint32_t count;
    std::uint32_t entryBytes;
    std::uint32_t reserved;
};

struct Range {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t deviceAddress;
    std::uint32_t permissions;
    std::uint32_t reserved;
};

enum class FaultState : std::uint32_t { Empty, Writing, Ready };
enum class FaultReason : std::uint32_t { Unmapped = 1, Permission, Overflow, InvalidTable, InvalidRectangle, LoopLimit, Unaligned, InvalidDescriptor, ImageTable, UnlinkedCall };

struct Fault {
    FaultState state;
    FaultReason reason;
    std::uint64_t address;
    std::uint32_t bytes;
    std::uint32_t stage;
    std::uint32_t instruction;
    std::uint32_t reserved;
};

static_assert(std::is_standard_layout_v<Header> && std::is_trivially_copyable_v<Header> && sizeof(Header) == 16);
static_assert(std::is_standard_layout_v<Range> && std::is_trivially_copyable_v<Range> && sizeof(Range) == 32);
static_assert(offsetof(Range, deviceAddress) == 16 && offsetof(Range, permissions) == 24);
static_assert(std::is_standard_layout_v<Fault> && std::is_trivially_copyable_v<Fault> && sizeof(Fault) == 32);
static_assert(offsetof(Fault, address) == 8 && offsetof(Fault, instruction) == 24);

// After the Fault record, the fault buffer holds the pages the shader stored to through the table
// (Write permission), for the host to mark GPU-written once the work completed: an overflow word,
// then an open-addressed set of page number + 1 (0 is free), probed WrittenPageProbes slots from a
// multiplicative hash. A store that finds no slot sets the overflow word.
inline constexpr std::uint32_t WrittenPageShift = 12;
inline constexpr std::uint32_t WrittenChunkShift = 17;
inline constexpr std::uint32_t WrittenChunkPages = 1u << (WrittenChunkShift - WrittenPageShift);
inline constexpr std::uint32_t WrittenPageSlots = 2048;
inline constexpr std::uint32_t WrittenPageProbes = 8;
inline constexpr std::uint64_t WrittenAddressLimit = std::uint64_t{0xffffffffu} << WrittenChunkShift;
inline constexpr std::uint32_t WrittenOverflowWord = sizeof(Fault) / sizeof(std::uint32_t);
inline constexpr std::uint32_t WrittenCountWord = WrittenOverflowWord + 1;
inline constexpr std::uint32_t WrittenSlotsWord = WrittenCountWord + 1;
inline constexpr std::uint32_t WrittenListWord = WrittenSlotsWord + 2 * WrittenPageSlots;
// After the written set: the table entries with Absent or Tracked accessed by the work. A bitmap of
// entry indices (entries from NotedEntries on are not noted: an Absent one faults Unmapped), and
// the list of indices in the order their bits were set, NotedCountWord counting them (more than
// NotedListSlots: the host scans the bitmap). NotedWriteWord is set by a store or atomic to an
// Absent entry (the store was dropped).
inline constexpr std::uint32_t NotedEntries = 4096;
inline constexpr std::uint32_t NotedListSlots = 1024;
inline constexpr std::uint32_t NotedWriteWord = WrittenListWord + WrittenPageSlots;
inline constexpr std::uint32_t NotedCountWord = NotedWriteWord + 1;
inline constexpr std::uint32_t NotedBitsWord = NotedCountWord + 1;
inline constexpr std::uint32_t NotedListWord = NotedBitsWord + NotedEntries / 32;
inline constexpr std::size_t FaultBufferBytes = (NotedListWord + NotedListSlots) * sizeof(std::uint32_t);
static_assert(NotedEntries % 32 == 0);
static_assert((WrittenPageSlots & (WrittenPageSlots - 1)) == 0);
static_assert(WrittenChunkPages == 32);

inline constexpr std::uint32_t WrittenChunkKey(std::uint64_t address) {
    return static_cast<std::uint32_t>(address >> WrittenChunkShift) + 1u;
}

template<typename TMark>
bool ForEachWrittenRange(const std::uint32_t* words, TMark&& mark) {
    const auto count = words[WrittenCountWord];
    if (count > WrittenPageSlots) return false;
    for (std::uint32_t claim = 0; claim < count; ++claim) {
        const auto slot = words[WrittenListWord + claim];
        if (slot >= WrittenPageSlots || words[WrittenSlotsWord + 2 * slot] == 0) return false;
    }
    for (std::uint32_t claim = 0; claim < count; ++claim) {
        const auto slot = words[WrittenListWord + claim];
        const auto key = words[WrittenSlotsWord + 2 * slot];
        auto mask = words[WrittenSlotsWord + 2 * slot + 1];
        const auto base = static_cast<std::uint64_t>(key - 1u) << WrittenChunkShift;
        while (mask != 0) {
            const auto first = static_cast<std::uint32_t>(std::countr_zero(mask));
            const auto pages = static_cast<std::uint32_t>(std::countr_one(mask >> first));
            mark(base + (static_cast<std::uint64_t>(first) << WrittenPageShift), static_cast<std::size_t>(pages) << WrittenPageShift);
            mask = pages + first >= 32u ? 0u : mask & ~((1u << (first + pages)) - 1u);
        }
    }
    return true;
}

}

#endif
