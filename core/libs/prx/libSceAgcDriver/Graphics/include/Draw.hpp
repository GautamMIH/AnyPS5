#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP

#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"

namespace AgcDriver::Graphics {

// Push-constant bytes holding a mesh draw's parameters: vertex or index count, base vertex, first
// instance, index element bytes (0 without indices) and the index buffer address (low, high).
constexpr std::uint32_t MeshDrawParameterBytes = 6u * sizeof(std::uint32_t);

void Draw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots = {});

}

#endif
