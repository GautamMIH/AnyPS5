#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdint>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceUserService/UserService.hpp"

extern "C" {

int APS5_VABI sceUserServiceGetAccessibilityChatTranscription(int user_id, int32_t* chat_transcription) {
 if (user_id != USER_SERVICE_INITIAL_USER_ID || !chat_transcription) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 *chat_transcription = 0;
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceGetAccessibilityPressAndHoldDelay(int user_id, int32_t* press_and_hold_delay) {
 if (user_id != USER_SERVICE_INITIAL_USER_ID || !press_and_hold_delay) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 *press_and_hold_delay = 0;
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceGetAccessibilityTriggerEffect(int user_id, int32_t* trigger_effect) {
 if (user_id != USER_SERVICE_INITIAL_USER_ID || !trigger_effect) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 *trigger_effect = 0;
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceGetAccessibilityVibration(int user_id, int32_t* vibration) {
 if (user_id != USER_SERVICE_INITIAL_USER_ID || !vibration) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 *vibration = 1;
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceGetAccessibilityZoomEnabled(int user_id, int32_t* zoom_enabled) {
 if (user_id != USER_SERVICE_INITIAL_USER_ID || !zoom_enabled) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 *zoom_enabled = 0;
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceGetAgeLevel(int user_id, uint32_t* age_level) {
 if (user_id != USER_SERVICE_INITIAL_USER_ID || !age_level) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 *age_level = USER_SERVICE_ADULT_AGE_LEVEL;
 return USER_SERVICE_OK;
}

// The initial user is logged in at boot, which queues one login event; games that track players
// through events rather than the login list depend on it (shadPS4 UserManager::LoginUser).
int APS5_VABI sceUserServiceGetEvent(SceUserServiceEvent* event) {
 if (!event) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 static std::atomic<bool> loginDelivered{false};
 if (loginDelivered.exchange(true)) return USER_SERVICE_ERROR_NO_EVENT;
 event->event_type = USER_SERVICE_EVENT_LOGIN;
 event->user_id = USER_SERVICE_INITIAL_USER_ID;
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceGetGamePresets(int user_id, UserServiceGamePresets* presets) {
 if (user_id != USER_SERVICE_INITIAL_USER_ID || !presets || presets->this_size < sizeof(UserServiceGamePresets)) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 const std::size_t size = presets->this_size;
 *presets = UserServiceGamePresets{};
 presets->this_size = size;
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceGetInitialUser(int* user_id) {
 if (user_id == nullptr) {
  return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 }
 *user_id = USER_SERVICE_INITIAL_USER_ID;
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceGetLoginUserIdList(UserServiceLoginUserIdList* user_id_list) {
 if (user_id_list == nullptr) {
  return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 }
 user_id_list->user_id[0] = USER_SERVICE_INITIAL_USER_ID;
 user_id_list->user_id[1] = USER_SERVICE_USER_ID_INVALID;
 user_id_list->user_id[2] = USER_SERVICE_USER_ID_INVALID;
 user_id_list->user_id[3] = USER_SERVICE_USER_ID_INVALID;
 return USER_SERVICE_OK;
}

// The buffer must hold the longest possible user name and its terminator.
int APS5_VABI sceUserServiceGetUserName(int user_id, char* name, size_t size) {
 if (user_id != USER_SERVICE_INITIAL_USER_ID || !name) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 if (size < USER_SERVICE_MAX_USER_NAME_LENGTH + 1) return USER_SERVICE_ERROR_BUFFER_TOO_SHORT;
 const char* host = std::getenv(USER_SERVICE_NAME_VARIABLE);
 const std::string userName = host != nullptr && host[0] != '\0' ? host : USER_SERVICE_DEFAULT_NAME;
 const std::size_t count = std::min(USER_SERVICE_MAX_USER_NAME_LENGTH, userName.size());
 std::memcpy(name, userName.data(), count);
 name[count] = '\0';
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceGetUserNumber(int user_id, int32_t* number) {
 if (user_id != USER_SERVICE_INITIAL_USER_ID || !number) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 *number = 1;
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceInitialize(const void* params) {
 (void)params;
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceInitialize2(void) {
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceTerminate(void) {
 return USER_SERVICE_OK;
}

int APS5_VABI sceUserServiceGetAccessibilityZoomFollowFocus(int user_id, int32_t* zoom_follow_focus) {
 if (user_id != USER_SERVICE_INITIAL_USER_ID || !zoom_follow_focus) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 *zoom_follow_focus = 0;
 return USER_SERVICE_OK;
}

// No PSN account exists, so the platform privacy setting reports the feature as not permitted.
int APS5_VABI sceUserServiceGetPlatformPrivacyWs1(int32_t user_id, int32_t* value) {
 (void)user_id;
 if (!value) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
 *value = 0;
 return USER_SERVICE_OK;
}

}
