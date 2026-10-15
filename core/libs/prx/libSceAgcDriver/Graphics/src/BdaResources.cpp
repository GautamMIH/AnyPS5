#include "prx/libSceAgcDriver/Graphics/include/BdaResources.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include "ImageTableAbi.hpp"
#include <iomanip>
#include <algorithm>
#include <bit>
#include <cstring>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace AgcDriver::Graphics {

BdaResources::BdaResources(const Context& context) {
    static_assert(std::endian::native == std::endian::little);
    Require(ShaderRecompiler::BdaAbi::FaultBufferBytes <= context.limits.maxStorageBufferRange, "BDA fault buffer exceeds storage buffer range limit");
    fault = std::make_unique<Buffer>(context, ShaderRecompiler::BdaAbi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(fault->Bytes().data(), 0, fault->Bytes().size());
}

BdaResources::BdaResources(const Context& context, const GuestBufferMemory& memory) : BdaResources(context) {
    const auto ranges = memory.AddressRanges();
    Require(ranges.size() <= std::numeric_limits<std::uint32_t>::max(), "BDA table range count overflow");
    Require(ranges.size() <= (std::numeric_limits<std::size_t>::max() - sizeof(ShaderRecompiler::BdaAbi::Header)) / sizeof(ShaderRecompiler::BdaAbi::Range), "BDA table size overflow");
    tableBytes = sizeof(ShaderRecompiler::BdaAbi::Header) + ranges.size() * sizeof(ShaderRecompiler::BdaAbi::Range);
    Require(tableBytes <= context.limits.maxStorageBufferRange && ShaderRecompiler::BdaAbi::FaultBufferBytes <= context.limits.maxStorageBufferRange, "BDA descriptors exceed storage buffer range limit");
    table = std::make_unique<Buffer>(context, tableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const ShaderRecompiler::BdaAbi::Header header{ShaderRecompiler::BdaAbi::Version, static_cast<std::uint32_t>(ranges.size()), sizeof(ShaderRecompiler::BdaAbi::Range), 0};
    std::memcpy(table->Bytes().data(), &header, sizeof(header));
    if (!ranges.empty()) std::memcpy(table->Bytes().data() + sizeof(header), ranges.data(), ranges.size() * sizeof(ranges.front()));
    constexpr auto flagged = ShaderRecompiler::BdaAbi::Absent | ShaderRecompiler::BdaAbi::Tracked;
    if (context.guestGpuMemory != nullptr && std::any_of(ranges.begin(), ranges.end(), [](const auto& range) { return (range.permissions & flagged) != 0; })) {
        noted = ranges;
        gpuMemory = context.guestGpuMemory;
    }
}

VkDescriptorBufferInfo BdaResources::Table() const {
    Require(table != nullptr, "BDA page table was not requested");
    return {table->Handle(), 0, tableBytes};
}

VkDescriptorBufferInfo BdaResources::Fault() const {
    return {fault->Handle(), 0, ShaderRecompiler::BdaAbi::FaultBufferBytes};
}

void BdaResources::CheckFault() const {
    markWrittenPages();
    ShaderRecompiler::BdaAbi::Fault report{};
    std::memcpy(&report, fault->Bytes().data(), sizeof(report));
    if (!noted.empty() && notedEntries(report)) return;
    if (report.state == ShaderRecompiler::BdaAbi::FaultState::Empty) {
        Require(static_cast<std::uint32_t>(report.reason) == 0 && report.address == 0 && report.bytes == 0 && report.stage == 0 && report.instruction == 0 && report.reserved == 0, "BDA fault record has data without publication");
        return;
    }
    Require(report.state == ShaderRecompiler::BdaAbi::FaultState::Ready && report.reserved == 0, "incomplete or invalid BDA fault record");
    Require(report.reason != ShaderRecompiler::BdaAbi::FaultReason::InvalidRectangle, "rect-list requires finite nondegenerate axis-aligned positions with equal positive W");
    if (report.reason == ShaderRecompiler::BdaAbi::FaultReason::ImageTable) {
        // The shader selected a table entry the recompiler or this driver poisoned: report.bytes is
        // the poison entry's index.
        const auto found = std::find_if(imageTables.begin(), imageTables.end(), [&](const ImageTableFaults& candidate) { return candidate.stage == report.stage; });
        const auto* tables = found != imageTables.end() ? &*found : imageTables.size() == 1 ? &imageTables.front() : nullptr;
        std::ostringstream message;
        message << "image table: shader 0x" << std::hex << (tables != nullptr ? tables->shader : 0u) << std::dec << " stage " << report.stage << " pc 0x" << std::hex << report.instruction << " selected the descriptor at 0x" << report.address << std::dec;
        if (tables != nullptr && report.bytes < tables->poison.size()) {
            const auto& poison = tables->poison[report.bytes];
            if (poison.dwordCount != 0) {
                message << " (words";
                for (std::uint32_t index = 0; index < poison.dwordCount && index < poison.words.size(); ++index) message << ' ' << std::hex << std::setw(8) << std::setfill('0') << poison.words[index];
                message << std::dec << ")";
            }
            message << ": " << ShaderRecompiler::ImageTableAbi::PoisonReasonName(static_cast<ShaderRecompiler::ImageTableAbi::PoisonReason>(poison.reason));
            if (report.bytes < tables->messages.size() && !tables->messages[report.bytes].empty()) message << ": " << tables->messages[report.bytes];
        } else {
            message << ": poison entry " << report.bytes;
        }
        throw std::runtime_error(message.str());
    }
    if (report.reason == ShaderRecompiler::BdaAbi::FaultReason::UnlinkedCall) {
        // A computed call (s_swappc_b64 to a target loaded at run time) whose target no linked callee
        // matches: report.address is the target, report.instruction the call (upstream 9c8fb780).
        std::ostringstream message;
        message << "shader stage " << report.stage << " called address 0x" << std::hex << report.address << " at program counter 0x" << report.instruction << ", which no linked callee of that call matches";
        throw std::runtime_error(message.str());
    }
    std::ostringstream message;
    message << "BDA access failed: address=0x" << std::hex << report.address << " instruction=0x" << report.instruction << std::dec << " bytes=" << report.bytes << " stage=" << report.stage << " reason=" << static_cast<std::uint32_t>(report.reason);
    // Name the host mapping holding the address, to tell unregistered guest memory from garbage.
    std::ifstream maps("/proc/self/maps");
    std::string line;
    bool mapped = false;
    while (std::getline(maps, line)) {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        if (std::sscanf(line.c_str(), "%" SCNx64 "-%" SCNx64, &begin, &end) == 2 && report.address >= begin && report.address < end) {
            message << " (host mapping " << line << ")";
            mapped = true;
            break;
        }
    }
    if (!mapped) message << " (no host mapping)";
    // The table ranges around the address.
    if (table != nullptr) {
        ShaderRecompiler::BdaAbi::Header header{};
        std::memcpy(&header, table->Bytes().data(), sizeof(header));
        const auto* ranges = reinterpret_cast<const ShaderRecompiler::BdaAbi::Range*>(table->Bytes().data() + sizeof(header));
        message << " (" << header.count << " table ranges";
        for (std::uint32_t i = 0; i < header.count; ++i) {
            const auto& range = ranges[i];
            const auto near = range.end + 0x100000u > report.address && range.begin < report.address + 0x100000u;
            if (near) message << std::hex << " [0x" << range.begin << ", 0x" << range.end << ")" << std::dec;
        }
        message << ")";
    }
    throw std::runtime_error(message.str());
}

}

namespace AgcDriver::Graphics {

// ponytail: the pages are marked once the work completed, so a CPU read of them before that (a
// capture or texture upload of later work in the same batch) is not made to wait for the stores,
// as descriptor-bound writes are (Recorder::NotePendingWrites); note the V#s' ranges at record
// time if that shows up.
void BdaResources::SetImageTables(std::vector<ImageTableFaults> tables, std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges) {
    imageTables = std::move(tables);
    imageTableRanges = std::move(ranges);
}

void BdaResources::markWrittenPages() const {
    namespace Abi = ShaderRecompiler::BdaAbi;
    auto* words = reinterpret_cast<std::uint32_t*>(fault->Bytes().data());
    Require(words[Abi::WrittenOverflowWord] == 0, "more than " + std::to_string(Abi::WrittenPageSlots) + " pages stored to through GPU-selected buffer descriptors in one use are not implemented");
    bool any = false;
    for (std::uint32_t slot = 0; slot < Abi::WrittenPageSlots; ++slot) {
        const auto page = words[Abi::WrittenSlotsWord + slot];
        if (page == 0) continue;
        const auto begin = static_cast<std::uint64_t>(page - 1u) << Abi::WrittenPageShift;
        const auto end = begin + (std::uint64_t{1} << Abi::WrittenPageShift);
        // A table the work selects descriptors from was resolved before it ran: a store to it would
        // change what the hardware selects mid-dispatch.
        for (const auto& [base, size] : imageTableRanges) {
            if (size == 0 || begin >= base + size || base >= end) continue;
            std::ostringstream message;
            message << "image table at 0x" << std::hex << base << " is written by its own dispatch";
            Require(false, message.str());
        }
        WriteTracker::NoteGpuWrite(static_cast<std::uint64_t>(page - 1u) << Abi::WrittenPageShift, std::uint64_t{1} << Abi::WrittenPageShift);
        any = true;
    }
    if (any) std::memset(words + Abi::WrittenSlotsWord, 0, Abi::WrittenPageSlots * sizeof(std::uint32_t));
}


bool BdaResources::notedEntries(const ShaderRecompiler::BdaAbi::Fault& report) const {
    namespace Abi = ShaderRecompiler::BdaAbi;
    auto* words = reinterpret_cast<std::uint32_t*>(fault->Bytes().data());
    static const bool trace = std::getenv("APS5_TRACE_REGISTERED_FAULTS") != nullptr;
    const auto count = words[Abi::NotedCountWord];
    const bool dropped = words[Abi::NotedWriteWord] != 0;
    std::vector<std::uint32_t> entries;
    if (count > Abi::NotedListSlots) {
        for (std::uint32_t word = 0; word < Abi::NotedEntries / 32; ++word) {
            auto bits = words[Abi::NotedBitsWord + word];
            while (bits != 0) {
                entries.push_back(word * 32u + static_cast<std::uint32_t>(std::countr_zero(bits)));
                bits &= bits - 1u;
            }
        }
    } else {
        entries.assign(words + Abi::NotedListWord, words + Abi::NotedListWord + count);
    }
    for (const auto entry : entries) {
        if (entry >= noted.size()) continue;
        const auto& range = noted[entry];
        if ((range.permissions & Abi::Absent) != 0) {
            const bool imported = gpuMemory->FaultIn(range.begin, range.end - range.begin);
            if (trace) std::fprintf(stderr, "[bda] absent registered memory 0x%llx+0x%llx reached%s%s\n", static_cast<unsigned long long>(range.begin), static_cast<unsigned long long>(range.end - range.begin), imported ? "; imported for later work" : "", dropped ? " (stores of the work to absent memory were dropped)" : "");
        } else if ((range.permissions & Abi::Tracked) != 0) {
            gpuMemory->Touch(range.begin);
        }
    }
    if (dropped && !trace) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) std::fprintf(stderr, "[AnyPS5] a shader stored to registered memory not yet imported for the GPU; the store was dropped (imported for later work; APS5_TRACE_REGISTERED_FAULTS=1 lists them)\n");
    }
    if (count != 0 || dropped) {
        words[Abi::NotedWriteWord] = 0;
        words[Abi::NotedCountWord] = 0;
        std::memset(words + Abi::NotedBitsWord, 0, Abi::NotedEntries / 8);
    }
    // An access the shader could not note reaching Absent memory: imported, not an error. Past the
    // noted entries it faults Permission (an Absent entry grants nothing); a lookup that records its
    // own faults (a probe's caller), or one reaching from an imported entry into an Absent one,
    // faults Unmapped.
    if (report.state != Abi::FaultState::Ready || (report.reason != Abi::FaultReason::Unmapped && report.reason != Abi::FaultReason::Permission)) return false;
    const auto begin = report.address;
    const auto end = report.address + std::max<std::uint64_t>(report.bytes, 1u);
    bool absent = false;
    for (const auto& range : noted) {
        if ((range.permissions & Abi::Absent) == 0 || range.end <= begin || end <= range.begin) continue;
        absent = true;
        gpuMemory->FaultIn(range.begin, range.end - range.begin);
    }
    if (!absent) return false;
    if (trace) std::fprintf(stderr, "[bda] fault %u at 0x%llx (%u bytes, pc 0x%x) reached absent registered memory; imported for later work\n", static_cast<std::uint32_t>(report.reason), static_cast<unsigned long long>(report.address), report.bytes, report.instruction);
    std::memset(words, 0, sizeof(Abi::Fault));
    return true;
}

}
