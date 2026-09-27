#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceUserService/UserService.hpp"
#include <mutex>
#include <vector>

static constexpr int NP_ERROR_INVALID_ARGUMENT = static_cast<int>(0x80550003);
static constexpr int NP_ERROR_SIGNED_OUT = static_cast<int>(0x80550006);
static constexpr uint32_t NP_STATE_SIGNED_OUT = 1;
static constexpr uint32_t NP_REACHABILITY_STATE_UNAVAILABLE = 0;
static constexpr int NP_ERROR_CALLBACK_ALREADY_REGISTERED = static_cast<int>(0x80550008);

namespace {

// The console is offline: each registered state callback learns once, from sceNpCheckCallback, that
// the user is signed out.
using NpStateCallbackA = void (APS5_VABI*)(int userId, uint32_t state, void* userdata);

struct StateCallback {
    NpStateCallbackA callback;
    void* userdata;
    bool pending;
};

std::mutex stateMutex;
std::vector<StateCallback> stateCallbacks;

}

extern "C" {

int APS5_VABI sceNpAbortRequest(int req_id) {
 (void)req_id;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpCheckCallback(void) {
    std::vector<StateCallback> pending;
    {
        std::lock_guard lock(stateMutex);
        for (auto& entry : stateCallbacks) {
            if (entry.callback != nullptr && entry.pending) {
                pending.push_back(entry);
                entry.pending = false;
            }
        }
    }
    for (const auto& entry : pending) entry.callback(USER_SERVICE_INITIAL_USER_ID, NP_STATE_SIGNED_OUT, entry.userdata);
    return 0;
}

int APS5_VABI sceNpRegisterStateCallbackA(NpStateCallbackA callback, void* userdata) {
    if (callback == nullptr) return NP_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(stateMutex);
    for (const auto& entry : stateCallbacks)
        if (entry.callback == callback) return NP_ERROR_CALLBACK_ALREADY_REGISTERED;
    stateCallbacks.push_back({callback, userdata, true});
    return static_cast<int>(stateCallbacks.size());
}

int APS5_VABI sceNpUnregisterStateCallbackA(int callback_id) {
    std::lock_guard lock(stateMutex);
    if (callback_id <= 0 || static_cast<std::size_t>(callback_id) > stateCallbacks.size() || stateCallbacks[callback_id - 1].callback == nullptr)
        return NP_ERROR_INVALID_ARGUMENT;
    stateCallbacks[callback_id - 1].callback = nullptr;
    return 0;
}

int APS5_VABI sceNpCheckNpAvailability(int req_id, const char* user, void* result) {
 (void)req_id;
 (void)user;
 (void)result;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpCheckNpReachability(int req_id, int user_id) {
 (void)req_id;
 (void)user_id;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpCheckPremium(int req_id, const NpCheckPremiumParameter* param, NpCheckPremiumResult* result) {
 (void)req_id;
 (void)param;
 (void)result;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpCreateAsyncRequest(const NpCreateAsyncRequestParameter* param) {
 (void)param;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpCreateRequest(void) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpDeleteRequest(int req_id) {
 (void)req_id;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpGetAccountAge(int req_id, int user_id, uint8_t* age) {
 (void)req_id;
 (void)user_id;
 (void)age;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpGetAccountCountryA(int user_id, void* country_code) {
 (void)user_id;
 (void)country_code;
 return NP_ERROR_SIGNED_OUT;
}

int APS5_VABI sceNpGetAccountIdA(int user_id, uint64_t* account_id) {
 (void)user_id;
 if (!account_id) return NP_ERROR_INVALID_ARGUMENT;
 *account_id = 0;
 return NP_ERROR_SIGNED_OUT;
}

// Offline, users have no PSN identity (shadPS4 sceNpGetOnlineId without shadnet).
int APS5_VABI sceNpGetNpId(int user_id, NpId* np_id) {
 if (user_id == USER_SERVICE_USER_ID_INVALID || !np_id) return NP_ERROR_INVALID_ARGUMENT;
 return NP_ERROR_SIGNED_OUT;
}

int APS5_VABI sceNpGetNpReachabilityState(int user_id, uint32_t* state) {
 (void)user_id;
 if (!state) return NP_ERROR_INVALID_ARGUMENT;
 *state = NP_REACHABILITY_STATE_UNAVAILABLE;
 return 0;
}

int APS5_VABI sceNpGetOnlineId(int user_id, NpOnlineId* online_id) {
 if (user_id == USER_SERVICE_USER_ID_INVALID || !online_id) return NP_ERROR_INVALID_ARGUMENT;
 return NP_ERROR_SIGNED_OUT;
}

int APS5_VABI sceNpGetState(int user_id, uint32_t* state) {
 (void)user_id;
 if (!state) return NP_ERROR_INVALID_ARGUMENT;
 *state = NP_STATE_SIGNED_OUT;
 return 0;
}

int APS5_VABI sceNpHasSignedUp(int user_id, bool* has_signed_up) {
 (void)user_id;
 (void)has_signed_up;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpPollAsync(int req_id, int* result) {
 (void)req_id;
 (void)result;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

void APS5_VABI sceNpRegisterGamePresenceCallback(void* callback, void* userdata) {
 (void)callback;
 (void)userdata;
 NotImplemented_nid_no_patch(__func__);
}

int APS5_VABI sceNpRegisterNpReachabilityStateCallback(void* callback, void* userdata) {
 (void)callback;
 (void)userdata;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpRegisterPlusEventCallback(void* callback, void* userdata) {
 (void)callback;
 (void)userdata;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpRegisterPremiumEventCallback(void* callback, void* userdata) {
 (void)callback;
 (void)userdata;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpRegisterStateCallback(void* callback, void* userdata) {
 (void)callback;
 (void)userdata;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpSetContentRestriction(const NpContentRestriction* restriction) {
 (void)restriction;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpSetNpTitleId(const NpTitleId* title_id, const NpTitleSecret* title_secret) {
 (void)title_id;
 (void)title_secret;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNpUnregisterStateCallback(void) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
