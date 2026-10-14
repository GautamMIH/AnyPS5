#include <cstdint>
#include <cstddef>
#include <cstring>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/SonyCrt.hpp"
#include "prx/libkernel/Ampr/include/AmprPackets.hpp"
#include <array>
#include <atomic>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using AmprCommandBuffer = AmprPackets::CommandBuffer;

constexpr int AMPR_ERROR_PERMISSION = static_cast<int>(0x80020001);
constexpr int AMPR_ERROR_BUSY = static_cast<int>(0x80020010);
constexpr int AMPR_ERROR_INVALID = static_cast<int>(0x80020016);
constexpr std::uint32_t kMaxBufferSize = 0x4000000;
constexpr std::uint32_t kMaxCommands = 0x7fffffff;

int reserve(AmprCommandBuffer* cb, std::uint32_t size, std::uint32_t** out) {
    if (!cb) return AMPR_ERROR_INVALID;
    if (!cb->buffer) return AMPR_ERROR_PERMISSION;
    if (cb->bufferSize < size || cb->bufferSize - size < cb->currentOffset) return AMPR_ERROR_BUSY;
    if (cb->numCommands >= kMaxCommands) return AMPR_ERROR_INVALID;
    *out = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(cb->buffer) + cb->currentOffset);
    return 0;
}

void commit(AmprCommandBuffer* cb, std::uint32_t size, std::uint32_t flags) {
    cb->currentOffset += size;
    cb->numCommands += 1;
    cb->flags |= flags;
}

}

static constexpr int ThrdSuccess = 0;
static constexpr int ThrdNomem = 1;
static constexpr int ThrdTimedout = 2;
static constexpr int ThrdBusy = 3;
static constexpr int ThrdError = 4;

static constexpr int CppDeviceOrResourceBusy = 0;
static constexpr int CppInvalidArgument = 1;
static constexpr int CppResourceUnavailableTryAgain = 6;

static constexpr int MtxPlain = 0x1;

static std::atomic_flag sharedPtrSpinLock;

struct AmmUsageStatsData {
    std::uint64_t sizeInBytes;
    std::uint16_t numPageTablePoolEntries;
    std::uint16_t allocatedPageTablePoolEntries;
    std::uint16_t highWatermarkAllocatedEntries;
    std::uint16_t reserved;
    std::uint32_t ringIdleFlags;
};
static_assert(sizeof(AmmUsageStatsData) == 0x18);
static constexpr std::uint16_t AmmPageTablePoolEntries = 512;
static constexpr std::uint32_t AmmRingIdleFlags = 0x7;

struct GuestPad {
    unsigned (APS5_VABI* const* vtable)(GuestPad*);
    void* condition;
    void* mutex;
    bool started;
};

static_assert(offsetof(GuestPad, condition) == 0x8 && offsetof(GuestPad, mutex) == 0x10 && offsetof(GuestPad, started) == 0x18 && sizeof(GuestPad) == 0x20);

extern "C" {
int APS5_VABI pthread_create_nid_postfix(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg);
int APS5_VABI pthread_join_nid_postfix(Pthread thread, void** value);
int APS5_VABI _Mtx_init_nid_postfix(void** handle, int type);
void APS5_VABI _Mtx_destroy_nid_postfix(void** handle);
int APS5_VABI _Mtx_lock_nid_postfix(void** handle);
int APS5_VABI _Mtx_unlock_nid_postfix(void** handle);
int APS5_VABI _Cnd_init_nid_postfix(void** handle);
void APS5_VABI _Cnd_destroy_nid_postfix(void** handle);
int APS5_VABI _Cnd_wait_nid_postfix(void** condition, void** mutex);
int APS5_VABI _Cnd_signal_nid_postfix(void** handle);
int APS5_VABI wcsrtombs_s_nid_postfix(std::size_t* result, char* destination, std::size_t capacity, const std::uint16_t** source, std::size_t limit, void* state);
int APS5_VABI _Cnd_init_nid_postfix(void** handle);
int SignalCondition_nid_no_patch(void** handle);
[[noreturn]] void ThrowCppError_nid_no_patch(int code);
}

static void RequireThrd(int result, const char* what) {
    switch (result) {
    case ThrdSuccess:
        return;
    case ThrdNomem:
    case ThrdTimedout:
        ThrowCppError_nid_no_patch(CppResourceUnavailableTryAgain);
    case ThrdBusy:
        ThrowCppError_nid_no_patch(CppDeviceOrResourceBusy);
    case ThrdError:
        ThrowCppError_nid_no_patch(CppInvalidArgument);
    }
    throw std::runtime_error(std::string(what) + " failed with " + std::to_string(result));
}

static void* APS5_VABI PadEntry_nid_no_patch(void* argument) {
    auto* pad = static_cast<GuestPad*>(argument);
    const auto go = pad->vtable[0];
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(go(pad)));
}


extern "C" {

int APS5_VABI sceAmprCommandBufferConstructor(AmprCommandBuffer* cb) {
    if (cb) *cb = AmprCommandBuffer{};
    return 0;
}

int APS5_VABI sceAmprCommandBufferDestructor(AmprCommandBuffer*) {
    return 0;
}

int APS5_VABI sceAmprCommandBufferSetBuffer(AmprCommandBuffer* cb, void* buffer, uint32_t size) {
    if (!cb || !buffer) return AMPR_ERROR_INVALID;
    if (cb->buffer) return AMPR_ERROR_BUSY;
    if ((reinterpret_cast<std::uintptr_t>(buffer) & 3) != 0 || size == 0 || (size & 3) != 0 || size > kMaxBufferSize) return AMPR_ERROR_INVALID;
    cb->buffer = buffer;
    cb->bufferSize = size;
    return 0;
}

int APS5_VABI sceAmprCommandBufferReset(AmprCommandBuffer* cb) {
    if (!cb || !cb->buffer) return AMPR_ERROR_PERMISSION;
    cb->currentOffset = 0;
    cb->numCommands = 0;
    return 0;
}

void* APS5_VABI sceAmprCommandBufferClearBuffer(AmprCommandBuffer* cb) {
    if (cb->bufferSize == 0 || !cb->buffer) return nullptr;
    void* previous = cb->buffer;
    cb->buffer = nullptr;
    cb->bufferSize = 0;
    return previous;
}

uint32_t APS5_VABI sceAmprCommandBufferGetSize(const AmprCommandBuffer* cb) {
    return cb->bufferSize;
}

uint32_t APS5_VABI sceAmprCommandBufferGetCurrentOffset(const AmprCommandBuffer* cb) {
    return cb->currentOffset;
}

uint32_t APS5_VABI sceAmprCommandBufferGetNumCommands(const AmprCommandBuffer* cb) {
    return cb->numCommands;
}

int64_t APS5_VABI sceAmprCommandBufferGetType(const AmprCommandBuffer* cb) {
    return static_cast<int64_t>(static_cast<int32_t>(cb->flags));
}

void* APS5_VABI sceAmprCommandBufferGetBufferBaseAddress(const AmprCommandBuffer* cb) {
    return cb->buffer;
}

int APS5_VABI sceAmprAprCommandBufferConstructor(AmprCommandBuffer*, uint64_t* gatherState, uint64_t* scatterState) {
    if (gatherState) *gatherState = 0;
    if (scatterState) *scatterState = 0;
    return 0;
}

int APS5_VABI sceAmprAprCommandBufferDestructor(AmprCommandBuffer*, uint64_t*, uint64_t*) {
    return 0;
}

int APS5_VABI sceAmprAprCommandBufferReadFile(AmprCommandBuffer* cb, uint64_t*, uint64_t*, uint32_t file_id, void* destination, uint64_t size, uint64_t file_offset) {
    const auto address = reinterpret_cast<std::uint64_t>(destination);
    if (!AmprPackets::ReadFileArgumentsValid(address, size, file_offset)) return AMPR_ERROR_INVALID;
    const std::uint32_t packetSize = AmprPackets::ReadFileSize(file_offset);
    std::uint32_t* packet = nullptr;
    const int result = reserve(cb, packetSize, &packet);
    if (result != 0) return result;
    AmprPackets::EncodeReadFile(packet, {file_id, address, size, file_offset});
    commit(cb, packetSize, AmprPackets::kFlagReadFile);
    return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeReadFile(uint32_t, void* destination, uint64_t size, uint64_t file_offset) {
    if (!AmprPackets::ReadFileArgumentsValid(reinterpret_cast<std::uint64_t>(destination), size, file_offset)) return AMPR_ERROR_INVALID;
    return static_cast<int>(AmprPackets::ReadFileSize(file_offset));
}

int APS5_VABI sceAmprCommandBufferWriteKernelEventQueue_04_00(AmprCommandBuffer* cb, KernelEqueue eq, int32_t id, uint64_t data, uint32_t flag) {
    if (eq == 0) return AMPR_ERROR_INVALID;
    if (flag == 0 && cb && (cb->flags & AmprPackets::kFlagMapBlock) != 0) return AMPR_ERROR_PERMISSION;
    std::uint32_t* packet = nullptr;
    const int result = reserve(cb, AmprPackets::kWriteKernelEventQueueSize, &packet);
    if (result != 0) return result;
    AmprPackets::EncodeWriteKernelEventQueue(packet, {static_cast<std::uint64_t>(eq), id, data}, flag);
    commit(cb, AmprPackets::kWriteKernelEventQueueSize, 0);
    return 0;
}

int APS5_VABI sceAmprCommandBufferWriteKernelEventQueueOnCompletion(AmprCommandBuffer* cb, KernelEqueue eq, int32_t id, uint64_t data) {
    return sceAmprCommandBufferWriteKernelEventQueue_04_00(cb, eq, id, data, 0);
}

int APS5_VABI sceAmprMeasureCommandSizeWriteKernelEventQueue_04_00(KernelEqueue, int32_t, uint64_t, uint32_t) {
    return static_cast<int>(AmprPackets::kWriteKernelEventQueueSize);
}


int APS5_VABI sceAmprCommandBufferWriteAddress_04_00(AmprCommandBuffer* cb, uint64_t* address, uint64_t value, uint32_t flags) {
    const auto target = reinterpret_cast<std::uint64_t>(address);
    if (target == 0 || (target & 7) != 0 || flags > 0xffff) return AMPR_ERROR_INVALID;
    std::uint32_t* packet = nullptr;
    const int result = reserve(cb, AmprPackets::kWriteAddressSize, &packet);
    if (result != 0) return result;
    AmprPackets::EncodeWriteAddress(packet, {target, value, static_cast<std::uint16_t>(flags)});
    commit(cb, AmprPackets::kWriteAddressSize, 0);
    return 0;
}

uint32_t APS5_VABI sceAmprMeasureCommandSizeWriteAddress_04_00(void) {
    return AmprPackets::kWriteAddressSize;
}

// The OnCompletion forms (upstream's exports) are the _04_00 commands without flags.
int APS5_VABI sceAmprCommandBufferWriteAddressOnCompletion(AmprCommandBuffer* cb, uint64_t* address, uint64_t value) {
    return sceAmprCommandBufferWriteAddress_04_00(cb, address, value, 0);
}

uint32_t APS5_VABI sceAmprMeasureCommandSizeWriteAddressOnCompletion(void) {
    return AmprPackets::kWriteAddressSize;
}

uint32_t APS5_VABI sceAmprMeasureCommandSizeWriteKernelEventQueueOnCompletion(void) {
    return AmprPackets::kWriteKernelEventQueueSize;
}

// No packet encoding (and no executor in libkernel) exists yet for waits and counters.
int APS5_VABI sceAmprCommandBufferWriteCounterOnCompletion() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWaitOnAddress() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWaitOnCounter() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWriteAddressFromTimeCounterOnCompletion() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWriteAddressFromCounterOnCompletion() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWriteAddressFromCounterPairOnCompletion() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeWriteCounterOnCompletion() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeWaitOnAddress() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeWaitOnCounter() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprAprCommandBufferMapBegin() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprAprCommandBufferMapDirectBegin() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprAprCommandBufferMapEnd() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprAprCommandBufferReadFileGather() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprAprCommandBufferReadFileGatherScatter() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprAprCommandBufferReadFileScatter() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprAprCommandBufferResetGatherScatterState() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferConstructMarker() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferConstructNop() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferNop() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferNopWithData() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferPopMarker() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferPushMarker() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferPushMarkerWithColor() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferSetMarker() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferSetMarkerWithColor() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWaitOnAddress_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWaitOnCounter_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWriteAddressFromCounterPair_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWriteAddressFromCounter_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWriteAddressFromTimeCounter_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprCommandBufferWriteCounter_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeMapBegin() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeMapDirectBegin() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeMapEnd() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeNop() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeNopWithData() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizePopMarker() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizePushMarker() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizePushMarkerWithColor() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeReadFileGather() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeReadFileGatherScatter() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeReadFileScatter() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeResetGatherScatterState() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeSetMarker() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeSetMarkerWithColor() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeWaitOnAddress_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeWaitOnCounter_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeWriteAddressFromCounterPair_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeWriteAddressFromCounter_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeWriteAddressFromTimeCounter_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAmprMeasureCommandSizeWriteCounter_04_00() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

[[noreturn]] void APS5_VABI _ZSt16_Throw_Cpp_errori_nid_postfix(int code) {
    ThrowCppError_nid_no_patch(code);
}


void APS5_VABI _ZNSt4_Pad8_ReleaseEv_nid_postfix(GuestPad* pad) {
    if (!pad) APS5_INVALID_ARG_EX;
    RequireThrd(_Mtx_lock_nid_postfix(&pad->mutex), "_Pad::_Release: _Mtx_lock");
    pad->started = true;
    RequireThrd(_Cnd_signal_nid_postfix(&pad->condition), "_Pad::_Release: _Cnd_signal");
    RequireThrd(_Mtx_unlock_nid_postfix(&pad->mutex), "_Pad::_Release: _Mtx_unlock");
}

void APS5_VABI _ZNSt4_PadC2Ev_nid_postfix(GuestPad* pad) {
    if (!pad) APS5_INVALID_ARG_EX;
    RequireThrd(_Cnd_init_nid_postfix(&pad->condition), "_Pad::_Pad: _Cnd_init");
    const int created = _Mtx_init_nid_postfix(&pad->mutex, MtxPlain);
    if (created != ThrdSuccess) {
        _Cnd_destroy_nid_postfix(&pad->condition);
        RequireThrd(created, "_Pad::_Pad: _Mtx_init");
    }
    pad->started = false;
    const int locked = _Mtx_lock_nid_postfix(&pad->mutex);
    if (locked != ThrdSuccess) {
        _Mtx_destroy_nid_postfix(&pad->mutex);
        _Cnd_destroy_nid_postfix(&pad->condition);
        RequireThrd(locked, "_Pad::_Pad: _Mtx_lock");
    }
}

void APS5_VABI _ZNSt4_PadD2Ev_nid_postfix(GuestPad* pad) {
    if (!pad) APS5_INVALID_ARG_EX;
    RequireThrd(_Mtx_unlock_nid_postfix(&pad->mutex), "_Pad::~_Pad: _Mtx_unlock");
    _Mtx_destroy_nid_postfix(&pad->mutex);
    _Cnd_destroy_nid_postfix(&pad->condition);
}


void APS5_VABI _ZNSt4_Pad7_LaunchEPP7pthread_nid_postfix(GuestPad* pad, Pthread* thread) {
    if (!pad || !thread) APS5_INVALID_ARG_EX;
    const int created = pthread_create_nid_postfix(thread, nullptr, PadEntry_nid_no_patch, pad);
    if (created != 0) ThrowCppError_nid_no_patch(CppResourceUnavailableTryAgain);
    while (!pad->started) RequireThrd(_Cnd_wait_nid_postfix(&pad->condition, &pad->mutex), "_Pad::_Launch: _Cnd_wait");
}

int APS5_VABI _ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE2idE_nid_postfix(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}


void APS5_VABI _Unlock_shared_ptr_spin_lock_nid_postfix(void) {
    sharedPtrSpinLock.clear(std::memory_order_release);
}

int APS5_VABI _Cnd_init_with_name_nid_postfix(void** handle, const char*) {
    return _Cnd_init_nid_postfix(handle);
}

int APS5_VABI _ZTVSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE_nid_postfix(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI _ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE3getES3_S3_RSt8ios_baseRNSt5_IosbIiE8_IostateEP2tmPKcSE__nid_postfix(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI _ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE2idE_nid_postfix(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI _ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE7_GetcatEPPKNSt6locale5facetEPKS5__nid_postfix(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

void APS5_VABI _Lock_shared_ptr_spin_lock_nid_postfix(void) {
    while (sharedPtrSpinLock.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
}


int APS5_VABI wcstombs_s_nid_postfix(std::size_t* result, char* destination, std::size_t capacity, const std::uint16_t* source, std::size_t limit) {
    std::array<std::uint64_t, 16> state{};
    return wcsrtombs_s_nid_postfix(result, destination, capacity, &source, limit, state.data());
}

int APS5_VABI __cxa_call_unexpected_nid_postfix(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

APS5_EXPORT("0hlfW1O4Aa4", sceAmprLocaleconv);
SonyLconv* APS5_VABI sceAmprLocaleconv(void) {
    return SonyLocaleconv();
}

APS5_EXPORT("802pFCwC9w0", sceAmprUdivti3);
unsigned __int128 APS5_VABI sceAmprUdivti3(unsigned __int128 dividend, unsigned __int128 divisor) {
    return SonyUdivti3(dividend, divisor);
}

APS5_EXPORT("MU25eqxSDTw", sceAmprSinh);
double APS5_VABI sceAmprSinh(double x, double y) {
    return SonySinh(x, y);
}

APS5_EXPORT("-vXEQdRADLI", sceAmprStrtokS);
char* APS5_VABI sceAmprStrtokS(char* str, const char* delim, char** context) {
    return SonyStrtokS(str, delim, context);
}

APS5_EXPORT("2UFh+YKfuzk", sceAmprCtimeS);
int APS5_VABI sceAmprCtimeS(char* buffer, std::size_t size, const std::int64_t* time) {
    return SonyCtimeS(buffer, size, time);
}

APS5_EXPORT("6f5f-qx4ucA", sceAmprWcscpyS);
int APS5_VABI sceAmprWcscpyS(std::uint16_t* dest, std::size_t size, const std::uint16_t* src) {
    return SonyWcscpyS(dest, size, src);
}

APS5_EXPORT("ENLfKJEZTjE", sceAmprSetConstraintHandlerS);
void* APS5_VABI sceAmprSetConstraintHandlerS(void* handler) {
    return SonySetConstraintHandlerS(handler);
}

APS5_EXPORT("Jo9ON-AX9eU", sceAmprFltrounds);
int APS5_VABI sceAmprFltrounds(void) {
    return SonyFltrounds();
}

APS5_EXPORT("ODGONXcSmz4", sceAmprIgnoreHandlerS);
void APS5_VABI sceAmprIgnoreHandlerS(const char* message, void* pointer, int error) {
    SonyIgnoreHandlerS(message, pointer, error);
}

APS5_EXPORT("VUzjXknPPBs", sceAmprMbstowcs);
std::size_t APS5_VABI sceAmprMbstowcs(std::uint16_t* dest, const char* src, std::size_t size) {
    return SonyMbstowcs(dest, src, size);
}

APS5_EXPORT("qPe7-h5Jnuc", sceAmprAsctimeS);
int APS5_VABI sceAmprAsctimeS(char* buffer, std::size_t size, const GuestTm* time) {
    return SonyAsctimeS(buffer, size, time);
}

APS5_EXPORT("qlWiRfOJx1A", sceAmprFpclassifyd);
int APS5_VABI sceAmprFpclassifyd(double value) {
    return SonyFpclassifyd(value);
}

int APS5_VABI sceAmprAmmGetUsageStatsData(AmmUsageStatsData* stats) {
    if (!stats || stats->sizeInBytes > sizeof(AmmUsageStatsData)) return AMPR_ERROR_INVALID;
    AmmUsageStatsData out{};
    out.sizeInBytes = stats->sizeInBytes;
    out.numPageTablePoolEntries = AmmPageTablePoolEntries;
    out.ringIdleFlags = AmmRingIdleFlags;
    std::memcpy(stats, &out, static_cast<std::size_t>(stats->sizeInBytes));
    return 0;
}

int APS5_VABI sceAmprAmmSetPageTablePoolOccupancyNotificationThreshold(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
}
