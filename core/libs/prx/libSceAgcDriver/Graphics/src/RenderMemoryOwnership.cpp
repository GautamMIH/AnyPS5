#include "prx/libSceAgcDriver/Graphics/include/RenderCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawQueue.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include <limits>

namespace AgcDriver::Graphics {

ResidentColor::ResidentColor(const Context& context, const ColorTarget& color) : context(context), color(color), transfer(context) {
    if (color.Layered()) layered = std::make_unique<LayeredColorTransfer>(context, color);
    memoryWatch = std::make_unique<GuestMemoryTracking::Watch>(color.address, color.bytes, this, [](void* owner, GuestMemoryTracking::Access access) {
        static_cast<ResidentColor*>(owner)->resolveCpuAccess(access);
    });
}

ResidentColor::~ResidentColor() = default;

void ResidentColor::Invalidate() {
    Require(!dirty, "cannot discard GPU-owned render target contents");
    if (TraceRenderTargets() && valid) std::fprintf(stderr, "[rt] invalidate color@0x%llx+0x%llx site=%s\n", static_cast<unsigned long long>(color.address), static_cast<unsigned long long>(color.bytes), GuestMemory::AccessSite::Current());
    if (memoryWatch) memoryWatch->Protect(GuestMemoryTracking::Protection::ReadWrite);
    valid = false;
}

void ResidentColor::ReleaseMemory() {
    Require(!dirty, "cannot release GPU-owned render target memory");
    Invalidate();
    memoryWatch.reset();
}

bool ResidentColor::Overlaps(const ColorTarget& other) const {
    Require(color.bytes != 0 && other.bytes != 0, "empty render target range");
    Require(color.bytes <= std::numeric_limits<std::uint64_t>::max() - color.address && other.bytes <= std::numeric_limits<std::uint64_t>::max() - other.address, "render target range overflow");
    return color.address < other.address + other.bytes && other.address < color.address + color.bytes;
}

void ResidentColor::resolveCpuAccess(GuestMemoryTracking::Access access) {
    PerformanceTimer timing("Graphics.RenderMemory.CpuAccess");
    if (TraceRenderTargets()) std::fprintf(stderr, "[rt] cpu %s color@0x%llx dirty=%d site=%s\n", access == GuestMemoryTracking::Access::Read ? "read" : access == GuestMemoryTracking::Access::Invalidate ? "invalidate" : "write", static_cast<unsigned long long>(color.address), dirty ? 1 : 0, GuestMemory::AccessSite::Current());
    Require(memoryWatch != nullptr && context.drawQueue != nullptr, "render target memory resolver is unavailable");
    if (dirty || gpuWritePending || access == GuestMemoryTracking::Access::Invalidate) {
        context.drawQueue->WaitGpu();
        timing.Mark("draw_wait");
    }
    if (gpuWritePending && !dirty) {
        // The queued GPU write-back has reached guest memory.
        gpuWritePending = false;
        if (access == GuestMemoryTracking::Access::Read) memoryWatch->Protect(GuestMemoryTracking::Protection::Read);
    }
    if (dirty) {
        CommandBatch batch(context);
        Download(batch.Handle());
        batch.SubmitAndWait();
        timing.Mark("download_wait");
        Commit();
        timing.Mark("guest_writeback");
    }
    if (access != GuestMemoryTracking::Access::Read) Invalidate();
    if (access == GuestMemoryTracking::Access::Invalidate) context.drawQueue->Wait();
}

RenderCache::~RenderCache() {
    for (const auto& [address, entry] : entries) entry->ReleaseMemory();
    for (const auto& entry : depthEntries) entry->ReleaseMemory();
}

}
