#include "prx/libkernel/Ampr/include/AmprPackets.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void readFileRoundTrip(std::uint64_t destination, std::uint64_t size, std::uint64_t offset) {
    std::uint32_t packet[6] = {};
    const AmprPackets::ReadFile input{0x1234567, destination, size, offset};
    AmprPackets::EncodeReadFile(packet, input);
    require(AmprPackets::PacketSize(packet[0]) == AmprPackets::ReadFileSize(offset), "ReadFile packet size mismatch");
    require((packet[0] & AmprPackets::kOpcodeMask) == AmprPackets::kOpcodeReadFile, "ReadFile opcode mismatch");
    const auto output = AmprPackets::DecodeReadFile(packet);
    require(output.FileId == input.FileId && output.Destination == input.Destination && output.Size == input.Size && output.FileOffset == input.FileOffset, "ReadFile round trip mismatch");
}

void eventRoundTrip(std::uint64_t queue, std::int32_t id, std::uint64_t data, std::uint32_t flag) {
    std::uint32_t packet[5] = {};
    AmprPackets::EncodeWriteKernelEventQueue(packet, {queue, id, data}, flag);
    require(AmprPackets::PacketSize(packet[0]) == AmprPackets::kWriteKernelEventQueueSize, "event packet size mismatch");
    const auto output = AmprPackets::DecodeWriteKernelEventQueue(packet);
    require(output.Queue == queue && output.Id == id && output.Data == data, "event round trip mismatch");
}

}

int main() {
    try {
        readFileRoundTrip(0x1000, 1, 0);
        readFileRoundTrip(0x7fff12345678, 0x100000000, 0x3ffff);
        readFileRoundTrip(0x10203040, 4096, 0xfffc0123);
        readFileRoundTrip(0x10203040, 4096, 0xff12345678);
        require(AmprPackets::ReadFileSize(0xffffffff) == 20 && AmprPackets::ReadFileSize(0x100000000) == 24, "ReadFile size selection");
        require(!AmprPackets::ReadFileArgumentsValid(0x1000, 0, 0), "zero size accepted");
        require(!AmprPackets::ReadFileArgumentsValid(0x1000, 0x100000001, 0), "oversized read accepted");
        require(!AmprPackets::ReadFileArgumentsValid(0x1000, 1, 1ull << 40), "offset beyond 2^40 accepted");
        require(!AmprPackets::ReadFileArgumentsValid(0xF00000000000, 1, 0), "destination beyond the address limit accepted");
        eventRoundTrip(0x0000123456789abc, 0x74fe, (3ull << 58) | 42, 0);
        eventRoundTrip(7, -1, ~0ull, 1);
        std::cout << "AMPR packet tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
