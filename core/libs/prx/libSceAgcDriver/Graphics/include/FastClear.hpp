#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FASTCLEAR_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_FASTCLEAR_HPP

#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>

namespace AgcDriver::Graphics {

// CMASK fast clears, following shadPS4 (vk_rasterizer.cpp, texture_cache.h). The CMASK of a
// colour target with CB_COLOR_INFO.FAST_CLEAR is registered when the target is first drawn to,
// and a registered CMASK starts out cleared. It is cleared again when a DMA fill or a compute
// clear writes to its address (NoteMetadataClear). A draw into a target whose CMASK is cleared
// first fills the surface with the CB_COLOR_CLEAR_WORD colour; the target is then no longer
// cleared. An ELIMINATE_FAST_CLEAR pass only does that fill, for colour target 0.
// Must run inside a guest memory access scope so cached copies of the surface are resolved.
void ApplyFastClears(const State& state);

// A write whose destination starts exactly at a registered CMASK or HTILE (shadPS4's ClearMeta).
void NoteMetadataClear(std::uint64_t address);

// HTILE depth fast clears, the same way: a depth target's HTILE is registered as cleared when the
// target is first drawn to and cleared again by NoteMetadataClear. Returns whether the draw must
// first clear the depth (and stencil) planes to DB_DEPTH_CLEAR / DB_STENCIL_CLEAR; the HTILE is
// then no longer cleared.
bool TakeDepthFastClear(const State& state);

}

#endif
