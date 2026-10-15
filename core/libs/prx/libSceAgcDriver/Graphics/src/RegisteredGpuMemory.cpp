#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"

namespace AgcDriver::Graphics {

void GuestBufferMemory::AcquireRegistered(bool writes) {
    Require(!uploaded && regions.empty() && lease.empty(), "guest allocation lease must precede resource registration");
    lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
    regions.reserve(lease.size());
    if (context.guestGpuMemory != nullptr) {
        std::uint64_t registered = 0;
        for (const auto& range : lease) registered += range->bytes;
        faultDriven = context.guestGpuMemory->FaultDriven(registered);
        if (faultDriven) static_cast<void>(gpuLease());
    }
    std::vector<GuestGpuMemory::RegisteredPiece> pieces;
    for (const auto& range : lease) {
        validate(range->address, range->bytes);
        // Fault-driven: chunk by chunk, those not imported Absent (the executable's ranges are copied).
        if (faultDriven && range->releasable) {
            pieces.clear();
            if (context.guestGpuMemory->RegisteredPieces(range->address, range->bytes, pieces)) {
                for (const auto& piece : pieces) {
                    regions.push_back({piece.begin, piece.end, range->writable && writes, {}, nullptr, true, std::nullopt, nullptr});
                    regions.back().flags = piece.flags;
                }
                continue;
            }
        }
        // With a mirror, registered memory is used in place instead of captured.
        if (context.guestGpuMemory != nullptr) {
            regions.push_back({range->address, range->address + range->bytes, range->writable && writes, {}, nullptr, true, std::nullopt, range->releasable ? nullptr : range});
            continue;
        }
        std::vector<std::byte> snapshot;
        if (!range->writable) {
            snapshot.resize(range->bytes);
            try {
                GuestMemory::Read(range->address, snapshot);
            } catch (const std::exception& error) {
                char context[96];
                std::snprintf(context, sizeof(context), " (snapshotting registered read-only range 0x%llx+0x%llx)", static_cast<unsigned long long>(range->address), static_cast<unsigned long long>(range->bytes));
                throw std::runtime_error(std::string(error.what()) + context);
            }
        }
        regions.push_back({range->address, range->address + range->bytes, range->writable, std::move(snapshot), nullptr});
    }
}

std::uint64_t GuestBufferMemory::gpuLease() {
    if (!faultDriven || context.guestGpuMemory == nullptr) return 0;
    if (gpuLeaseToken.serial == 0) {
        gpuLeaseToken.memory = context.guestGpuMemory;
        gpuLeaseToken.serial = context.guestGpuMemory->BeginLease();
    }
    return gpuLeaseToken.serial;
}

}
