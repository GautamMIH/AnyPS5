#ifndef ELFPATCHER_DOMAIN_IENTRYSTUBBUILDER_HPP
#define ELFPATCHER_DOMAIN_IENTRYSTUBBUILDER_HPP

#include <cstdint>
#include <vector>

namespace Elfpatcher {

class IEntryStubBuilder {
public:
    virtual ~IEntryStubBuilder() = default;

    virtual std::vector<std::uint8_t> BuildEntryStub(
        std::uint64_t stubVaddr,
        std::uint64_t realEntryVaddr
    ) const = 0;

    virtual std::vector<std::uint8_t> BuildNullArgumentCallStub(
        std::uint64_t stubVaddr,
        std::uint64_t targetVaddr
    ) const = 0;

    // Library init: tail-calls the runtime's module-init hook (read from a GOT slot) with the PS5
    // init entry, or calls the entry with null arguments when the hook is absent.
    virtual std::vector<std::uint8_t> BuildModuleInitStub(
        std::uint64_t stubVaddr,
        std::uint64_t targetVaddr,
        std::uint64_t hookSlotVaddr
    ) const = 0;
};

}

#endif
