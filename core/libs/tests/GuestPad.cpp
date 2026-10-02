#include "prx/libScePad/include/Pad.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdlib>

extern "C" {
int APS5_VABI scePadInit_nid_postfix(void);
int APS5_VABI scePadOpen_nid_postfix(int, int, int, const void*);
int APS5_VABI scePadClose_nid_postfix(int);
int APS5_VABI scePadGetHandle(int, int, int);
int APS5_VABI scePadSetVibrationTriggerEffectWeakWhileEmbeddedMicInUse(bool);
}

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    constexpr int user = 0x10000000;

    Require(scePadGetHandle(user, 0, 0) == PAD_ERROR_NOT_INITIALIZED);
    Require(scePadInit_nid_postfix() == 0);
    Require(scePadGetHandle(user, 0, 0) == PAD_ERROR_NO_HANDLE);
    Require(scePadOpen_nid_postfix(user, 1, 0, nullptr) == PAD_ERROR_INVALID_ARG);
    Require(scePadOpen_nid_postfix(user, 0, 1, nullptr) == PAD_ERROR_INVALID_ARG);
    Require(scePadGetHandle(user, 0, 0) == PAD_ERROR_NO_HANDLE);
    const int handle = scePadOpen_nid_postfix(user, 0, 0, nullptr);
    Require(handle > 0);
    Require(scePadGetHandle(user, 0, 0) == handle);
    // A handle belongs to the (user, type, index) port that was opened.
    Require(scePadGetHandle(user, 2, 0) == PAD_ERROR_NO_HANDLE);
    Require(scePadOpen_nid_postfix(0xff, 16, 0, nullptr) == handle);
    Require(scePadGetHandle(0xff, 16, 0) == handle);
    Require(scePadGetHandle(user, 16, 0) == PAD_ERROR_NO_HANDLE);
    Require(scePadGetHandle(user, 0, 1) == PAD_ERROR_NO_HANDLE);
    Require(scePadClose_nid_postfix(handle) == 0);
    Require(scePadGetHandle(user, 0, 0) == PAD_ERROR_NO_HANDLE);
    Require(scePadSetVibrationTriggerEffectWeakWhileEmbeddedMicInUse(true) == 0);
}
