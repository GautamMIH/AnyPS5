#include <chrono>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <set>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int MOUSE_ERROR_INVALID_ARG = static_cast<int>(0x80DF0001);
constexpr int MOUSE_ERROR_INVALID_HANDLE = static_cast<int>(0x80DF0003);
constexpr int MOUSE_ERROR_NOT_INITIALIZED = static_cast<int>(0x80DF0002);

std::mutex mouseMutex;
std::set<int32_t> openHandles;
int32_t nextHandle = 1;
bool initialized = false;

}

extern "C" {

int APS5_VABI sceMouseInit(void) {
 const std::lock_guard lock(mouseMutex);
 initialized = true;
 return 0;
}

int APS5_VABI sceMouseOpen(int user_id, int32_t type, int32_t index, const void* param) {
 (void)user_id;
 (void)type;
 (void)param;
 const std::lock_guard lock(mouseMutex);
 if (!initialized) return MOUSE_ERROR_NOT_INITIALIZED;
 if (index < 0) return MOUSE_ERROR_INVALID_ARG;
 const int32_t handle = nextHandle++;
 openHandles.insert(handle);
 return handle;
}

int APS5_VABI sceMouseClose(int32_t handle) {
 const std::lock_guard lock(mouseMutex);
 return openHandles.erase(handle) != 0 ? 0 : MOUSE_ERROR_INVALID_HANDLE;
}

int APS5_VABI sceMouseRead(int32_t handle, MouseData* data, int32_t num) {
 if (!data || num < 1) return MOUSE_ERROR_INVALID_ARG;
 {
  const std::lock_guard lock(mouseMutex);
  if (openHandles.count(handle) == 0) return MOUSE_ERROR_INVALID_HANDLE;
 }
 data[0] = MouseData{};
 data[0].timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
 data[0].connected = false;
 return 1;
}

}
