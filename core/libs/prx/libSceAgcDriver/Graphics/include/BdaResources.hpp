#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BDARESOURCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BDARESOURCES_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "Recompiler.hpp"
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class BdaResources {
public:
    explicit BdaResources(const Context& context);
    BdaResources(const Context& context, const GuestBufferMemory& memory);
    VkDescriptorBufferInfo Table() const;
    VkDescriptorBufferInfo Fault() const;
    void CheckFault() const;
    // The image tables of the work, per stage: the poison entries (and the driver's messages for the
    // entries it rejected) a table fault names by index (BdaAbi::FaultReason::ImageTable), and the
    // guest ranges of the tables, which the work must not store to through the BDA table.
    struct ImageTableFaults {
        std::uint32_t stage = 0;
        std::uint64_t shader = 0;
        std::vector<ShaderRecompiler::ImageTableEntryPoison> poison;
        std::vector<std::string> messages;
    };
    void SetImageTables(std::vector<ImageTableFaults> tables, std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges);

private:
    // The pages stores through the table noted in the fault buffer (BdaAbi::WrittenPageSlots) are
    // marked GPU-written, and the set emptied; CheckFault does it first.
    void markWrittenPages() const;
    // Fault-driven registered memory: imports the Absent entries the work reached and keeps the
    // Tracked ones it used (BdaAbi::NotedEntries); returns whether a fault record of an access to
    // Absent memory (Unmapped) was such a request, so not an error.
    bool notedEntries(const ShaderRecompiler::BdaAbi::Fault& report) const;
    // The table's entries when some are Absent or Tracked (else empty), and who imports them.
    std::vector<ShaderRecompiler::BdaAbi::Range> noted;
    GuestGpuMemory* gpuMemory = nullptr;
    std::unique_ptr<Buffer> table;
    std::unique_ptr<Buffer> fault;
    std::size_t tableBytes = 0;
    std::vector<ImageTableFaults> imageTables;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> imageTableRanges;
};

}

#endif
