#ifndef CODEGEN_X86_SHA1OPERANDS_HPP
#define CODEGEN_X86_SHA1OPERANDS_HPP

#include <cstddef>
#include <cstdint>

namespace Codegen {

enum class Sha1Operation : std::uint8_t {
    Rnds4,
    Nexte,
    Msg1,
    Msg2
};

struct Sha1Operands {
    Sha1Operation Operation;
    std::uint8_t Destination;
    std::uint8_t Source;
    std::uint8_t Function;
};

[[nodiscard]] Sha1Operands DecodeSha1(const std::uint8_t* data, std::size_t length);

}

#endif
