#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"

namespace AgcDriver::Graphics {

void GuestBufferMemory::AcquireRegistered() {
    Require(!uploaded && regions.empty() && lease.empty(), "guest allocation lease must precede resource registration");
    lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
    regions.reserve(lease.size());
    for (const auto& range : lease) {
        validate(range->address, range->bytes);
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

}
