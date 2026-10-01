#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWQUEUE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWQUEUE_HPP

#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include <deque>
#include <map>

namespace AgcDriver::Graphics {

class DrawQueue {
public:
    ~DrawQueue();
    VkCommandBuffer Begin(const Context& context);
    void Enqueue(std::shared_ptr<ShaderResources> resources, std::shared_ptr<void> storage);
    // A GPU write of guest memory recorded in the current batch that is not a draw (a resident
    // surface written back into imported guest memory): CPU accesses to [begin, end) wait for the
    // batch like they wait for draw writes, and storage stays alive until it completes.
    void NoteGuestWrite(std::uint64_t begin, std::uint64_t end, std::shared_ptr<void> storage);
    void Flush();
    void Resolve(std::uint64_t address, std::size_t bytes);
    void Wait();
    void WaitGpu();
    void Collect();
    void RecordMemoryBarrier(const Context& context);
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
    };
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
    };
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
};

}

#endif
