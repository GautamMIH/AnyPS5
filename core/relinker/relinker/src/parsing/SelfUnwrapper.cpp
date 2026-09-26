#include <relinker/parsing/SelfUnwrapper.hpp>
#include <algorithm>
#include <cstring>
#include <string>

namespace Relinker {

bool SelfUnwrapper::_hasMagic(const std::vector<std::uint8_t>& bytes, const std::uint8_t (&magic)[4]) {
    return bytes.size() >= sizeof(magic) && std::memcmp(bytes.data(), magic, sizeof(magic)) == 0;
}

std::uint64_t SelfUnwrapper::_readU64(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 8)
        throw RelinkerException("SELF read out of bounds", offset);
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, 8);
    return value;
}

std::uint32_t SelfUnwrapper::_readU32(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw RelinkerException("SELF read out of bounds", offset);
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, 4);
    return value;
}

std::uint16_t SelfUnwrapper::_readU16(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2)
        throw RelinkerException("SELF read out of bounds", offset);
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, 2);
    return value;
}

void SelfUnwrapper::_copy(const std::vector<std::uint8_t>& source, const std::size_t sourceOffset, std::vector<std::uint8_t>& target, const std::size_t targetOffset, const std::size_t size) {
    if (sourceOffset > source.size() || source.size() - sourceOffset < size)
        throw RelinkerException("SELF segment data lies outside the file", sourceOffset);
    if (targetOffset > target.size() || target.size() - targetOffset < size)
        throw RelinkerException("SELF segment does not fit its ELF program header", targetOffset);
    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(sourceOffset), size, target.begin() + static_cast<std::ptrdiff_t>(targetOffset));
}

bool SelfUnwrapper::IsSelf(const std::vector<std::uint8_t>& fileBytes) const {
    return _hasMagic(fileBytes, kPs4SelfMagic) || _hasMagic(fileBytes, kPs5SelfMagic);
}

std::vector<std::uint8_t> SelfUnwrapper::Unwrap(const std::vector<std::uint8_t>& fileBytes) const {
    if (_hasMagic(fileBytes, kElfMagic))
        return fileBytes;
    if (!IsSelf(fileBytes))
        throw RelinkerException("Input is neither an ELF nor a PS4/PS5 SELF file");

    const std::uint16_t segmentCount = _readU16(fileBytes, kSelfSegmentCountOffset);
    const std::size_t elfOffset = kSelfHeaderSize + static_cast<std::size_t>(segmentCount) * kSelfSegmentEntrySize;
    if (elfOffset > fileBytes.size() || fileBytes.size() - elfOffset < kElfHeaderSize)
        throw RelinkerException("SELF file is too small for its embedded ELF header");
    if (std::memcmp(fileBytes.data() + elfOffset, kElfMagic, sizeof(kElfMagic)) != 0)
        throw RelinkerException("SELF file has no embedded ELF header", elfOffset);

    const std::uint64_t phOff = _readU64(fileBytes, elfOffset + 0x20);
    const std::uint16_t phEntSize = _readU16(fileBytes, elfOffset + 0x36);
    const std::uint16_t phNum = _readU16(fileBytes, elfOffset + 0x38);
    if (phEntSize != kProgramHeaderEntrySize)
        throw RelinkerException("SELF embedded ELF has an unsupported program header entry size");

    struct Segment {
        std::uint32_t Type;
        std::uint64_t Offset;
        std::uint64_t FileSize;
    };
    std::vector<Segment> programHeaders;
    std::uint64_t imageSize = phOff + static_cast<std::uint64_t>(phNum) * phEntSize;
    for (std::uint16_t index = 0; index < phNum; ++index) {
        const std::size_t entry = elfOffset + static_cast<std::size_t>(phOff) + static_cast<std::size_t>(index) * phEntSize;
        Segment segment{_readU32(fileBytes, entry), _readU64(fileBytes, entry + 0x08), _readU64(fileBytes, entry + 0x20)};
        imageSize = std::max(imageSize, segment.Offset + segment.FileSize);
        programHeaders.push_back(segment);
    }

    std::vector<std::uint8_t> image(static_cast<std::size_t>(imageSize), 0);
    _copy(fileBytes, elfOffset, image, 0, static_cast<std::size_t>(phOff) + static_cast<std::size_t>(phNum) * phEntSize);
    std::memset(image.data() + 0x28, 0, 8);
    std::memset(image.data() + 0x3a, 0, 6);

    std::vector<bool> filled(phNum, false);
    for (std::uint16_t index = 0; index < segmentCount; ++index) {
        const std::size_t entry = kSelfHeaderSize + static_cast<std::size_t>(index) * kSelfSegmentEntrySize;
        const std::uint64_t flags = _readU64(fileBytes, entry);
        const std::uint64_t dataOffset = _readU64(fileBytes, entry + 0x08);
        const std::uint64_t compressedSize = _readU64(fileBytes, entry + 0x10);
        const std::uint64_t decompressedSize = _readU64(fileBytes, entry + 0x18);
        if ((flags & kSegmentFlagBlocked) == 0)
            continue;

        const auto programIndex = static_cast<std::size_t>((flags >> kSegmentIdShift) & kSegmentIdMask);
        if (programIndex >= programHeaders.size())
            throw RelinkerException("SELF segment references a missing program header", entry);
        if ((flags & kSegmentFlagEncrypted) != 0)
            throw RelinkerException("SELF segment " + std::to_string(programIndex) + " is encrypted; the dump must be decrypted first", entry);
        if ((flags & kSegmentFlagCompressed) != 0 || compressedSize != decompressedSize)
            throw RelinkerException("SELF segment " + std::to_string(programIndex) + " is compressed; compressed SELF segments are not supported", entry);
        if (decompressedSize != programHeaders[programIndex].FileSize)
            throw RelinkerException("SELF segment size differs from its program header", entry);

        _copy(fileBytes, static_cast<std::size_t>(dataOffset), image, static_cast<std::size_t>(programHeaders[programIndex].Offset), static_cast<std::size_t>(decompressedSize));
        filled[programIndex] = true;
    }

    for (std::size_t index = 0; index < programHeaders.size(); ++index) {
        const Segment& segment = programHeaders[index];
        if (filled[index] || segment.Type != kPtSceVersion || segment.FileSize == 0)
            continue;
        if (segment.FileSize > fileBytes.size())
            throw RelinkerException("SELF version segment is larger than the file");
        _copy(fileBytes, fileBytes.size() - static_cast<std::size_t>(segment.FileSize), image, static_cast<std::size_t>(segment.Offset), static_cast<std::size_t>(segment.FileSize));
    }

    return image;
}

}
