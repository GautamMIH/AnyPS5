#include <cstdint>
#include <cstddef>
#include <cstdio>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

// The error dialog uses the common dialog status values and error codes (shadPS4 commondialog.h).
constexpr int COMMON_DIALOG_OK = 0;
constexpr int COMMON_DIALOG_ERROR_NOT_INITIALIZED = static_cast<int>(0x80B80003);
constexpr int COMMON_DIALOG_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80B80004);
constexpr int COMMON_DIALOG_ERROR_INVALID_STATE = static_cast<int>(0x80B80006);
constexpr int COMMON_DIALOG_ERROR_NOT_RUNNING = static_cast<int>(0x80B8000B);
constexpr int COMMON_DIALOG_ERROR_ARG_NULL = static_cast<int>(0x80B8000D);

constexpr int STATUS_NONE = 0;
constexpr int STATUS_INITIALIZED = 1;
constexpr int STATUS_RUNNING = 2;
constexpr int STATUS_FINISHED = 3;

struct ErrorDialogParam {
    std::int32_t size;
    std::int32_t errorCode;
    std::int32_t userId;
    std::int32_t reserved;
};

int g_status = STATUS_NONE;

}

extern "C" {

int APS5_VABI sceErrorDialogClose(void) {
 if (g_status != STATUS_RUNNING) return COMMON_DIALOG_ERROR_NOT_RUNNING;
 g_status = STATUS_FINISHED;
 return COMMON_DIALOG_OK;
}

int APS5_VABI sceErrorDialogGetStatus(void) {
 return g_status;
}

int APS5_VABI sceErrorDialogInitialize(void) {
 if (g_status != STATUS_NONE) return COMMON_DIALOG_ERROR_ALREADY_INITIALIZED;
 g_status = STATUS_INITIALIZED;
 return COMMON_DIALOG_OK;
}

// shadPS4 shows the code until the user dismisses it. There is no system UI here, so the code
// is logged and the dialog finishes at once, as if dismissed.
int APS5_VABI sceErrorDialogOpen(const void* param) {
 if (g_status != STATUS_INITIALIZED && g_status != STATUS_FINISHED) return COMMON_DIALOG_ERROR_INVALID_STATE;
 if (param == nullptr) return COMMON_DIALOG_ERROR_ARG_NULL;
 const auto* dialog = static_cast<const ErrorDialogParam*>(param);
 std::fprintf(stderr, "[AnyPS5] error dialog: code 0x%08x for user %d\n", static_cast<unsigned>(dialog->errorCode), dialog->userId);
 g_status = STATUS_FINISHED;
 return COMMON_DIALOG_OK;
}

int APS5_VABI sceErrorDialogTerminate(void) {
 if (g_status == STATUS_RUNNING) sceErrorDialogClose();
 if (g_status == STATUS_NONE) return COMMON_DIALOG_ERROR_NOT_INITIALIZED;
 g_status = STATUS_NONE;
 return COMMON_DIALOG_OK;
}

int APS5_VABI sceErrorDialogUpdateStatus(void) {
 return g_status;
}

}
