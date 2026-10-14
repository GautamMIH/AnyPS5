#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTMEMORYTRACKING_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTMEMORYTRACKING_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <functional>

namespace GuestMemoryTracking {

enum class Access { Read, Write, Invalidate };
enum class Protection { None, Read, ReadWrite };
using Resolver = void (*)(void*, Access);

extern "C" {
std::recursive_mutex& GuestMemoryTrackingMutex_nid_postfix();
std::size_t GuestMemoryTrackingPageSize_nid_postfix();
void* GuestMemoryTrackingCreate_nid_postfix(std::uint64_t address, std::size_t bytes, void* context, Resolver resolver);
void GuestMemoryTrackingDestroy_nid_postfix(void* handle) noexcept;
void GuestMemoryTrackingProtect_nid_postfix(void* handle, Protection protection);
void GuestMemoryTrackingResolve_nid_postfix(std::uint64_t address, std::size_t bytes, bool writable);
void GuestMemoryTrackingInvalidate_nid_postfix(std::uint64_t address, std::size_t bytes);
void GuestMemoryTrackingValidate_nid_postfix(std::uint64_t address, std::size_t bytes, const std::function<void(std::uint64_t, std::size_t)>& validate);
}

class Watch {
public:
    Watch(std::uint64_t address, std::size_t bytes, void* context, Resolver resolver) : handle(GuestMemoryTrackingCreate_nid_postfix(address, bytes, context, resolver)) {}
    ~Watch() { GuestMemoryTrackingDestroy_nid_postfix(handle); }
    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
    void Protect(Protection protection) { GuestMemoryTrackingProtect_nid_postfix(handle, protection); }

private:
    void* handle;
};

// A host write into guest memory (a socket or file read into a guest buffer): the range's tracking is
// resolved writable first, so watches see it as written. Open() is false for a null pointer with a
// non-zero size (the caller fails with EFAULT). Matches upstream's GuestArena::HostWrite interface.
class HostWrite {
public:
    HostWrite(void* pointer, std::size_t bytes) : open(bytes == 0 || pointer != nullptr) {
        if (open && bytes != 0) GuestMemoryTrackingResolve_nid_postfix(reinterpret_cast<std::uint64_t>(pointer), bytes, true);
    }
    HostWrite(const HostWrite&) = delete;
    HostWrite& operator=(const HostWrite&) = delete;
    bool Open() const { return open; }

private:
    bool open;
};

}

#endif
