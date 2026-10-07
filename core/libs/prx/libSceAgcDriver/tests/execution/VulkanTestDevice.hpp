#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_VULKANTESTDEVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_VULKANTESTDEVICE_HPP

#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestMemoryBacking.hpp"
#include <cstddef>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

constexpr int VulkanTestSkipped = 77;

inline std::unique_ptr<AgcDriver::VulkanDevice> OpenVulkanTestDevice() {
    try {
        return std::make_unique<AgcDriver::VulkanDevice>();
    } catch (const std::exception& error) {
        if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
        std::printf("skipped, no usable Vulkan device: %s\n", error.what());
        return nullptr;
    }
}

// Guest memory (flexible, CPU and GPU readable and writable) for what the driver resolves only in
// guest memory: storage images, which it writes back through the guest mapping.
class GuestTestMemory {
public:
    explicit GuestTestMemory(std::size_t bytes) : bytes((bytes + GuestMemoryBacking::kPageBytes - 1) / GuestMemoryBacking::kPageBytes * GuestMemoryBacking::kPageBytes) {
        using namespace GuestMemoryBacking;
        if (GuestVirtualMap_nid_postfix(&address, this->bytes, kPageBytes, Kind::Flexible, kProtCpuRead | kProtCpuWrite | kProtGpuRead | kProtGpuWrite, 0, 0) != Status::Ok) {
            throw std::runtime_error("cannot map guest test memory");
        }
    }
    ~GuestTestMemory() { GuestMemoryBacking::GuestVirtualUnmap_nid_postfix(address, bytes); }
    GuestTestMemory(const GuestTestMemory&) = delete;
    GuestTestMemory& operator=(const GuestTestMemory&) = delete;
    template<typename T>
    T* As() const { return static_cast<T*>(address); }

private:
    void* address = nullptr;
    std::size_t bytes;
};

#endif
