#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"

namespace AgcDriver {

void VulkanDevice::AcquireGpuMemory() {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    PerformanceTimer timing("Vulkan.AcquireGpuMemory");
    const auto context = graphicsContext();
    Graphics::Require(context.drawQueue != nullptr, "GPU memory barrier requires a draw queue");
    context.drawQueue->RecordMemoryBarrier(context);
    timing.Mark("barrier_record");
}

std::uint64_t VulkanDevice::SubmitMarker() {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    const auto context = graphicsContext();
    Graphics::Require(context.drawQueue != nullptr, "GPU marker requires a draw queue");
    return context.drawQueue->SubmitMarker(context);
}

bool VulkanDevice::MarkerReached(std::uint64_t marker) {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    return graphicsContext().drawQueue->MarkerReached(marker);
}

void VulkanDevice::WaitMarker(std::uint64_t marker) {
    std::lock_guard memoryLock(GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix());
    PerformanceTimer timing("Vulkan.WaitMarker");
    graphicsContext().drawQueue->WaitMarker(marker);
}

}
