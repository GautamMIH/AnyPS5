#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_VULKANDEVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_VULKANDEVICE_HPP

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "Recompiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include <memory>

namespace AgcDriver {

class FrameTiming;

class VulkanDevice {
public:
    explicit VulkanDevice(const PresentationWindow* window = nullptr);
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
    ShaderRecompiler::SpirvTarget Target() const;
    void WaitIdle();
    void WaitDraws();
    // The samples that passed the depth and stencil tests in every draw recorded since the first
    // call (which turns counting on), after waiting for the queued draws (PIXEL_PIPE_STAT_DUMP).
    std::uint64_t CountSamples();
    void AcquireGpuMemory();
    // GPU completion markers (see Graphics::DrawQueue::SubmitMarker).
    std::uint64_t SubmitMarker();
    bool MarkerReached(std::uint64_t marker);
    void WaitMarker(std::uint64_t marker);
    void ResolveMemory(std::uint64_t address, std::size_t bytes, bool writable);
    // Records a DMA_DATA copy or fill into the draw queue, in order after earlier GPU work, when both
    // ranges are imported guest memory (no CPU wait); false when the CPU must perform it.
    // serial (when given) receives the write's draw-queue serial (DrawQueue::LastWriter).
    bool RecordDmaData(const Pm4::DmaCopy& dma, std::uint64_t* serial = nullptr);
    std::uint64_t LastGpuWriter(std::uint64_t address, std::size_t bytes);
    void ResolveFastClears(const Graphics::State& graphics);
    void* Window() const;
    void Resize(std::uint32_t width, std::uint32_t height);
    bool Presentable() const;
    // VK_EXT_primitive_topology_list_restart: primitive restart also applies to list topologies.
    bool PrimitiveListRestart() const;
    void PresentClear(std::uint32_t width, std::uint32_t height, bool opaque);
    void PresentPixels(std::uint32_t width, std::uint32_t height, std::span<const std::byte> pixels);
    void PresentDisplayBuffer(const DisplayBuffer& buffer);
    void DumpFrame(const DisplayBuffer& buffer);
    // ANYPS5_GPU_TIMING: adds the GPU time of batches completed since the last report to the frame
    // (GPU.<label>, bytes = batches; GPU.busy = their sum).
    void ReportGpuTime(FrameTiming& frame);
    void Dispatch(const ShaderRecompiler::RecompileResult& shader, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::span<const Graphics::GuestMemorySnapshot> snapshots = {});
    void Draw(const Graphics::State& graphics, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders, std::span<const Graphics::GuestMemorySnapshot> snapshots = {});
    void EnqueueDraw(const Graphics::State& graphics, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> shaders, std::span<const Graphics::GuestMemorySnapshot> snapshots = {});

private:
    Graphics::Context graphicsContext() const;
    void present(std::uint32_t width, std::uint32_t height, bool opaque, std::span<const std::byte> pixels, const DisplayBuffer* display = nullptr);
    struct State;
    std::unique_ptr<State> state;
};

}

#endif
