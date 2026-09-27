# Research notes

Issues that needed research, with a short answer. Check here before researching; add an entry after.

## Memory

- **PS5 map flags / protection bits.** FIXED 0x10, NO_OVERWRITE 0x80, NO_COALESCE 0x400000; Kyty also accepts 0x400, 0x8000 and alignment `1 << (flags >> 24)`. Prot: CPU R 0x1, W 0x2, X 0x4, GPU R 0x10, W 0x20; write implies read; 0x40/0x80 are AMPR/ACP device bits (no host effect). CPU exec on direct/flexible maps → EACCES. (shadPS4 core/memory.h, Kyty kernel/memory.cpp)
- **Fixed map / reserve / unmap semantics.** Reserve = unbacked area, honours FIXED and address hints. Fixed map without NO_OVERWRITE replaces anything in range; with it, any overlap (reserved included) → ENOMEM. Unmap skips free parts, reserved parts become free. Protect on reserved changes metadata only. (shadPS4 memory.cpp MapMemory/UnmapMemory/Protect)
- **Direct memory is physical.** Mappings are views of a physical range: aliases share contents, contents survive unmap/remap; ReleaseDirectMemory unmaps every view. Flexible unmap zeroes. (shadPS4 memory.cpp, Kyty "anonymous fallbacks break aliasing")
- **Contiguous memory spans mappings.** Games treat adjacent mappings as one object; lookups must join pieces across allocations.
- **Single source of truth.** Registry and backing drift when updated separately; use one VA map (types: free, reserved, direct+phys offset, flexible, heap, image) that drives the backing. (shadPS4 vma_map) Implemented in GuestMemoryBacking: area map + memfd segments (one shared physical segment for direct memory), views mapped into claimed ranges.

## GPU access to guest memory

- **Shaders reading guest memory by address.** shadPS4/Kyty keep a persistent GPU mirror (sparse 4 GiB arenas or per-range buffers) + page table of device addresses; upload only CPU-dirty pages before submit (write-protect tracking); missing pages read 0 and set a fault bit, made resident next frame. Read source bytes through the unprotected backing alias. (shadPS4 buffer_cache, page_manager; Kyty bufferCache)
- **Storage-buffer offset alignment.** Bind at aligned-down offset, pass remainder to the shader. (shadPS4 vk_rasterizer.cpp)

## Geometry / NGG

- **Split GS halves (GsFront/GsBack).** Fuse into one mesh program: replace the front's final `s_setpc` with the back code. (Kyty ShaderRecompiler.cpp DecodeFusedProgram)
- **Merged-stage SGPRs.** User SGPRs start at s8; s0–s7 system (s3 merged_wave_info: ES verts bits 0–7, GS prims 8–15); v5 vertex id, v8 instance id. (Mesa radv_shader_args.c)
- **Mesh draw parameters.** Six push dwords: count, base/first vertex, first instance, index size (0 = none), index address lo/hi; indices read in-shader by address. (Kyty renderDraw.cpp)
- **Layer from vertex.** Misc export: layer bits 0–10, viewport 16–19; mesh writes it per primitive from the provoking vertex. Pixel ancillary VGPR: layer bits 16–28, sample 8–11. (Kyty spirvEmitterMesh.cpp, Mesa)
- **Volume render targets.** CB_COLOR_VIEW SLICE_MAX may exceed ATTRIB3 depth; clamp to depth − 1. (Kyty colorRenderTarget.cpp)

## Indirect draws

- **SET_BASE / indirect args.** SET_BASE index 1, header bit 1 = shader type (0 draw base, 1 dispatch base). Records: auto {count, instances, start vertex, start instance}; indexed {count, instances, start index, base vertex, start instance}. (Kyty CpOpSetBase, graphicsRun.cpp)
- **Patch SGPRs.** DW2 bits 0–15 base-vertex SGPR, bits 16–31 start-index SGPR (indexed, enabled by DW3 bit 27); DW3 bits 0–15 start-instance SGPR; 0x280 = none. The CP writes the values there and the vertex shader adds them itself. MULTI: DW4 bits 0–15 draw-index SGPR (bit 31 enables), bit 30 = count from memory (min with max count); indexed count clamps to INDEX_BUFFER_SIZE. (Kyty agc.cpp decode_indirect_modifier_patch_offsets)

## System services

- **Initial user login.** Some games track players only through sceUserServiceGetEvent; the boot user must arrive as one Login (type 0) event. (shadPS4 UserManager::LoginUser)
- **Save data mount points.** Mount points are short guest paths `/savedataN` (16-byte field) served from the save directory, not host paths. (shadPS4 savedata)
- **Offline NP.** Online/NP ids return SIGNED_OUT 0x80550006; bad args 0x80550003. Trophy bad args return 0x80551604 instead of failing. (shadPS4 np_manager.cpp, np_trophy.cpp)

## Fast clears / compression

- **CMASK fast clear.** Register CMASK (only with FAST_CLEAR) as cleared on first use; DMA fill or XOR-free compute write to it re-clears; draw into a cleared target fills with CLEAR_WORD; eliminate pass = that fill for target 0. DCC ignored: surfaces stay uncompressed. (shadPS4 vk_rasterizer.cpp, texture_cache.h)
