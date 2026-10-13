# Per-draw shader data flow: compact draw records

Status: design (2026-10-12). Goal: remove the per-draw `RecompileResult` the worker builds and the
device thread re-walks, the largest remaining per-draw CPU cost after the pipelined driver.

## Why

Hellboy, normal configuration, 4K: 68 ms a frame (~15 fps); worker 37.6 ms and device 34.6 ms of CPU
a frame. KytyPS5 runs the same scene at 30 fps with one GPU thread at ~25 ms a frame.

Per draw, today (Hellboy profile, glibc, so allocation is overstated by ~2x):

| Step | Thread | Sampled ms/frame |
|---|---|---|
| `materializeResult`: copy the artifact fields and binding templates, build the plan and module keys | worker | 3.3 |
| `DescriptorBindingBuilder::Populate`: a vector per binding for `guestDescriptor`, shader data, push bytes | worker | 1.8 (1.2 allocation) |
| result destruction (now on the reaper, b638a1cbc) | worker → reaper | 2.8 |
| `ShaderResources::build` and image and sampler paths: walk bindings, decode words | device | ~7 |

Most draws can't reuse a result: their buffer and image base addresses differ per draw. Zorro's
result memo hits ~75% of lookups, but ~470k materializations per 10 s bypass it.

KytyPS5 (`src/graphics/host_gpu/renderer/pipeline/descriptors.cpp`) carries per draw only
`ShaderStageRuntime{program*, snapshot*}`. The GPU thread resolves bindings into `PreparedBindings`,
scratch vectors it clears and resizes but never frees, and writes them with `vkCmdPushDescriptorSetKHR`.
It keeps no per-draw result object.

## What is per variant and what is per draw

Inventory (full table in the 2026-10-12 research note). The effective per-variant unit is the
**(CompiledVariant, PreparedBindingPlan, PreparedModuleEntry) triple**, identified by
`PipelineVariantId()` (= `specializationId`).

- **Per variant, already:**
  - every `CompiledShaderArtifact` field; the module's `spirv`;
  - `specializationId`, `pushUserData`, `bdaWrites`, `usesBitwiseXor`, `workgroupMemoryDwords`;
  - for every `DescriptorBinding`, every field except `guestDescriptor`: kind, role, set/binding,
    count, image shape, all flag vectors;
  - the sampler words (all four are in the plan key);
  - the specialization constants (baked into the module by `specializeModule`; materialized
    results carry an empty `specialization`).
- **Per draw:**
  - buffer V#s (4 words each), image T#s (8 words each, with the identity-swizzle patch);
  - the flattened SRT words;
  - shader data / push bytes: user data, partial threads, image metadata, misalignment bytes;
  - vertex attributes (the draw's V# and fetch index);
  - `poisonedSrtReads`.

## Design

### 1. `ShaderVariant`: shared, immutable, per variant

Promote `PreparedModuleEntry` into a public immutable object, held by `shared_ptr`, created once per
(variant, plan, module) and cached where the module is cached today. It holds:

- the artifact fields consumers read: spirv, subgroup size, ABI versions, vertex/instance offset
  SGPRs, fragment parameters and parameter exports (rect-list and barycentric builders), and
  `runtimeImageCount`/`runtimeImageResources`/`imageMetadataDword`/`shaderDataDwords`;
- the binding templates (all of `DescriptorBinding` except `guestDescriptor`), plus a **word layout**:
  for each binding, the offset and length of its words in the draw record, and the snapshot indices
  they come from;
- the patches to apply while copying words (identity swizzle, sampler point filter), and the
  constant sampler words;
- `pushUserData` and the push layout;
- facts consumers compute per draw today, precomputed once:
  - the `ShaderResources::LayoutKey` and the push stages;
  - the written-buffer element mask (`appendShaderWrites`) and the any-write flags (`recordDispatch`);
  - the validation verdict (ShaderValidation already keys on variant ids only).

### 2. `ShaderDraw`: the compact per-draw record

```cpp
struct ShaderDraw {
    const ShaderVariant* variant;   // kept alive by the job (one shared_ptr per distinct variant)
    std::span<const std::uint32_t> words;  // buffers, images, SRT, shader data, in layout order
    std::span<const std::byte> push;       // push bytes (aliases the shader-data words when push-only)
    std::span<const VertexAttribute> vertexAttributes;
    std::uint32_t poisonedSrtReads;
};
```

The spans point into a **per-job arena**: a bump allocator the job owns and the reaper frees with the
job. The worker writes each draw's words straight from the snapshot into the arena: one copy, with no
allocation per binding or per draw. Sampler words aren't copied; they live in the variant.

### 3. Selecting the variant without per-draw key building

Choosing the plan and module builds two keys per draw today, part of the 3.3 ms. Cache the selected
`ShaderVariant` per capture:

- **CaptureMemo hit:** the snapshot is the same object, so the variant cached beside it is valid
  (store `{SnapshotId, request hash} → ShaderVariant*` in the existing thread-local 1024-slot cache).
- **Capture miss:** build the plan key as today. It is cheaper than materializing.

### 4. Consumers

- **Device thread:**
  - `ShaderResources::build`, `addImageBinding` and the sampler path walk `variant->bindings` and read
    words at `layout[i].offset`; resolution scratch is reused across draws, as in Kyty's `PreparedBindings`;
  - `ValidateRuntimeResources`, `prepareAddressBindings`;
  - `Shaders.hpp` `CompiledShader`/`PushData`;
  - `GraphicsPipelineCache::makeKey`, Pipeline.cpp, VulkanDevice Dispatch and EnqueueDraw, and the
    vertex-buffer code in Draw.cpp.
- **Worker:** `appendShaderWrites`, `recordDispatch`, `queueDraw`, `queueDispatch`,
  `preparedRectangle`, and the push path (`ApplyRuntimeUserData` becomes building the push bytes from
  user data directly).
- **Unchanged:**
  - `Recompile(request)`, the full result used by tests and tools;
  - the shader disk cache, which stores `CompiledVariant`s and module blobs, not results
    (`EncodeResult` has no non-test callers).

### 5. Synthetic shaders

Rect-list and barycentric shaders are built from results and have `variantId` 0. Add
`ShaderVariant::FromResult(const RecompileResult&)`: an adapter that wraps a full result as a variant
plus one fixed draw record. Their pipeline key keeps hashing the SPIR-V. The adapter is also the
migration path: any consumer can be fed either source.

## Revision after stage 1 (2026-10-12)

- **Seam.** The device thread reads per-variant fields through `CompiledShader::program`, ~30 sites;
  only about a dozen read per-draw data. So the shared variant stays a `RecompileResult` (one per
  specialized module, words empty, cached with the module entry, which is never evicted). Per-draw
  data is read through `CompiledShader::Words(binding)`, `VertexAttributes()` and `PushData()`.
  Stage 1 routed every device-side per-draw read through these accessors; they still return the
  full result's data. Stage 2 points them at the compact record.
- **Device-side estimate corrected.** `ShaderResources::build` (7.4 ms sampled) is mostly real
  resource work: guest buffers 1.5, image lookups 1.45, uploads 1.1, descriptor cache 0.5. Walking
  the binding metadata is only `build`'s own 0.65 ms. Reachable on the device: `makeKey` (1.1),
  `build`'s own walk (0.65), and the per-draw `ShaderResources` allocation and free (~2, glibc). That
  is about 1–2 ms of real time, not 3–5. The worker estimate stands.

## Stages

Each stage builds, passes the 657 tests, and is A/B'd on Zorro (hidden, 2 rounds, normal
configuration). Stages 2–4 are also checked visually and timed on Hellboy. A switch
(`ANYPS5_SHADER_RESULTS=1`) keeps the old path until stage 5.

1. **`ShaderVariant` + `FromResult`.** Device-thread consumers read through `ShaderDraw`, fed by the
   adapter from today's results. No behaviour change; it proves the consumer rewrite in isolation.
   Expected gain: precomputed layout keys and write masks.
2. **Arena + direct record building on the worker.** A new `Recompile(request, capture) → ShaderDraw`
   overload: select the variant, copy words into the job arena. Removes materializeResult and
   Populate for the pipelined path.
3. **Variant cached per capture**, which skips plan and module key building on CaptureMemo hits.
4. **Worker consumers** (`appendShaderWrites`, dispatch) on precomputed masks.
5. **Retire the per-draw result path:** the result memo and the 1024-slot result cache. The
   standalone `Recompile` API stays.

## Expected effect, and how we'll know

- **Worker:** most of materializeResult and Populate, plus the destruction the reaper now absorbs.
  Estimate 4–6 ms a frame of real (mimalloc) CPU in Hellboy.
- **Device thread:** about 1–2 ms (see the revision above).
- **Hellboy target:** ~68 → ~55–60 ms a frame at 4K. This is an estimate; each stage is measured, and a stage that doesn't pay is reverted.

## Risks

- **Lifetimes:** the device thread holds raw pointers today, kept alive by `Job::results`; the job
  must now own the arena and one `shared_ptr` per distinct variant.
- **Memory:** the plan map is unbounded and never evicts. Variants hang off it, so the same growth
  applies; add a count to the stats line and watch it.
- **Push/shader data aliasing:** when the layout uses push data, the shader data *is* the push data,
  including the per-draw image metadata words. The record keeps one copy and both spans view it.
- **Dispatch:** `partialThreads` is per dispatch; the non-pipelined `recordDispatch` reads words on
  the worker. Both read the record the same way.
- **Contract roles:** BDA, fault buffer and GDS carry no words but keep their checks. ShaderData and
  FlattenedSrt are contents, not V#s, and are never written back.
- **Struct changes:** ShaderDiskCache's static_asserts fire on changes to the stored structs (the
  intent is not to change them).
