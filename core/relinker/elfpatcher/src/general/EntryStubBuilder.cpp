#include <elfpatcher/general/EntryStubBuilder.hpp>
#include <elfpatcher/general/ElfConstants.hpp>
#include <domain/Types.hpp>
#include <cstdint>

namespace Elfpatcher {

namespace {
void _appendBytes(std::vector<std::uint8_t>& s, const std::uint8_t* bytes, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        s.push_back(bytes[i]);
}
}

std::vector<std::uint8_t> EntryStubBuilder::BuildEntryStub(
    const std::uint64_t stubVaddr,
    const std::uint64_t realEntryVaddr
) const {
    std::vector<std::uint8_t> s;
    s.push_back(kStubOpPopRax);
    _appendBytes(s, kStubOpMovRbxRsp, sizeof(kStubOpMovRbxRsp));
    _appendBytes(s, kStubOpSubRsp0x30, sizeof(kStubOpSubRsp0x30));
    _appendBytes(s, kStubOpAndRsp0xf0, sizeof(kStubOpAndRsp0xf0));
    _appendBytes(s, kStubOpMovDwordPtrRsp, sizeof(kStubOpMovDwordPtrRsp));
    _appendBytes(s, kStubOpMovQwordPtrRsp8Rbx, sizeof(kStubOpMovQwordPtrRsp8Rbx));
    _appendBytes(s, kStubOpMovRdiRsp, sizeof(kStubOpMovRdiRsp));
    _appendBytes(s, kStubOpXorRsiRsi, sizeof(kStubOpXorRsiRsi));
    const std::uint64_t callInsnVaddr = stubVaddr + s.size();
    const std::uint64_t callNextVaddr = callInsnVaddr + kStubCallInstructionSize;
    const auto rel32 = static_cast<std::int32_t>(realEntryVaddr - callNextVaddr);
    s.push_back(kStubOpCallRel32);
    s.push_back(static_cast<std::uint8_t>(rel32 & 0xff));
    s.push_back(static_cast<std::uint8_t>((rel32 >> 8) & 0xff));
    s.push_back(static_cast<std::uint8_t>((rel32 >> 16) & 0xff));
    s.push_back(static_cast<std::uint8_t>((rel32 >> 24) & 0xff));
    _appendBytes(s, kStubOpUd2, sizeof(kStubOpUd2));
    return s;
}

std::vector<std::uint8_t> EntryStubBuilder::BuildNullArgumentCallStub(
    const std::uint64_t stubVaddr,
    const std::uint64_t targetVaddr
) const {
    std::vector<std::uint8_t> s;
    s.push_back(kStubOpPushRbp);
    _appendBytes(s, kStubOpMovRbpRsp, sizeof(kStubOpMovRbpRsp));
    _appendBytes(s, kStubOpXorEdiEdi, sizeof(kStubOpXorEdiEdi));
    _appendBytes(s, kStubOpXorEsiEsi, sizeof(kStubOpXorEsiEsi));
    _appendBytes(s, kStubOpXorEdxEdx, sizeof(kStubOpXorEdxEdx));
    const std::uint64_t callNextVaddr = stubVaddr + s.size() + kStubCallInstructionSize;
    const auto delta = static_cast<std::int64_t>(targetVaddr - callNextVaddr);
    if (delta < INT32_MIN || delta > INT32_MAX)
        throw Domain::RelinkerException("Module call stub target is out of rel32 range", targetVaddr);
    const auto rel32 = static_cast<std::int32_t>(delta);
    s.push_back(kStubOpCallRel32);
    s.push_back(static_cast<std::uint8_t>(rel32 & 0xff));
    s.push_back(static_cast<std::uint8_t>((rel32 >> 8) & 0xff));
    s.push_back(static_cast<std::uint8_t>((rel32 >> 16) & 0xff));
    s.push_back(static_cast<std::uint8_t>((rel32 >> 24) & 0xff));
    s.push_back(kStubOpPopRbp);
    s.push_back(kStubOpRet);
    return s;
}

}
