#ifndef RELINKER_DOMAIN_ISELFUNWRAPPER_HPP
#define RELINKER_DOMAIN_ISELFUNWRAPPER_HPP

#include <cstdint>
#include <vector>

namespace Relinker {

class ISelfUnwrapper {
public:
    virtual ~ISelfUnwrapper() = default;

    [[nodiscard]] virtual bool IsSelf(const std::vector<std::uint8_t>& fileBytes) const = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> Unwrap(const std::vector<std::uint8_t>& fileBytes) const = 0;
};

}

#endif
