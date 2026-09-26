#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastClear.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libSceAgcDriver/Execution/include/MemoryAccessScope.hpp"
#include "prx/libc/include/GuestMemoryTracking.hpp"
#include <cstdio>
#include <bit>
#include <algorithm>
#include <cstdlib>
#include <array>
#include <condition_variable>
#include <cstring>
#include <chrono>
#include <deque>
#include <set>
#include <exception>
#include <limits>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace AgcDriver {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(std::string("AGC driver: ") + reason);
    }
}

struct ShaderSnapshot {
    std::uint64_t codeAddress;
    std::uint64_t headerAddress;
    std::uint8_t type;
    std::vector<std::uint32_t> code;
    std::vector<std::byte> header;
};

struct Submission {
    std::uint64_t serial;
    std::uint32_t queue;
    std::vector<std::uint32_t> commands;
    std::map<std::uint64_t, std::shared_ptr<const ShaderSnapshot>> shaders;
    std::map<std::size_t, std::shared_ptr<IFlipRequest>> flips;
    std::map<std::size_t, std::shared_ptr<IRenderingWait>> renderingWaits;
    bool suspend = false;
    std::size_t cursor = 0;
    bool started = false;
    FrameTiming::Clock::time_point received;
    FrameTiming::Clock::time_point copied;
    FrameTiming::Clock::time_point validated;
    FrameTiming::Clock::time_point enqueued;
    FrameTiming::Clock::time_point dequeued;
};

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset) {
    const auto it = registers.find(offset);
    if (it == registers.end()) {
        char message[96];
        std::snprintf(message, sizeof(message), "AGC driver: required shader register 0x%x has not been written", offset);
        throw std::runtime_error(message);
    }
    return it->second;
}

class Driver {
public:
    static Driver& Get() {
        static Driver driver;
        return driver;
    }

    ~Driver() {
        stop();
    }

    void Shutdown() {
        stop();
        CheckFailure();
    }

private:
    void stop() {
        require(std::this_thread::get_id() != worker.get_id(), "worker cannot stop itself");
        std::lock_guard shutdownLock(shutdownMutex);
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();
    }

public:

    void Submit(const Packet* packet, std::uint32_t queue) {
        const auto received = FrameTiming::Clock::now();
        CheckFailure();
        require(queue == 0 || (queue >= 0x20 && queue < 0x58), "unsupported compute queue");
        GuestMemory::CheckRange(packet, sizeof(Packet), alignof(Packet));
        const auto descriptor = *packet;
        require(descriptor.flags == 0, "nonzero submission flags are not implemented");
        Submission submission{};
        submission.queue = queue;
        submission.received = received;
        if (descriptor.dw_num != 0) {
            require(descriptor.dw_num <= std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t), "command size overflow");
            GuestMemory::CheckRange(descriptor.addr, static_cast<std::size_t>(descriptor.dw_num) * sizeof(std::uint32_t), alignof(std::uint32_t));
            submission.commands.assign(descriptor.addr, descriptor.addr + descriptor.dw_num);
        }
        submission.copied = FrameTiming::Clock::now();
        validate(submission.commands, queue);
        submission.validated = FrameTiming::Clock::now();
        {
            std::lock_guard lock(mutex);
            rethrowFailure();
            require(!stopping, "submission during shutdown");
            require(accepted != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
            for (std::size_t cursor = 0; cursor < submission.commands.size();) {
                const auto* words = submission.commands.data() + cursor;
                if (words[0] == RenderingWaitPacketHeader) {
                    const auto output = outputs.find(words[1]);
                    require(output != outputs.end(), "rendering wait references an unregistered video output");
                    auto wait = output->second->CaptureRenderingWait(words[2]);
                    require(wait != nullptr, "video output returned a null rendering wait");
                    submission.renderingWaits.emplace(cursor, std::move(wait));
                }
                if (words[0] == FlipPacketHeader) {
                    const auto output = outputs.find(words[1]);
                    require(output != outputs.end(), "flip references an unregistered video output");
                    const FlipInfo info{words[1], std::bit_cast<std::int32_t>(words[2]), words[3], std::bit_cast<std::int64_t>(static_cast<std::uint64_t>(words[4]) | (static_cast<std::uint64_t>(words[5]) << 32u))};
                    auto request = output->second->Reserve(info);
                    require(request != nullptr, "video output returned a null flip reservation");
                    submission.flips.emplace(cursor, std::move(request));
                }
                cursor += static_cast<std::size_t>((words[0] >> 16u) & 0x3fffu) + 2;
            }
            submission.shaders = shaders;
            submission.serial = accepted + 1;
            submission.enqueued = FrameTiming::Clock::now();
            pending.push_back(std::move(submission));
            ++accepted;
        }
        changed.notify_all();
    }

    void WaitIdle() {
        require(std::this_thread::get_id() != worker.get_id(), "worker cannot wait for itself");
        std::unique_lock lock(mutex);
        const auto target = accepted;
        changed.wait(lock, [&] { return failure != nullptr || completed >= target; });
        rethrowFailure();
    }

    void SuspendPoint() {
        const auto received = FrameTiming::Clock::now();
        require(std::this_thread::get_id() != worker.get_id(), "worker cannot suspend itself");
        std::unique_lock lock(mutex);
        rethrowFailure();
        require(!stopping, "suspend during shutdown");
        require(accepted != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
        Submission boundary{};
        boundary.serial = accepted + 1;
        boundary.suspend = true;
        boundary.received = received;
        boundary.copied = received;
        boundary.validated = received;
        boundary.enqueued = FrameTiming::Clock::now();
        pending.push_back(std::move(boundary));
        const auto target = ++accepted;
        changed.notify_all();
        changed.wait(lock, [&] { return failure != nullptr || completed >= target; });
        rethrowFailure();
    }

    void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
        require(output != nullptr, "null video output");
        std::lock_guard lock(mutex);
        rethrowFailure();
        require(!stopping, "video output registration during shutdown");
        require(outputs.emplace(handle, output).second, "video output already registered");
    }

    void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
        std::lock_guard lock(mutex);
        const auto it = outputs.find(handle);
        require(it != outputs.end() && it->second == output, "video output registration mismatch");
        outputs.erase(it);
    }

    void CheckFailure() {
        std::lock_guard lock(mutex);
        rethrowFailure();
    }

    void ReportFailure(std::exception_ptr error) {
        require(error != nullptr, "null asynchronous failure");
        {
            std::lock_guard lock(mutex);
            if (!failure) failure = error;
            for (const auto& [handle, output] : outputs) output->Fail(failure);
            for (const auto& item : pending) {
                for (const auto& [offset, flip] : item.flips) flip->Fail(failure);
            }
            pending.clear();
        }
        changed.notify_all();
    }

    void Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque, void (*gpuReady)(void*), void* context) {
        PerformanceContext timingContext(window.timing.get());
        PerformanceTimer timing("Driver.Present");
        CheckFailure();
        require(gpuReady != nullptr && context != nullptr, "missing GPU completion callback");
        require(window.getDrawableSize != nullptr, "missing window drawable size query");
        std::shared_ptr<VulkanDevice> presenting;
        timing.Mark("validate");
        try {
            {
                std::lock_guard lock(gpuMutex);
                timing.Mark("gpu_mutex_wait");
                if (device == nullptr || device->Window() == nullptr) {
                    if (device) device->WaitIdle();
                    device = std::make_shared<VulkanDevice>(&window);
                }
                require(device->Window() == window.context, "presentation window does not match device surface");
                presenting = device;
                timing.Mark("device_setup");
                std::uint32_t drawableWidth = 0;
                std::uint32_t drawableHeight = 0;
                window.getDrawableSize(window.context, &drawableWidth, &drawableHeight);
                presenting->Resize(drawableWidth, drawableHeight);
                timing.Mark("resize");
                if (presenting->Presentable()) {
                    if (buffer != nullptr) {
                        require(buffer->width == window.width && buffer->height == window.height, "display buffer extent differs from output");
                        presenting->WaitDraws();
                        timing.Mark("draw_wait");
                        presenting->PresentDisplayBuffer(*buffer);
                        timing.Mark("present_display_buffer");
                    } else {
                        presenting->PresentClear(window.width, window.height, opaque);
                        timing.Mark("present_clear");
                    }
                }
            }
            gpuReady(context);
            timing.Mark("release_and_callback");
            CheckFailure();
        } catch (...) {
            ReportFailure(std::current_exception());
            throw;
        }
    }

    void ReleaseWindow(void* window) {
        std::lock_guard lock(gpuMutex);
        if (device && device->Window() == window) device.reset();
    }

    void RegisterShader(const Shader* shader) {
        CheckFailure();
        // Shader binaries embedded in game data are only guaranteed 4-byte alignment, so the fixed
        // fields are read through a copy rather than through the guest pointer.
        GuestMemory::CheckRange(shader, sizeof(Shader), alignof(std::uint32_t));
        Shader fixed;
        std::memcpy(&fixed, shader, sizeof(Shader));
        require(fixed.file_header == 0x34333231u && fixed.version == 0x18u, "invalid shader header");
        require(fixed.header_size >= sizeof(Shader), "shader header is smaller than its fixed fields");
        require(fixed.shader_size != 0 && (fixed.shader_size & 3u) == 0, "invalid shader size");
        GuestMemory::CheckRange(shader, fixed.header_size, alignof(std::uint32_t));
        const auto* code = const_cast<const void*>(fixed.code);
        GuestMemory::CheckRange(code, fixed.shader_size, 256);
        ShaderSnapshot snapshot{reinterpret_cast<std::uintptr_t>(code), reinterpret_cast<std::uintptr_t>(shader), fixed.type, {}, {}};
        snapshot.code.resize(fixed.shader_size / sizeof(std::uint32_t));
        std::memcpy(snapshot.code.data(), code, fixed.shader_size);
        snapshot.header.resize(fixed.header_size);
        std::memcpy(snapshot.header.data(), shader, fixed.header_size);
        std::lock_guard lock(mutex);
        rethrowFailure();
        const auto address = snapshot.codeAddress;
        shaders.insert_or_assign(address, std::make_shared<const ShaderSnapshot>(std::move(snapshot)));
    }

private:
    std::mutex mutex;
    std::mutex shutdownMutex;
    std::condition_variable changed;
    std::deque<Submission> pending;
    std::map<std::uint64_t, std::shared_ptr<const ShaderSnapshot>> shaders;
    std::map<std::uint32_t, QueueState> queues;
    std::map<std::uint32_t, std::shared_ptr<IVideoOutput>> outputs;
    std::recursive_mutex& gpuMutex = GuestMemoryTracking::GuestMemoryTrackingMutex_nid_postfix();
    std::shared_ptr<VulkanDevice> device;
    std::uint64_t accepted = 0;
    std::uint64_t completed = 0;
    std::set<std::uint64_t> finishedOutOfOrder;
    std::deque<Submission> active;
    std::string blockedWait;
    std::exception_ptr failure;
    bool stopping = false;
    bool resetGraphics = false;
    std::shared_ptr<FrameTiming> frameTiming;
    std::uint64_t frameSerial = 0;
    std::thread worker;

    Driver() : worker([this] { run(); }) {
        try {
            LibcRegisterShutdown_nid_postfix([] { Driver::Get().Shutdown(); });
        } catch (...) {
            stop();
            throw;
        }
    }

    void rethrowFailure() const {
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

    // INDIRECT_BUFFER: the nested command buffer is read when execution reaches it and replaces the
    // packet (call) or the rest of the submission (chain, bit 20).
    static void spliceIndirectBuffer(Submission& submission, std::size_t cursor) {
        constexpr std::size_t packetDwords = 4;
        constexpr std::size_t maximumDwords = 64u * 1024u * 1024u;
        const auto* packet = submission.commands.data() + cursor;
        const auto address = (static_cast<std::uint64_t>(packet[2]) << 32u) | (packet[1] & ~3u);
        const auto dwords = static_cast<std::size_t>(packet[3] & 0xfffffu);
        const bool chain = (packet[3] & (1u << 20u)) != 0;
        std::vector<std::uint32_t> nested;
        if (dwords != 0) {
            require(address != 0, "nested command buffer has a null address");
            const auto* source = reinterpret_cast<const std::uint32_t*>(address);
            GuestMemory::CheckRange(source, dwords * sizeof(std::uint32_t), alignof(std::uint32_t));
            nested.assign(source, source + dwords);
            validate(nested, submission.queue);
            for (std::size_t offset = 0; offset < nested.size(); offset += ((nested[offset] >> 16u) & 0x3fffu) + 2u) {
                require(nested[offset] != FlipPacketHeader && nested[offset] != RenderingWaitPacketHeader, "flips and rendering waits inside nested command buffers are not implemented");
            }
        }
        require(submission.commands.size() - packetDwords + nested.size() <= maximumDwords, "nested command buffers exceed the submission limit");
        const auto after = cursor + packetDwords;
        const auto shift = [&](auto& byOffset) {
            std::remove_reference_t<decltype(byOffset)> moved;
            for (auto& [offset, value] : byOffset) {
                if (offset < after) moved.emplace(offset, std::move(value));
                else {
                    require(!chain, "packets after a chained command buffer are not executed");
                    moved.emplace(offset - packetDwords + nested.size(), std::move(value));
                }
            }
            byOffset = std::move(moved);
        };
        shift(submission.flips);
        shift(submission.renderingWaits);
        auto& commands = submission.commands;
        const auto tail = chain ? commands.end() : commands.begin() + static_cast<std::ptrdiff_t>(after);
        commands.erase(commands.begin() + static_cast<std::ptrdiff_t>(cursor), tail);
        commands.insert(commands.begin() + static_cast<std::ptrdiff_t>(cursor), nested.begin(), nested.end());
    }

    static void validate(std::span<const std::uint32_t> commands, std::uint32_t queue) {
        for (std::size_t cursor = 0; cursor < commands.size();) {
            const auto header = commands[cursor];
            require((header & 0xc0000000u) == 0xc0000000u, "unsupported PM4 packet type");
            const auto count = static_cast<std::size_t>((header >> 16u) & 0x3fffu) + 2;
            require(count <= commands.size() - cursor, "truncated PM4 packet");
            try {
                Pm4::Validate(commands.subspan(cursor, count), queue);
            } catch (const std::exception& error) {
                throw std::runtime_error("AGC driver: " + Pm4::Name(header) + " at DWORD " + std::to_string(cursor) + ": " + error.what());
            }
            cursor += count;
        }
    }

    void dispatch(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission) {
        PerformanceTimer timing("Driver.Dispatch");
        std::lock_guard gpuLock(gpuMutex);
        timing.Mark("gpu_mutex_wait");
        const auto address = (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20c)) << 8u) | (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20d) & 0xffu) << 40u);
        auto it = submission.shaders.upper_bound(address);
        require(it != submission.shaders.begin(), "compute program does not belong to a registered shader");
        --it;
        const auto& snapshot = *it->second;
        require(address - snapshot.codeAddress < snapshot.code.size() * sizeof(std::uint32_t), "compute program is outside registered shader code");
        require(snapshot.type == 0, "compute program refers to a non-compute shader");
        const auto userCount = (readRegister(queue.shader, 0x213) >> 1u) & 0x1fu;
        std::vector<std::uint32_t> userData;
        for (std::uint32_t i = 0; i < userCount; ++i) {
            userData.push_back(readRegister(queue.shader, 0x240 + i));
        }
        const auto compute = Graphics::DecodeComputeStageInfo(queue.shader);
        const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}, {snapshot.headerAddress, snapshot.header}}};
        if (device == nullptr) {
            device = std::make_shared<VulkanDevice>();
        }
        const GuestMemory::MemoryAccessScope memoryScope(device.get(), [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
            static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
        });
        const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
        ShaderRecompiler::RecompileRequest request{
            {ShaderRecompiler::ShaderStage::Compute, address, std::span(snapshot.code).subspan(codeOffset), snapshot.headerAddress, snapshot.header},
            {(packet[4] & 0x8000u) != 0 ? 32u : 64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
            device->Target(),
            {0, 0, 0, 128}
        };
        ShaderMemory shaderMemory(memory);
        timing.Mark("prepare");
        shaderMemory.Capture(request);
        timing.Mark("shader_memory_capture");
        const auto captured = shaderMemory.Regions();
        request.context.memory = captured;
        timing.Mark("request_memory");
        const auto compiled = [&] {
            try {
                return ShaderRecompiler::Recompile(request);
            } catch (const std::exception& error) {
                char context[160]{};
                std::snprintf(context, sizeof(context), "compute shader 0x%llx (header 0x%llx, PGM_RSRC2 0x%08x, %u user SGPRs): ",
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(snapshot.headerAddress), readRegister(queue.shader, 0x213), userCount);
                throw std::runtime_error(std::string(context) + error.what());
            }
        }();
        timing.Mark(compiled.cacheHit ? "shader_cache_hit" : "shader_compile");
        std::vector<Graphics::GuestMemorySnapshot> snapshots;
        for (const auto& region : captured) snapshots.push_back({region.guestAddress, region.bytes});
        timing.Mark("snapshots");
        device->Dispatch(compiled, packet[1], packet[2], packet[3], snapshots);
        timing.Mark("dispatch_and_resource_release");
        // A shader that stores to a buffer starting at a registered CMASK without XOR address
        // math is taken to clear it (shadPS4). The dispatch itself still runs.
        if (!compiled.usesBitwiseXor) {
            for (const auto& binding : compiled.bindings) {
                if (binding.kind != ShaderRecompiler::DescriptorKind::StorageBuffer || binding.readOnly) continue;
                for (std::uint32_t element = 0; element < binding.count && (element + 1u) * 4u <= binding.guestDescriptor.size(); ++element) {
                    if (element < binding.bufferWritten.size() && !binding.bufferWritten[element]) continue;
                    const auto* words = binding.guestDescriptor.data() + element * 4u;
                    Graphics::NoteMetadataClear(words[0] | (static_cast<std::uint64_t>(words[1] & 0xffffu) << 32u));
                }
            }
        }
    }

    void draw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission) {
        PerformanceTimer timing("Driver.Draw");
        auto drawParameters = Pm4::ResolveDraw(packet, queue);
        if (!drawParameters.indexed && (drawParameters.indexCount == 0 || drawParameters.instanceCount == 0)) return;
        const auto graphics = Graphics::DecodeState(queue);
        if (graphics.eliminateFastClear) {
            std::lock_guard gpuLock(gpuMutex);
            device->ResolveFastClears(graphics);
            return;
        }
        struct Program {
            ShaderRecompiler::ShaderBinary binary;
            std::uint32_t userDataBase;
            std::uint32_t firstUserSgpr = 8;
            std::vector<std::uint32_t> userData;
            std::array<ShaderRecompiler::MemoryRegion, 2> memory;
        };
        const auto programAddress = [&](std::uint32_t base) {
            const auto high = readRegister(queue.shader, base + 1);
            require((high & ~0xffu) == 0, "reserved graphics program address bits are set");
            return (static_cast<std::uint64_t>(readRegister(queue.shader, base)) << 8u) | (static_cast<std::uint64_t>(high) << 40u);
        };
        const auto prepare = [&](std::uint64_t address, std::uint8_t type, ShaderRecompiler::ShaderStage stage, std::uint32_t rsrc2, std::uint32_t userDataBase) {
            auto it = submission.shaders.upper_bound(address);
            require(it != submission.shaders.begin(), "graphics program does not belong to a registered shader");
            --it;
            const auto& snapshot = *it->second;
            require(address - snapshot.codeAddress < snapshot.code.size() * sizeof(std::uint32_t), "graphics program is outside registered shader code");
            require(snapshot.type == type, "graphics program refers to an incompatible shader binary type");
            const auto resources = readRegister(queue.shader, rsrc2);
            const auto userCount = ((resources >> 1u) & 0x1fu) | (((resources >> 27u) & 1u) << 5u);
            require(userCount <= 32, "graphics user SGPR count exceeds the register bank");
            const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
            Program result{
                {stage, address, std::span(snapshot.code).subspan(codeOffset), snapshot.headerAddress, snapshot.header},
                userDataBase,
                8,
                {},
                {{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}, {snapshot.headerAddress, snapshot.header}}}
            };
            for (std::uint32_t i = 0; i < userCount; ++i) result.userData.push_back(readRegister(queue.shader, userDataBase + i));
            return result;
        };
        using Stage = ShaderRecompiler::ShaderStage;
        using Role = ShaderRecompiler::ProgramRole;
        std::vector<Program> programs;
        std::vector<Role> roles;
        programs.reserve(5);
        roles.reserve(5);
        const auto append = [&](std::uint32_t base, std::uint8_t type, Stage stage, std::uint32_t resources, std::uint32_t users, Role role) {
            programs.push_back(prepare(programAddress(base), type, stage, resources, users));
            roles.push_back(role);
        };
        const auto initializeMerged = [&](Program& program, std::uint32_t pointerBase, bool pointerRequired) {
            program.firstUserSgpr = 0;
            program.userData.insert(program.userData.begin(), 8, 0);
            if (pointerRequired) {
                const auto low = readRegister(queue.shader, pointerBase);
                const auto high = readRegister(queue.shader, pointerBase + 1);
                const auto address = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32u);
                require(address != 0, "merged shader user-data address is null");
                GuestMemory::CheckRange(reinterpret_cast<const void*>(address), 8, 4);
                program.userData[0] = low;
                program.userData[1] = high;
            }
        };
        if (graphics.stages.path == Graphics::ShaderPath::Tessellation) {
            append(0x148, 5, Stage::Local, 0x10b, 0x10c, Role::Local);
            append(0x108, 7, Stage::TessellationControl, 0x10b, 0x10c, Role::Hull);
            initializeMerged(programs.back(), 0x102, true);
            append(0x0c8, 2, Stage::TessellationEvaluation, 0x08b, 0x08c, Role::Domain);
        } else if (graphics.stages.path == Graphics::ShaderPath::Geometry) {
            const auto frontAddress = programAddress(0xc8);
            auto snapshot = submission.shaders.upper_bound(frontAddress);
            require(snapshot != submission.shaders.begin(), "geometry front program is not registered");
            --snapshot;
            const auto type = snapshot->second->type;
            require(type == 2 || type == 4, "invalid geometry front binary type");
            append(0xc8, type, Stage::Mesh, 0x8b, 0x8c, Role::Main);
            initializeMerged(programs.back(), 0x82, type == 4);
            if (type == 4) append(0x88, 6, Stage::Mesh, 0x8b, 0x8c, Role::GeometryBack);
        } else {
            append(0xc8, 2, Stage::Vertex, 0x8b, 0x8c, Role::Main);
        }
        std::optional<ShaderRecompiler::ShaderPixelStageInfo> pixel;
        if (graphics.hasFragmentShader) {
            append(0x008, 1, Stage::Fragment, 0x00b, 0x00c, Role::Fragment);
            programs.back().firstUserSgpr = 0;
            std::array<std::uint8_t, 8> mappings{};
            for (std::uint32_t slot = 0; slot < mappings.size(); ++slot) mappings[slot] = (graphics.colorTargetMask & (1u << slot)) != 0 ? graphics.colors[slot].componentMapping : 0xe4u;
            pixel = Graphics::DecodePixelStageInfo(queue.context, mappings);
        }
        std::vector<ShaderRecompiler::MemoryRegion> memory;
        std::vector<ShaderRecompiler::LinkedProgram> linked;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& program = programs[i];
            memory.insert(memory.end(), program.memory.begin(), program.memory.end());
            linked.push_back({roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
        }
        timing.Mark("prepare");
        std::lock_guard gpuLock(gpuMutex);
        timing.Mark("gpu_mutex_wait");
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        timing.Mark("device_setup");
        const GuestMemory::MemoryAccessScope memoryScope(device.get(), [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
            static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
        });
        ShaderMemory shaderMemory(memory);
        std::vector<ShaderRecompiler::RecompileResult> results;
        std::vector<Graphics::CompiledShader> stages;
        results.reserve(programs.size() + (graphics.rectList ? 2u : 0u));
        stages.reserve(programs.size());
        std::uint32_t pushCursorBytes = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (roles[i] == Role::GeometryBack) continue;
            const auto& program = programs[i];
            const auto waveSize = program.binary.stage == Stage::Fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
            ShaderRecompiler::RecompileRequest request{
                program.binary,
                {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? pixel : std::nullopt, program.binary.stage == Stage::Fragment ? std::nullopt : std::optional(Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData)), memory},
                device->Target(),
                {0, 0, pushCursorBytes, Graphics::PipelinePushConstantBytes - pushCursorBytes},
                ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation, {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}}
            };
            PerformanceTimer shaderTiming("Driver.GraphicsShader");
            shaderMemory.Capture(request);
            shaderTiming.Mark("memory_capture");
            memory = shaderMemory.Regions();
            request.context.memory = memory;
            shaderTiming.Mark("request_memory");
            results.push_back(ShaderRecompiler::Recompile(request));
            shaderTiming.Mark(results.back().cacheHit ? "cache_hit" : "compile");
            const auto& result = results.back();
            if (!drawParameters.indexed && i == 0) {
                const auto offsetValue = [&](std::int32_t sgpr) {
                    require(sgpr >= 0 && static_cast<std::uint32_t>(sgpr) >= program.firstUserSgpr, "invalid draw offset SGPR");
                    const auto index = static_cast<std::uint32_t>(sgpr) - program.firstUserSgpr;
                    require(index < program.userData.size(), "draw offset SGPR exceeds user data");
                    return program.userData[index];
                };
                if (drawParameters.firstVertex == 0 && result.vertexOffsetSgpr >= 0) drawParameters.firstVertex = offsetValue(result.vertexOffsetSgpr);
                if (result.instanceOffsetSgpr >= 0) drawParameters.firstInstance = offsetValue(result.instanceOffsetSgpr);
            }
            require(result.pushConstants.size() <= Graphics::PipelinePushConstantBytes - pushCursorBytes, "stage push constants exceed the pipeline push constant block");
            stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushCursorBytes});
            pushCursorBytes += static_cast<std::uint32_t>(result.pushConstants.size());
        }
        timing.Mark("shaders");
        if (graphics.rectList) {
            require(stages.size() == (graphics.hasFragmentShader ? 2u : 1u), "rect-list requires a vertex program and at most one fragment program");
            // Without a pixel shader no parameters are interpolated.
            const ShaderRecompiler::RecompileResult noFragment{};
            auto rectangle = ShaderRecompiler::BuildRectListShaders(results[0], graphics.hasFragmentShader ? results[1] : noFragment, device->Target());
            results.push_back(std::move(rectangle.control));
            results.push_back(std::move(rectangle.evaluation));
            const auto control = results.size() - 2;
            stages.insert(stages.begin() + 1, {{Stage::TessellationControl, &results[control], 0}, {Stage::TessellationEvaluation, &results[control + 1], 0}});
        }
        std::vector<Graphics::GuestMemorySnapshot> snapshots;
        for (const auto& region : memory) snapshots.push_back({region.guestAddress, region.bytes});
        timing.Mark("post_compile_prepare");
        // Debug aid: ANYPS5_SYNC_DRAWS=1 completes each draw before the next packet, so a GPU
        // fault surfaces at the draw that caused it.
        static const bool syncDraws = std::getenv("ANYPS5_SYNC_DRAWS") != nullptr;
        if (syncDraws) {
            device->Draw(graphics, drawParameters, stages, snapshots);
            std::fprintf(stderr, "[pm4] draw completed\n");
        } else {
            device->EnqueueDraw(graphics, drawParameters, stages, snapshots);
        }
        timing.Mark("draw_and_resource_release");
    }

    void includeSubmission(const Submission& submission, bool firstSegment) {
        if (frameTiming == nullptr) {
            require(frameSerial != std::numeric_limits<std::uint64_t>::max(), "frame serial overflow");
            frameTiming = std::make_shared<FrameTiming>(++frameSerial);
        }
        const auto dequeued = firstSegment ? submission.dequeued : FrameTiming::Clock::now();
        frameTiming->IncludeSubmission(submission.serial, submission.received, submission.enqueued, dequeued, firstSegment);
        if (firstSegment) {
            frameTiming->Add(frameTiming->Get("Submission", "copy"), submission.copied - submission.received, submission.commands.size() * sizeof(std::uint32_t));
            frameTiming->Add(frameTiming->Get("Submission", "validate"), submission.validated - submission.copied);
            frameTiming->Add(frameTiming->Get("Submission", "reserve_enqueue"), submission.enqueued - submission.validated);
        }
    }

    // Runs the submission from its cursor. Returns false when a WAIT_REG_MEM is not yet satisfied; the
    // cursor then points at that packet so the worker can resume it after other queues make progress.
    bool execute(Submission& submission) {
        const auto resumed = submission.started;
        if (!resumed) {
            submission.started = true;
            includeSubmission(submission, true);
        }
        if (submission.suspend) {
            PerformanceContext timingContext(frameTiming.get());
            PerformanceTimer timing("Driver.Suspend");
            std::lock_guard gpuLock(gpuMutex);
            timing.Mark("gpu_mutex_wait");
            if (device != nullptr) device->WaitIdle();
            timing.Mark("device_idle_wait");
            resetGraphics = true;
            return true;
        }
        if (!resumed && submission.queue == 0 && resetGraphics) {
            queues.erase(0);
            resetGraphics = false;
        }
        auto& queue = queues[submission.queue];
        for (auto& cursor = submission.cursor; cursor < submission.commands.size();) {
            if (frameTiming == nullptr) includeSubmission(submission, false);
            const auto header = submission.commands[cursor];
            const auto count = static_cast<std::size_t>((header >> 16u) & 0x3fffu) + 2;
            const auto packet = std::span(submission.commands).subspan(cursor, count);
            const auto opcode = (header >> 8u) & 0xffu;
            if ((header & 1u) != 0 && queue.predicateSkip) {
                cursor += count;
                continue;
            }
            // Debug aid: ANYPS5_TRACE_PM4=1 logs every executed packet; DMA_DATA also logs its
            // destination and size.
            static const bool tracePm4 = std::getenv("ANYPS5_TRACE_PM4") != nullptr;
            if (tracePm4) {
                if (opcode == 0x15 && queue.shader.contains(0x20c) && queue.shader.contains(0x20d)) std::fprintf(stderr, "[pm4] q%u op=0x15 dispatch %ux%ux%u program=0x%llx\n", static_cast<unsigned>(submission.queue), packet[1], packet[2], packet[3], static_cast<unsigned long long>((static_cast<std::uint64_t>(queue.shader.at(0x20c)) << 8u) | (static_cast<std::uint64_t>(queue.shader.at(0x20d) & 0xffu) << 40u)));
                else if (opcode == 0x50 && count >= 7) std::fprintf(stderr, "[pm4] q%u op=0x%02x dst=0x%llx bytes=0x%x control=0x%08x src=0x%08x%08x\n", static_cast<unsigned>(submission.queue), static_cast<unsigned>(opcode), static_cast<unsigned long long>(packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u)), packet[6] & 0x3ffffffu, packet[1], packet[3], packet[2]);
                else std::fprintf(stderr, "[pm4] q%u op=0x%02x dwords=%zu\n", static_cast<unsigned>(submission.queue), static_cast<unsigned>(opcode), count);
            }
            if (opcode == 0x3f) {
                spliceIndirectBuffer(submission, cursor);
                continue;
            }
            // A DMA_DATA fill (immediate source) of a registered CMASK clears it (shadPS4 FillBuffer).
            if (opcode == 0x50 && count >= 7 && (((packet[1] >> 29u) & 3u) | ((packet[6] >> 24u) & 4u) | ((packet[6] >> 25u) & 8u)) == 2u) {
                Graphics::NoteMetadataClear(packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u));
            }
            {
                PerformanceContext timingContext(frameTiming.get());
                PerformanceTimer timing("Driver.Packet");
                CheckFailure();
                timing.Mark("failure_check");
                if (opcode == 0x3c || opcode == 0x93) {
                    std::lock_guard gpuLock(gpuMutex);
                    if (device != nullptr) device->WaitIdle();
                    const GuestMemory::MemoryAccessScope memoryScope(device.get(), [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
                        if (context) static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
                    });
                    if (!Pm4::WaitSatisfied(packet)) {
                        blockedWait = Pm4::DescribeWait(packet);
                        return false;
                    }
                    timing.Mark("memory_wait");
                    cursor += count;
                    continue;
                }
                if (opcode == 0x37 || opcode == 0x40 || opcode == 0x50 || opcode == 0x42 || opcode == 0x46 || opcode == 0x49 || opcode == 0x58 || header == FlipPacketHeader) {
                    std::lock_guard gpuLock(gpuMutex);
                    timing.Mark("gpu_mutex_wait");
                    const auto eventType = opcode == 0x46 ? packet[1] & 0x3fu : 0u;
                    const auto memoryTransfer = opcode == 0x37 || opcode == 0x40 || opcode == 0x50;
                    const auto waitDraws = memoryTransfer || opcode == 0x42 || (opcode == 0x46 && (eventType == 0x07 || eventType == 0x0f || eventType == 0x10));
                    const auto gpuCacheBarrier = opcode == 0x58 && Pm4::UsesGpuCacheBarrier(packet);
                    if (device != nullptr) {
                        if (gpuCacheBarrier) device->AcquireGpuMemory();
                        else if (waitDraws) device->WaitDraws();
                        else {
                            const auto scope = header == FlipPacketHeader ? "Driver.FlipWait" : opcode == 0x58 ? "Driver.AcquireMemoryWait" : opcode == 0x49 ? "Driver.ReleaseMemoryWait" : "Driver.CacheEventWait";
                            PerformanceTimer waitTiming(scope);
                            device->WaitIdle();
                        }
                    }
                    timing.Mark(gpuCacheBarrier ? "gpu_cache_barrier" : waitDraws ? "draw_wait" : "device_idle_wait");
                }
                if (header == RenderingWaitPacketHeader) {
                    submission.renderingWaits.at(cursor)->Wait();
                    timing.Mark("rendering_wait");
                } else if (header == FlipPacketHeader) {
                    CheckFailure();
                    timing.Mark("flip_prepare");
                } else if (opcode == 0x15) {
                    dispatch(queue, packet, submission);
                } else if (opcode == 0x16) {
                    std::array<std::uint32_t, 5> direct;
                    {
                        std::lock_guard gpuLock(gpuMutex);
                        const GuestMemory::MemoryAccessScope memoryScope(device.get(), [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
                            if (context) static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
                        });
                        direct = Pm4::ResolveDispatch(packet, queue);
                    }
                    dispatch(queue, direct, submission);
                } else if (opcode == 0x35 || opcode == 0x27 || opcode == 0x2d) {
                    draw(queue, packet, submission);
                } else if (opcode != 0x42 && opcode != 0x46 && opcode != 0x58) {
                    std::lock_guard gpuLock(gpuMutex);
                    const GuestMemory::MemoryAccessScope memoryScope(device.get(), [](void* context, std::uint64_t address, std::size_t bytes, bool writable) {
                        if (context) static_cast<VulkanDevice*>(context)->ResolveMemory(address, bytes, writable);
                    });
                    Pm4::Execute(packet, queue);
                    timing.Mark("pm4_execute");
                }
                if (opcode == 0x49 && ((packet[2] >> 24u) & 7u) != 0) {
                    AgcDriverTriggerEqEvent_nid_postfix(static_cast<int>(submission.queue), packet[7]);
                    timing.Mark("release_interrupt");
                }
            }
            if (header == FlipPacketHeader) {
                frameTiming->SetFlip(submission.serial, cursor, submission.received, FrameTiming::Clock::now());
                const auto completedFrame = std::exchange(frameTiming, nullptr);
                submission.flips.at(cursor)->GpuReady(completedFrame);
            }
            cursor += count;
        }
        return true;
    }

    // Serials can finish out of order once queues interleave; waiters see the contiguous prefix.
    void markCompleted(std::uint64_t serial) {
        finishedOutOfOrder.insert(serial);
        while (!finishedOutOfOrder.empty() && *finishedOutOfOrder.begin() == completed + 1) {
            completed = *finishedOutOfOrder.begin();
            finishedOutOfOrder.erase(finishedOutOfOrder.begin());
        }
    }

    // A submission may run once every earlier active submission on its queue has finished; suspend
    // boundaries order against everything.
    bool runnable(std::size_t index) const {
        const auto& candidate = active[index];
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const auto& other = active[earlier];
            if (other.suspend || candidate.suspend || other.queue == candidate.queue) return false;
        }
        return true;
    }

    void run() noexcept {
        // Memory waits normally resolve within microseconds; one that stays blocked this long with no
        // other work able to run is a synchronization the emulation cannot satisfy.
        constexpr auto stallLimit = std::chrono::seconds(10);
        auto lastProgress = std::chrono::steady_clock::now();
        try {
            for (;;) {
                {
                    PerformanceContext timingContext(frameTiming.get());
                    PerformanceTimer timing("Driver.Worker");
                    std::unique_lock lock(mutex);
                    timing.Mark("queue_mutex_wait");
                    if (active.empty()) changed.wait(lock, [&] { return failure || stopping || !pending.empty(); });
                    timing.Mark("wait_for_submission");
                    rethrowFailure();
                    while (!pending.empty()) {
                        active.push_back(std::move(pending.front()));
                        pending.pop_front();
                        active.back().dequeued = FrameTiming::Clock::now();
                    }
                    if (active.empty()) {
                        break;
                    }
                }
                bool progressed = false;
                for (std::size_t index = 0; index < active.size();) {
                    if (!runnable(index)) {
                        ++index;
                        continue;
                    }
                    const auto startCursor = active[index].cursor;
                    const auto wasStarted = active[index].started;
                    if (!execute(active[index])) {
                        if (active[index].cursor != startCursor || !wasStarted) progressed = true;
                        ++index;
                        continue;
                    }
                    {
                        PerformanceContext timingContext(frameTiming.get());
                        PerformanceTimer timing("Driver.SubmissionCompletion");
                        std::lock_guard gpuLock(gpuMutex);
                        if (device) device->WaitIdle();
                    }
                    {
                        PerformanceContext timingContext(frameTiming.get());
                        PerformanceTimer timing("Driver.Completion");
                        std::lock_guard lock(mutex);
                        timing.Mark("mutex_wait");
                        rethrowFailure();
                        markCompleted(active[index].serial);
                    }
                    active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
                    progressed = true;
                    changed.notify_all();
                }
                if (progressed) {
                    lastProgress = std::chrono::steady_clock::now();
                    continue;
                }
                require(std::chrono::steady_clock::now() - lastProgress < stallLimit, ("GPU memory wait never satisfied: " + blockedWait).c_str());
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::microseconds(200), [&] { return failure || !pending.empty(); });
            }
            std::lock_guard gpuLock(gpuMutex);
            device.reset();
        } catch (...) {
            const auto error = std::current_exception();
            for (const auto& submission : active)
                for (const auto& [offset, flip] : submission.flips) flip->Fail(error);
            ReportFailure(error);
            {
                std::lock_guard gpuLock(gpuMutex);
                device.reset();
            }
        }
    }
};

}

void Submit(const Packet* packet, std::uint32_t queue) {
    Driver::Get().Submit(packet, queue);
}

void WaitIdle() {
    Driver::Get().WaitIdle();
}

void RegisterShader(const Shader* shader) {
    Driver::Get().RegisterShader(shader);
}

void SuspendPoint() {
    Driver::Get().SuspendPoint();
}

void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    Driver::Get().RegisterVideoOutput(handle, output);
}

void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    Driver::Get().UnregisterVideoOutput(handle, output);
}

void PresentClear(const PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context) {
    Driver::Get().Present(window, nullptr, opaque, gpuReady, context);
}

void PresentBuffer(const PresentationWindow& window, const DisplayBuffer& buffer, void (*gpuReady)(void*), void* context) {
    Driver::Get().Present(window, &buffer, true, gpuReady, context);
}

void ReleaseWindow(void* window) {
    Driver::Get().ReleaseWindow(window);
}

void ReportFailure(std::exception_ptr error) {
    Driver::Get().ReportFailure(error);
}

}

extern "C" void AgcDriverWaitIdle_nid_postfix() {
    AgcDriver::WaitIdle();
}

extern "C" void AgcDriverRegisterShader_nid_postfix(const Shader* shader) {
    AgcDriver::RegisterShader(shader);
}

extern "C" void AgcDriverSuspendPoint_nid_postfix() {
    AgcDriver::SuspendPoint();
}

extern "C" void AgcDriverRegisterVideoOutput_nid_postfix(std::uint32_t handle, const std::shared_ptr<AgcDriver::IVideoOutput>& output) {
    AgcDriver::RegisterVideoOutput(handle, output);
}

extern "C" void AgcDriverUnregisterVideoOutput_nid_postfix(std::uint32_t handle, const std::shared_ptr<AgcDriver::IVideoOutput>& output) {
    AgcDriver::UnregisterVideoOutput(handle, output);
}

extern "C" void AgcDriverPresentClear_nid_postfix(const AgcDriver::PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context) {
    AgcDriver::PresentClear(window, opaque, gpuReady, context);
}

extern "C" void AgcDriverPresentBuffer_nid_postfix(const AgcDriver::PresentationWindow& window, const AgcDriver::DisplayBuffer& buffer, void (*gpuReady)(void*), void* context) {
    AgcDriver::PresentBuffer(window, buffer, gpuReady, context);
}

extern "C" void AgcDriverReleaseWindow_nid_postfix(void* window) {
    AgcDriver::ReleaseWindow(window);
}

extern "C" void AgcDriverReportFailure_nid_postfix(std::exception_ptr error) {
    AgcDriver::ReportFailure(error);
}
