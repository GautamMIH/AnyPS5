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
- **Zero-copy guest memory (tested on this machine).** VK_EXT_external_memory_host imports memfd-backed MAP_SHARED mappings (segment aliases) on Intel, NVIDIA and llvmpipe (alignment 0x1000); GPU writes are visible through other views of the segment. NVIDIA pins the whole range at import (RSS += size) and offers host-visible+coherent (uncached) types only, so import on demand in chunks, never whole segments.
- **GuestGpuMemory (implemented).** Guest ranges translate to (segment, offset) via GuestVirtualTranslate; the segment alias is imported in 32 MiB chunks (spans for crossing ranges), kept until the segment dies (Collect at WaitIdle). Descriptors and BDA ranges use the imports; no per-draw copies or write-back. Smurfs: 2950 → 1320 ms/frame. Main image / cross-segment ranges fall back to copies. ANYPS5_GPU_MEMORY_COPY=1 forces the old path.
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

## Command buffers

- **Conditional branch (CbBranch).** INDIRECT_BUFFER with 14 dwords: mode (1 then-only, 2 then/else), compare function (WAIT_REG_MEM codes) on 64-bit (*addr & mask) vs reference; target runs as a chain (no return). Writer drops low address bits (compare 31:3, buffers 31:2) instead of rejecting. Flips in branch targets are reserved when reached. (Kyty CpOpBranch, AgcCbBranch)

## Textures

- **Texture cache cost (measured on Smurfs).** Full-content validation (memcmp against the snapshot) is cheap: ~19 ms/frame for 341 MB. The real cost was eviction thrash: a 256 MB budget counting snapshot + GPU copy holds ~15 large textures, so a ~25-texture working set missed every frame (218 MB re-read and re-detiled). Budget 1 GiB / 1024 entries: misses ~1/frame. Write-tracked validation would need page-level watches that coexist with render-target watches on shared pages; not worth it for ~20 ms.
- **Shader shape vs descriptor type.** The instruction's dimension picks the view (shadPS4 ImageViewInfo uses the shader's is_array): a 2D sample of an array/cube views the base layer; an array sample of a 2D texture views it as one layer. The descriptor type still defines the memory layout.

- **Storage images (image_store targets).** Were detiled/re-tiled per texel on the CPU around every dispatch (~370 ms/frame on Smurfs; Zorro's full-screen compute pass capped it at ~11 fps). Now on the GPU over GuestGpuMemory: detile from guest, tile back into device staging plus a byte mask (tiling an all-ones image), then a per-dword masked merge into guest. Texel-exact write-back matters: single-pass downsamplers (SPD) bind many mips in one dispatch, and tail mips share one block, so writing back whole tiledSize ranges clobbers the other mips (the CPU fallback still does). ANYPS5_CHECK_STORAGE_TILING=1 compares against the CPU tiler.

## Audio / video decode

- **libSceAudiodec codec ids / errors.** AT9 = 1, MP3 = 2, M4AAC = 3; errors 0x807F00xx (fpPS4 ps4_libsceaudiodec.pas). Ctrl = {param, bsiInfo, auInfo {size 0x18, addr, bytes}, pcmItem {size 0x18, addr, bytes}}; decode writes consumed AU bytes and PCM bytes back. Unfilled bsi (channels 0) makes games divide by zero.
- **M4AAC layouts (verified on Smurfs: 48 kHz stereo decodes).** Param 0x20: {size, wordSize (2 = float, per game's channels*4 sizing), config (1 ADTS, 2 raw — shadPS4 AJM), samplingFreqIndex, maxChannels, enableHeaac, enableNondelayOutput, interleaveOrder}. Bsi 0x14: {size, samplingFreq, channels (game indexes channel maps by channels-1), bitrate, heaac}. Raw streams carry no channel config: first element (SCE/CPE) narrows it. Decoded with system FFmpeg; shadPS4 uses fdk-aac.

- **libSceVideodec2.** Structs, errors 0x811D01xx/02xx and flow as shadPS4 videodec2 (FFmpeg H.264 codec 1 / HEVC 974921, handle = pointer, compute queue = pointer into title memory). Output NV12: luma then interleaved chroma, height aligned 16, picture info (Avc 0x78 / Hevc 0xB8) right after the frame for GetPictureInfo. Pitch must be 256-byte aligned (linear GPU row alignment, 2048 for 1920) or the title's linear texture over the frame over-reads; shadPS4 uses 64.

## Unreal Engine symptoms

- **Null store in UE shipping code** can be a deliberate fatal: e.g. `gc.EnableTimeoutOnPendingDestroyedObjectInShipping` crashes when GC waits too long for FinishDestroy — caused by very slow frames, not a missing API. Find cvar names by decoding UTF-16 strings next to the registration call.

## Fast clears / compression

- **CMASK fast clear.** Register CMASK (only with FAST_CLEAR) as cleared on first use; DMA fill or XOR-free compute write to it re-clears; draw into a cleared target fills with CLEAR_WORD; eliminate pass = that fill for target 0. DCC ignored: surfaces stay uncompressed. (shadPS4 vk_rasterizer.cpp, texture_cache.h)

## Upstream merges

- **C++ exception runtime on Linux.** Upstream (a4c519b) interposes libstdc++ process-wide with libc.prx's runtime; ours (60e4393) keeps host code on libstdc++ and serves guest code through NID exports (interposing broke try/catch in host libraries: driver, Vulkan, SDL). Kept ours on Linux, upstream's exports on Windows. tests/ExceptionRuntime.cpp assumes the process-wide model, so it is built but not registered on Linux.
- **Guest locale.** Upstream's std::locale rewrite (e526f1f) breaks titles whose inlined Dinkumware code reads locale internals (Zorro then parsed its settings wrong and asked for flip mode 2) and dropped the facet id exports (ctype<char>::id, num_put id/vtable, locale::id::_Id_cnt). Kept our layout-compatible GuestLocale; _Getptolower/_Getptoupper return short tables.
- **Game modules.** Upstream's single-file mode relinks sce_module/ ELF modules as eagerly linked guest modules (GuestModuleBuilder, --skip-sce-module); our --game mode relinks a whole dump (SELF unwrapping, runtime loading). Both kept: builder runs between PrepareModule and EmitModule in single-file mode only.
- **Bisecting a merge regression.** Build the pre-merge branch in a temp worktree (symlink its empty 3rdparty/ submodule dirs to a populated checkout), then swap .prx files into the run folder; libc.prx and libSceLibcInternal.prx must be swapped together.

## Other projects

- **OverkillLabs/DeadCells-PS5-Native** (checked 2026-09-27). Not a separate engine: a copy of upstream AnyPS5 at 1a0ebbc plus one squashed commit (1e9b001), Windows PE output, Dead Cells (PPSA15552) relinked like any title. Worth porting selectively: DISPATCH_DIRECT initiator bits (FORCE_START_AT_000, thread-count mode, COMPUTE_START_X/Y/Z), a software NGS2 mixer (libSceNgs2.native), event-flag AND/OR/CLEAR modes, FreeBSD getdents/fcntl on directory fds, SDL game controllers. Avoid: swallow-errors-and-continue policy, ISA-matched kernel replacement, RELEASE_MEM layout probing, blanket return-0 stubs, hardcoded title checks. GPL-2.0; NGS2 layouts are said to follow the Prospero SDK.

