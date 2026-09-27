#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"


extern "C" {

// Initialization succeeds as on an offline console: the title tears its whole web API layer down on
// an init failure and later dereferences it anyway. Requests made through it fail as offline.
int APS5_VABI _ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE(void* self, const void* parameter) {
    (void)self;
    (void)parameter;
    return 0;
}

// sce::Json::InitParameter2 (layout cross-checked with KytyPS5).
struct JsonInitParameter2 {
    void* allocator;
    void* userData;
    std::size_t fileBufferSize;
    std::uint32_t specialFloatFormatType;
    std::uint32_t reserved[3];
};
static_assert(sizeof(JsonInitParameter2) == 40);

JsonInitParameter2* APS5_VABI _ZN3sce4Json14InitParameter2C1Ev(JsonInitParameter2* self) {
    if (self == nullptr) APS5_INVALID_ARG_EX;
    *self = JsonInitParameter2{};
    return self;
}

void APS5_VABI _ZN3sce4Json14InitParameter212setAllocatorEPNS0_12MemAllocatorEPv(JsonInitParameter2* self, void* allocator, void* userData) {
    if (self == nullptr) APS5_INVALID_ARG_EX;
    self->allocator = allocator;
    self->userData = userData;
}

void APS5_VABI _ZN3sce4Json14InitParameter217setFileBufferSizeEm(JsonInitParameter2* self, std::size_t size) {
    if (self == nullptr) APS5_INVALID_ARG_EX;
    self->fileBufferSize = size;
}

// Same offline-console behaviour as the InitParameter overload above.
int APS5_VABI _ZN3sce4Json11Initializer10initializeEPKNS0_14InitParameter2E(void* self, const JsonInitParameter2* parameter) {
    (void)self;
    if (parameter == nullptr) APS5_INVALID_ARG_EX;
    return 0;
}

int APS5_VABI _ZN3sce4Json11InitializerC1Ev(void* self) {
    (void)self;
    return 0;
}

int APS5_VABI _ZN3sce4Json11InitializerD1Ev(void* self) {
    (void)self;
    return 0;
}

int APS5_VABI _ZN3sce4Json12MemAllocatorC2Ev(void* self) {
    (void)self;
    return 0;
}

int APS5_VABI _ZN3sce4Json12MemAllocatorD2Ev(void* self) {
    (void)self;
    return 0;
}

}
