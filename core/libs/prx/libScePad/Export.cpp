#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <thread>

#include "SceTypes.hpp"
#include "prx//libc/include/General.hpp"
#include "prx/libScePad/include/Pad.hpp"
#include "prx/libScePad/include/PadState.hpp"


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

int APS5_VABI scePadGetHandle(int user_id, int type, int index) {
 (void)user_id;
 (void)type;
 (void)index;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadGetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info) {
 (void)handle;
 (void)info;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadInit_nid_postfix(void) {
 Pad::Initialize();
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
 return PAD_HANDLE;
}

// The pad layer keeps only the current state, so each read returns one sample: the latest.
int APS5_VABI scePadRead_nid_postfix(int handle, PadData* data, int num) {
 constexpr int kPadErrorInvalidArgument = static_cast<int>(0x80920001);
 constexpr int kPadErrorInvalidHandle = static_cast<int>(0x80920003);
 constexpr int kMaxSamples = 64;
 if (handle != 1) return kPadErrorInvalidHandle;
 if (data == nullptr || num < 1 || num > kMaxSamples) return kPadErrorInvalidArgument;
 data[0] = Pad::ReadState();
 return 1;
}

int APS5_VABI scePadReadState(int handle, PadData* data) {
 if (handle != 1) APS5_INVALID_ARG_EX;
 if (data == nullptr) APS5_INVALID_ARG_EX;

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

int APS5_VABI scePadSetTriggerEffect(int handle, const void* param) {
 (void)handle;
 (void)param;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadSetVibration(int handle, const PadVibrationParam* param) {
 (void)handle;
 (void)param;
 NotImplemented_nid_no_patch(__func__);
 return 0;
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
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
