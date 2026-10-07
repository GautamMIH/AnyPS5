#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

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

// The host keyboard drives the virtual pad (docs/INPUT_MAPPING.md), so no USB keyboard is attached:
// opening the keyboard succeeds, as on a console with none plugged in, and it never reports events.
constexpr int ErrorNotOpened = static_cast<int>(0x80bc0002u);
constexpr int ErrorConnectionFailed = static_cast<int>(0x80bc0004u);
constexpr int ErrorInvalidUserId = static_cast<int>(0x80bc0010u);
constexpr int ErrorInvalidType = static_cast<int>(0x80bc0011u);
constexpr int ErrorInvalidOption = static_cast<int>(0x80bc0015u);
constexpr int ErrorInvalidAddress = static_cast<int>(0x80bc0031u);
constexpr int ErrorNoResourceId = static_cast<int>(0x80bc0023u);
constexpr int ErrorInvalidMode = static_cast<int>(0x80bc0024u);
constexpr uint32_t ValidKeyboardModes = 0x0000007f;
constexpr uint32_t TypeNumber = 4;
constexpr uint32_t ValidOptions = 0x00007bff;
constexpr uint32_t OptionUseOver2K = 0x00004000;
constexpr int32_t UserIdInvalid = -1;


}

extern "C" {

int APS5_VABI sceImeClose_nid_postfix(void) {
 return ErrorNotOpened;
}

int APS5_VABI sceImeGetPanelSize(const Param* param, uint32_t* width, uint32_t* height) {
 if (!param || !width || !height) return ErrorInvalidAddress;
 if (param->type > TypeNumber) return ErrorInvalidType;
 if ((param->option & ~ValidOptions) != 0) return ErrorInvalidOption;
 const uint32_t scale = (param->option & OptionUseOver2K) != 0 ? 2 : 1;
 *width = (param->type == TypeNumber ? 370u : 793u) * scale;
 *height = (param->type == TypeNumber ? 402u : 408u) * scale;
 return 0;
}

int APS5_VABI sceImeKeyboardClose(int32_t user_id) {
 std::lock_guard lock(keyboardMutex);
 return keyboardUsers.erase(user_id) != 0 ? IME_OK : IME_ERROR_NOT_OPENED;
}

int APS5_VABI sceImeKeyboardGetInfo(uint32_t resource_id, KeyboardInfo* info) {
 (void)resource_id;
 if (!info) return ErrorInvalidAddress;
 std::lock_guard lock(keyboardMutex);
 return keyboardUsers.empty() ? ErrorNotOpened : ErrorNoResourceId;
}

int APS5_VABI sceImeKeyboardGetResourceId(int32_t user_id, KeyboardResourceIdArray* resource_ids) {
 if (resource_ids == nullptr) return IME_ERROR_INVALID_ADDRESS;
 if (user_id == UserIdInvalid) return ErrorInvalidUserId;
 // The array is cleared and given the user. No USB keyboard is reported through the IME, so an
 // open session fails to connect (upstream 54f0e03c, as shadPS4); an unopened one is NOT_OPENED.
 *resource_ids = {};
 resource_ids->user_id = user_id;
 std::lock_guard lock(keyboardMutex);
 return keyboardUsers.contains(user_id) ? ErrorConnectionFailed : IME_ERROR_NOT_OPENED;
}

int APS5_VABI sceImeKeyboardOpen(int32_t user_id, const KeyboardParam* param) {
 if (param == nullptr) return IME_ERROR_INVALID_ADDRESS;
 if (param->handler == nullptr) return IME_ERROR_INVALID_PARAM;
 std::lock_guard lock(keyboardMutex);
 return keyboardUsers.insert(user_id).second ? IME_OK : IME_ERROR_BUSY;
}

int APS5_VABI sceImeKeyboardSetMode(int32_t user_id, uint32_t mode) {
 if (user_id == UserIdInvalid) return ErrorInvalidUserId;
 std::lock_guard lock(keyboardMutex);
 if (!keyboardUsers.contains(user_id)) return ErrorNotOpened;
 return (mode & ~ValidKeyboardModes) == 0 ? 0 : ErrorInvalidMode;
}

int APS5_VABI sceImeOpen_nid_postfix(const Param* param, const ExtendedParam* extended) {
 (void)param;
 (void)extended;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

void APS5_VABI sceImeParamInit(Param* param) {
 if (!param) return;
 std::memset(param, 0, sizeof(*param));
 param->user_id = -1;
}

int APS5_VABI sceImeSetCaret(const Caret* caret) {
 (void)caret;
 return ErrorNotOpened;
}

int APS5_VABI sceImeSetText(const char16_t* text, uint32_t length) {
 (void)text;
 (void)length;
 return ErrorNotOpened;
}

int APS5_VABI sceImeSetTextGeometry(TextAreaMode mode, const TextGeometry* geometry) {
 (void)mode;
 (void)geometry;
 return ErrorNotOpened;
}

int APS5_VABI sceImeUpdate(EventHandler handler) {
 if (handler == nullptr) return IME_ERROR_INVALID_ADDRESS;
 std::lock_guard lock(keyboardMutex);
 return keyboardUsers.empty() ? IME_ERROR_NOT_OPENED : IME_OK;
}

}
