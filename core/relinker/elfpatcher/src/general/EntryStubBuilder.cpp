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
    // The guest entry takes EntryParams in rdi. The process stack at entry already has that layout
    // (argc as a qword, then the argv pointers and a null; upstream d8b8aafc), so rdi is rsp: the
    // host's arguments reach the game.
    std::vector<std::uint8_t> s;
    _appendBytes(s, kStubOpMovRdiRsp, sizeof(kStubOpMovRdiRsp));
    _appendBytes(s, kStubOpAndRsp0xf0, sizeof(kStubOpAndRsp0xf0));
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

std::vector<std::uint8_t> EntryStubBuilder::BuildModuleInitStub(
    const std::uint64_t stubVaddr,
    const std::uint64_t targetVaddr,
    const std::uint64_t hookSlotVaddr
) const {
    const auto rel32 = [](std::uint64_t target, std::uint64_t next) {
        const auto delta = static_cast<std::int64_t>(target - next);
        if (delta < INT32_MIN || delta > INT32_MAX)
            throw Domain::RelinkerException("Module init stub reference is out of rel32 range", target);
        return static_cast<std::int32_t>(delta);
    };
    const auto appendRel32 = [](std::vector<std::uint8_t>& out, std::int32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    };
    std::vector<std::uint8_t> s;
    // mov rax, [rip + hook slot]
    _appendBytes(s, kStubOpMovRaxRipRel, sizeof(kStubOpMovRaxRipRel));
    appendRel32(s, rel32(hookSlotVaddr, stubVaddr + s.size() + 4));
    // test rax, rax; jz fallback
    _appendBytes(s, kStubOpTestRaxRax, sizeof(kStubOpTestRaxRax));
    s.push_back(kStubOpJzRel8);
    const std::size_t jumpOffset = s.size();
    s.push_back(0);
    // lea rdi, [rip + PS5 init entry]; jmp rax
    _appendBytes(s, kStubOpLeaRdiRipRel, sizeof(kStubOpLeaRdiRipRel));
    appendRel32(s, rel32(targetVaddr, stubVaddr + s.size() + 4));
    _appendBytes(s, kStubOpJmpRax, sizeof(kStubOpJmpRax));
    s[jumpOffset] = static_cast<std::uint8_t>(s.size() - jumpOffset - 1);
    const auto fallback = BuildNullArgumentCallStub(stubVaddr + s.size(), targetVaddr);
    s.insert(s.end(), fallback.begin(), fallback.end());
    return s;
}

}
