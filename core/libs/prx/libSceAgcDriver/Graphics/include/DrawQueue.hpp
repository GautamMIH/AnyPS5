#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWQUEUE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWQUEUE_HPP

#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include <map>

namespace AgcDriver::Graphics {

class DrawQueue {
public:
    ~DrawQueue();
    VkCommandBuffer Begin(const Context& context);
    void Enqueue(std::shared_ptr<ShaderResources> resources, std::shared_ptr<void> storage);
    void Flush();
    void Resolve(std::uint64_t address, std::size_t bytes);
    void Wait();
    void WaitGpu();
    void Collect();
    void RecordMemoryBarrier(const Context& context);

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
    };
    void retire(Batch batch);
    Batch recording;
    std::vector<Batch> pending;
    std::vector<std::unique_ptr<CommandBatch>> available;
    std::size_t drawCount = 0;
    WriteIndex writes;
    // A GPU access overlaps writes of queued draws: the next recording starts with a memory barrier.
    bool barrierRequested = false;
};

}

#endif
