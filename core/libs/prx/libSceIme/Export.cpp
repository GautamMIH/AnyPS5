#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <cstring>
#include <mutex>
#include <set>

// Hardware keyboards: none are attached, so keyboard sessions open normally, report no devices and
// never produce events.
namespace {

constexpr int IME_OK = 0;
constexpr int IME_ERROR_BUSY = static_cast<int>(0x80BC0001);
constexpr int IME_ERROR_NOT_OPENED = static_cast<int>(0x80BC0002);
constexpr int IME_ERROR_INVALID_PARAM = static_cast<int>(0x80BC0030);
constexpr int IME_ERROR_INVALID_ADDRESS = static_cast<int>(0x80BC0031);

std::mutex keyboardMutex;
std::set<std::int32_t> keyboardUsers;

}

extern "C" {

int APS5_VABI sceImeClose_nid_postfix(void) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceImeGetPanelSize(const Param* param, uint32_t* width, uint32_t* height) {
 (void)param;
 (void)width;
 (void)height;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceImeKeyboardClose(int32_t user_id) {
 std::lock_guard lock(keyboardMutex);
 return keyboardUsers.erase(user_id) != 0 ? IME_OK : IME_ERROR_NOT_OPENED;
}

int APS5_VABI sceImeKeyboardGetInfo(uint32_t resource_id, KeyboardInfo* info) {
 (void)resource_id;
 if (info == nullptr) return IME_ERROR_INVALID_ADDRESS;
 std::memset(info, 0, sizeof(*info));
 return IME_OK;
}

int APS5_VABI sceImeKeyboardGetResourceId(int32_t user_id, KeyboardResourceIdArray* resource_ids) {
 if (resource_ids == nullptr) return IME_ERROR_INVALID_ADDRESS;
 std::lock_guard lock(keyboardMutex);
 if (keyboardUsers.count(user_id) == 0) return IME_ERROR_NOT_OPENED;
 std::memset(resource_ids, 0, sizeof(*resource_ids));
 resource_ids->user_id = user_id;
 return IME_OK;
}

int APS5_VABI sceImeKeyboardOpen(int32_t user_id, const KeyboardParam* param) {
 if (param == nullptr) return IME_ERROR_INVALID_ADDRESS;
 if (param->handler == nullptr) return IME_ERROR_INVALID_PARAM;
 std::lock_guard lock(keyboardMutex);
 return keyboardUsers.insert(user_id).second ? IME_OK : IME_ERROR_BUSY;
}

int APS5_VABI sceImeKeyboardSetMode(int32_t user_id, uint32_t mode) {
 (void)mode;
 std::lock_guard lock(keyboardMutex);
 return keyboardUsers.count(user_id) != 0 ? IME_OK : IME_ERROR_NOT_OPENED;
}

int APS5_VABI sceImeOpen_nid_postfix(const Param* param, const ExtendedParam* extended) {
 (void)param;
 (void)extended;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

void APS5_VABI sceImeParamInit(Param* param) {
 (void)param;
 NotImplemented_nid_no_patch(__func__);
}

int APS5_VABI sceImeSetCaret(const Caret* caret) {
 (void)caret;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceImeSetText(const char16_t* text, uint32_t length) {
 (void)text;
 (void)length;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceImeSetTextGeometry(TextAreaMode mode, const TextGeometry* geometry) {
 (void)mode;
 (void)geometry;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceImeUpdate(EventHandler handler) {
 if (handler == nullptr) return IME_ERROR_INVALID_ADDRESS;
 std::lock_guard lock(keyboardMutex);
 return keyboardUsers.empty() ? IME_ERROR_NOT_OPENED : IME_OK;
}

}
