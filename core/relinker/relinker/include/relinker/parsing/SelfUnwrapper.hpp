#ifndef RELINKER_PARSING_SELFUNWRAPPER_HPP
#define RELINKER_PARSING_SELFUNWRAPPER_HPP

#include <relinker/domain/ISelfUnwrapper.hpp>
#include <relinker/domain/Types.hpp>

namespace Relinker {

class SelfUnwrapper : public ISelfUnwrapper {
public:
    [[nodiscard]] bool IsSelf(const std::vector<std::uint8_t>& fileBytes) const override;
    [[nodiscard]] std::vector<std::uint8_t> Unwrap(const std::vector<std::uint8_t>& fileBytes) const override;

private:
    static constexpr std::uint8_t kPs4SelfMagic[] = {0x4f, 0x15, 0x3d, 0x1d};
    static constexpr std::uint8_t kPs5SelfMagic[] = {0x54, 0x14, 0xf5, 0xee};
    static constexpr std::uint8_t kElfMagic[] = {0x7f, 'E', 'L', 'F'};

    static constexpr std::size_t kSelfHeaderSize = 0x20;
    static constexpr std::size_t kSelfSegmentCountOffset = 0x18;
    static constexpr std::size_t kSelfSegmentEntrySize = 0x20;
    static constexpr std::size_t kElfHeaderSize = 0x40;
    static constexpr std::size_t kProgramHeaderEntrySize = 0x38;

    static constexpr std::uint64_t kSegmentFlagEncrypted = 0x2;
    static constexpr std::uint64_t kSegmentFlagCompressed = 0x8;
    static constexpr std::uint64_t kSegmentFlagBlocked = 0x800;
    static constexpr std::uint32_t kSegmentIdShift = 20;
    static constexpr std::uint64_t kSegmentIdMask = 0xfff;

    static constexpr std::uint32_t kPtSceVersion = 0x6fffff01;

    [[nodiscard]] static bool _hasMagic(const std::vector<std::uint8_t>& bytes, const std::uint8_t (&magic)[4]);
    [[nodiscard]] static std::uint64_t _readU64(const std::vector<std::uint8_t>& bytes, std::size_t offset);
    [[nodiscard]] static std::uint32_t _readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset);
    [[nodiscard]] static std::uint16_t _readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset);
    static void _copy(const std::vector<std::uint8_t>& source, std::size_t sourceOffset, std::vector<std::uint8_t>& target, std::size_t targetOffset, std::size_t size);
};

}

#endif
