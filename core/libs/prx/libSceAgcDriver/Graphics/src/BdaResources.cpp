#include "prx/libSceAgcDriver/Graphics/include/BdaResources.hpp"
#include "prx/libSceAgcDriver/Execution/include/WriteTracker.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
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
    if (report.state == ShaderRecompiler::BdaAbi::FaultState::Empty) {
        Require(static_cast<std::uint32_t>(report.reason) == 0 && report.address == 0 && report.bytes == 0 && report.stage == 0 && report.instruction == 0 && report.reserved == 0, "BDA fault record has data without publication");
        return;
    }
    Require(report.state == ShaderRecompiler::BdaAbi::FaultState::Ready && report.reserved == 0, "incomplete or invalid BDA fault record");
    Require(report.reason != ShaderRecompiler::BdaAbi::FaultReason::InvalidRectangle, "rect-list requires finite nondegenerate axis-aligned positions with equal positive W");
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
void BdaResources::markWrittenPages() const {
    namespace Abi = ShaderRecompiler::BdaAbi;
    auto* words = reinterpret_cast<std::uint32_t*>(fault->Bytes().data());
    Require(words[Abi::WrittenOverflowWord] == 0, "more than " + std::to_string(Abi::WrittenPageSlots) + " pages stored to through GPU-selected buffer descriptors in one use are not implemented");
    bool any = false;
    for (std::uint32_t slot = 0; slot < Abi::WrittenPageSlots; ++slot) {
        const auto page = words[Abi::WrittenSlotsWord + slot];
        if (page == 0) continue;
        WriteTracker::NoteGpuWrite(static_cast<std::uint64_t>(page - 1u) << Abi::WrittenPageShift, std::uint64_t{1} << Abi::WrittenPageShift);
        any = true;
    }
    if (any) std::memset(words + Abi::WrittenSlotsWord, 0, Abi::WrittenPageSlots * sizeof(std::uint32_t));
}

}
