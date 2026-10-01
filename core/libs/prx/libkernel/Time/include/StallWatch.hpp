#ifndef CORE_LIBS_PRX_LIBKERNEL_TIME_INCLUDE_STALLWATCH_HPP
#define CORE_LIBS_PRX_LIBKERNEL_TIME_INCLUDE_STALLWATCH_HPP

// Debug aid: APS5_TRACE_STALLS=<seconds> reports every guest wait (semaphore, event queue, event
// flag, condition variable, mutex, join) outstanding for longer than that, once, with the guest's
// return address and the two callers above it (from its frame pointers). A thread blocked for good
// shows up here, where the completed-wait traces never see it.
namespace KernelStallWatch {

void Begin(const char* kind, const void* object, const void* caller, const void* guestFrame);
void End();

class Scope {
public:
    Scope(const char* kind, const void* object, const void* caller, const void* guestFrame) { Begin(kind, object, caller, guestFrame); }
    ~Scope() { End(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}

// In a function the guest calls: watches the rest of the function's scope.
#define APS5_STALL_WATCH(kind, object) \
    const KernelStallWatch::Scope stallWatch_((kind), (object), __builtin_return_address(0), *static_cast<void* const*>(__builtin_frame_address(0)))

#endif
