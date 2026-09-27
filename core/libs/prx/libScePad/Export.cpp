#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <set>
#include <thread>
#include <tuple>

#include "SceTypes.hpp"
#include "prx//libc/include/General.hpp"
#include "prx/libScePad/include/Pad.hpp"
#include "prx/libScePad/include/PadState.hpp"


namespace {

// Ports opened with scePadOpen, as (user, type, index); all share the one emulated controller.
bool padInitialized = false;
std::set<std::tuple<int, int, int>> openedPorts;

}

extern "C" {

int APS5_VABI scePadClose_nid_postfix(int handle) {
 if (handle != PAD_HANDLE) {
  return PAD_ERROR_INVALID_HANDLE;
 }
 return PAD_OK;
}

int APS5_VABI scePadDeviceClassGetExtendedInformation(int handle, PadDeviceClassExtendedInformation* info) {
 (void)handle;
 if (!info) return PAD_ERROR_INVALID_ARG;
 *info = PadDeviceClassExtendedInformation{};
 info->deviceClass = PAD_DEVICE_CLASS_STANDARD;
 return 0;
}

// The emulated controller is a standard pad, so samples carry no device-class payload
// (special controllers such as wheels would report theirs here).
int APS5_VABI scePadDeviceClassParseData(int handle, const PadData* data, PadDeviceClassData* class_data) {
 if (handle != PAD_HANDLE) {
  return PAD_ERROR_INVALID_HANDLE;
 }
 if (data == nullptr || class_data == nullptr) {
  return PAD_ERROR_INVALID_ARG;
 }
 *class_data = PadDeviceClassData{};
 class_data->deviceClass = PAD_DEVICE_CLASS_STANDARD;
 class_data->dataValid = data->connected;
 return PAD_OK;
}

int APS5_VABI scePadGetControllerInformation(int handle, PadControllerInformation* info) {
 if (handle != PAD_HANDLE) {
  return PAD_ERROR_INVALID_HANDLE;
 }
 if (info == nullptr) {
  return PAD_ERROR_INVALID_ARG;
 }
 std::memset(info, 0, sizeof(*info));
 info->touchPadInfo.pixelDensity = 44.86f;
 info->touchPadInfo.resolution.x = 1920;
 info->touchPadInfo.resolution.y = 943;
 info->stickInfo.deadZoneLeft = 2;
 info->stickInfo.deadZoneRight = 2;
 info->connectionType = PAD_CONNECTION_TYPE_LOCAL;
 info->connectedCount = 1;
 info->connected = true;
 info->deviceClass = PAD_DEVICE_CLASS_STANDARD;
 return PAD_OK;
}

// As in shadPS4: the handle of a port opened with the same user, type and index.
int APS5_VABI scePadGetHandle(int user_id, int type, int index) {
 if (!padInitialized) return PAD_ERROR_NOT_INITIALIZED;
 return openedPorts.contains({user_id, type, index}) ? PAD_HANDLE : PAD_ERROR_NO_HANDLE;
}

// The emulated pad has no adaptive triggers, so no effect is ever running (KytyPS5).
int APS5_VABI scePadGetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (info == nullptr) return PAD_ERROR_INVALID_ARG;
 *info = PadTriggerEffectStateInformation{};
 return PAD_OK;
}

int APS5_VABI scePadInit_nid_postfix(void) {
 Pad::Initialize();
 padInitialized = true;
 return PAD_OK;
}

int APS5_VABI scePadOpen_nid_postfix(int userId, int type, int index, const void* param) {
 (void)param;
 if (index != 0) {
  return PAD_ERROR_INVALID_ARG;
 }
 const bool personalPort = (type == PAD_PORT_TYPE_STANDARD || type == PAD_PORT_TYPE_SPECIAL);
 const bool systemRemote = (userId == PAD_USER_ID_SYSTEM && type == PAD_PORT_TYPE_REMOTE);
 if (!personalPort && !systemRemote) {
  return PAD_ERROR_INVALID_ARG;
 }
 openedPorts.insert({userId, type, index});
 return PAD_HANDLE;
}

// The pad layer keeps only the current state, so each read returns one sample: the latest.
int APS5_VABI scePadRead_nid_postfix(int handle, PadData* data, int num) {
 constexpr int kMaxSamples = 64;
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (data == nullptr || num < 1 || num > kMaxSamples) return PAD_ERROR_INVALID_ARG;
 data[0] = Pad::ReadState();
 return 1;
}

int APS5_VABI scePadReadState(int handle, PadData* data) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (data == nullptr) return PAD_ERROR_INVALID_ARG;

 *data = Pad::ReadState();

 return 0;
}

int APS5_VABI scePadResetLightBar(int handle) {
 (void)handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

// The emulated pad reports a fixed orientation, so there is no reference to reset.
int APS5_VABI scePadResetOrientation(int handle) {
 if (handle != PAD_HANDLE) {
  return PAD_ERROR_INVALID_HANDLE;
 }
 return PAD_OK;
}

int APS5_VABI scePadSetAngularVelocityDeadbandState(int handle, bool enable) {
 (void)handle;
 (void)enable;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadSetLightBar(int handle, const PadLightBarParam* param) {
 (void)handle;
 (void)param;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadSetMotionSensorState(int handle, bool enable) {
 (void)handle;
 (void)enable;
 // if (enable) {
 //  throw std::runtime_error("scePadSetMotionSensorState: motion sensor not supported");
 // }
 return PAD_OK;
}

int APS5_VABI scePadSetTiltCorrectionState(int handle, bool enabled) {
 (void)handle;
 (void)enabled;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

// Trigger effects and rumble are controller output the emulated pad does not render; requests are
// accepted like on a pad without those motors.
int APS5_VABI scePadSetTriggerEffect(int handle, const void* param) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (param == nullptr) return PAD_ERROR_INVALID_ARG;
 return PAD_OK;
}

int APS5_VABI scePadSetVibration(int handle, const PadVibrationParam* param) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (param == nullptr) return PAD_ERROR_INVALID_ARG;
 return PAD_OK;
}

// Selects compatible rumble or advanced haptics; the emulated pad has neither, as in KytyPS5.
int APS5_VABI scePadSetVibrationMode(int handle, int mode) {
 (void)mode;
 if (handle != PAD_HANDLE) {
  return PAD_ERROR_INVALID_HANDLE;
 }
 return PAD_OK;
}

int APS5_VABI scePadSetVibrationTriggerEffectWeakWhileEmbeddedMicInUse(bool enabled) {
 (void)enabled;
 return PAD_OK;
}

}
