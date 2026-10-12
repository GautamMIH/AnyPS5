#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWQUEUE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWQUEUE_HPP

#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include <deque>
#include <map>

namespace AgcDriver::Graphics {

class DrawQueue {
public:
    ~DrawQueue();
    // Room for `bytes` of shader data in a chunk shared by queued work (one pooled host-visible
    // buffer per DataChunkBytes instead of one per data buffer): the chunk, which returns to the
    // buffer pool once the last work holding it retires, and the offset. Null when it does not fit.
    static constexpr std::size_t DataChunkBytes = 1u << 20u;
    std::pair<std::shared_ptr<Buffer>, VkDeviceSize> TakeData(const Context& context, std::size_t bytes);
    VkCommandBuffer Begin(const Context& context);
    void Enqueue(std::shared_ptr<ShaderResources> resources, std::shared_ptr<void> storage);
    // A GPU write of guest memory recorded in the current batch that is not a draw (a resident
    // surface written back into imported guest memory): CPU accesses to [begin, end) wait for the
    // batch like they wait for draw writes, and storage stays alive until it completes.
    // Returns the write's serial (see LastWriter).
    std::uint64_t NoteGuestWrite(std::uint64_t begin, std::uint64_t end, std::shared_ptr<void> storage);
    // The serial of the last queued entry (not yet retired) that writes [address, address + bytes),
    // or 0 when none does.
    std::uint64_t LastWriter(std::uint64_t address, std::size_t bytes) const;
    void Flush();
    // Markers may ride on the batch being recorded instead of submitting it (SubmitMarker): only when
    // something submits an open batch eventually, the pipelined driver's idle flush.
    static void DeferMarkers(bool defer);
    // The submission-timeline value a CPU access of [address, address + bytes) waits for before
    // Resolve finds the range's writers complete (0: none pending, or no timeline). A batch being
    // recorded that writes the range is submitted first.
    std::uint64_t WriterTimeline(std::uint64_t address, std::size_t bytes);
    void Resolve(std::uint64_t address, std::size_t bytes);
    void Wait();
    void WaitGpu();
    void Collect();
    void RecordMemoryBarrier(const Context& context);
    // A full memory barrier (all writes before it visible to everything after it) was recorded after
    // every entry queued so far (VulkanDevice::Dispatch's trailing barrier).
    void NoteFullBarrier() { barrierSerial = nextEntrySerial - 1; }
    // Render-pass merging: the last draw leaves its pass open (KeepPassOpen); a following draw with
    // the same pass key that records nothing outside a pass continues it (ContinuePass returns the
    // batch's commands, or null). Every other recording (Begin), Flush and the end of the batch
    // close it first, so work outside the pass stays in order.
    VkCommandBuffer ContinuePass(const std::vector<std::uint64_t>& key);
    void KeepPassOpen(const Context& context, const std::vector<std::uint64_t>& key);
    void EndPass();
    // Completion markers: SubmitMarker submits everything queued, then an empty batch whose fence
    // signals once all earlier GPU work on the queue finished (one queue, in order). Marker serials
    // increase; MarkerReached and WaitMarker retire the draws completed by then (their writes reach
    // guest memory) before reporting the marker done.
    std::uint64_t SubmitMarker(const Context& context);
    bool MarkerReached(std::uint64_t serial);
    void WaitMarker(std::uint64_t serial);
    // Occlusion counting (PIXEL_PIPE_STAT_DUMP): once enabled, every batch recorded afterwards runs
    // inside an occlusion query (precise when the device allows) whose result is added to a running
    // sample count when the batch is retired. SamplesPassed covers the retired batches.
    void EnableSampleCounting(const Context& context, bool precise);
    std::uint64_t SamplesPassed() const { return samplesPassed; }

private:
    struct Entry {
        std::shared_ptr<void> storage;
        std::shared_ptr<ShaderResources> resources;
        // The ranges this draw may write (fixed once it is queued), as indexed in `writes`.
        std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
        std::uint64_t serial = 0;
    };
    std::uint64_t nextEntrySerial = 1;
    // The write ranges of every queued draw until its batch is retired, so a range check (made
    // thousands of times a frame) finds overlapping writes without visiting each draw.
    class WriteIndex {
    public:
        void Add(std::uint64_t begin, std::uint64_t end);
        void Remove(std::uint64_t begin, std::uint64_t end);
        bool Overlaps(std::uint64_t address, std::size_t bytes) const;

    private:
        std::multimap<std::uint64_t, std::uint64_t> ranges;  // begin -> end
        // No indexed range is longer (an upper bound; reset when the index empties).
        std::uint64_t longest = 0;
    };
    struct Batch {
        std::vector<Entry> entries;
        std::unique_ptr<CommandBatch> commands;
        bool hasBarrier = false;
        // The occlusion query around the batch, or -1.
        std::int32_t query = -1;
        // A marker this batch's completion stands for (SubmitMarker), or 0.
        std::uint64_t marker = 0;
    };
    // CommandBatch::Submissions() right after the last batch was submitted.
    std::uint64_t lastBatchSubmission = 0;
    // The latest marker reached by retiring the batch carrying it.
    std::uint64_t retiredMarker = 0;
    struct SampleCounter {
        explicit SampleCounter(const Context& context, bool precise);
        ~SampleCounter();
        Context context;
        bool precise;
        VkQueryPool pool = VK_NULL_HANDLE;
        std::vector<std::uint32_t> free;
    };
    std::unique_ptr<SampleCounter> samples;
    std::uint64_t samplesPassed = 0;
    void retire(Batch batch);
    bool passOpen = false;
    std::vector<std::uint64_t> openPassKey;
    PFN_vkCmdEndRenderPass endRenderPass = nullptr;
    PFN_vkCmdPipelineBarrier pipelineBarrier = nullptr;
    Batch recording;
    std::vector<Batch> pending;
    std::vector<std::unique_ptr<CommandBatch>> available;
    std::size_t drawCount = 0;
    struct Marker {
        std::uint64_t serial;
        std::unique_ptr<CommandBatch> commands;
    };
    std::deque<Marker> markers;
    std::uint64_t nextMarker = 1;
    std::uint64_t reachedMarker = 0;
    void collectMarkers(bool wait, std::uint64_t serial);
    WriteIndex writes;
    // A GPU access overlaps writes of queued draws: the next recording starts with a memory barrier.
    bool barrierRequested = false;
    // Entries up to this serial are ordered before everything recorded since by a full barrier: a GPU
    // access overlapping only their writes needs no new barrier and leaves the open render pass
    // going (the idea of upstream's PassHazards, 0211ee7c/ecc14b0f: hazards count only against the
    // work since the last barrier). ANYPS5_BARRIER_PER_OVERLAP=1 requests one for every overlap.
    std::uint64_t barrierSerial = 0;
    std::shared_ptr<Buffer> dataChunk;
    std::size_t dataChunkUsed = 0;
    // Work was recorded (Begin) since the last full memory barrier (RecordMemoryBarrier).
    bool workSinceBarrier = true;
};

}

#endif
