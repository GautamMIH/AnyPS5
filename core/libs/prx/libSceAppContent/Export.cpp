#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/AppMetadata/include/AppMetadata.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

constexpr int APP_CONTENT_ERROR_PARAMETER = static_cast<int>(0x80D90002);
constexpr int APP_CONTENT_ERROR_BUSY = static_cast<int>(0x80D90003);
constexpr uint32_t APP_CONTENT_APPPARAM_ID_SKU_FLAG = 0;
constexpr uint32_t APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_1 = 1;
constexpr uint32_t APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_4 = 4;
constexpr int32_t APP_CONTENT_APPPARAM_SKU_FLAG_FULL = 3;
constexpr char kParamJsonPath[] = "/app0/sce_sys/param.json";
constexpr char kTemporaryMountPoint[] = "/temp0";
constexpr char kDownloadMountPoint[] = "/download0";
constexpr uint32_t kTemporaryDataOptionFormat = 1;

bool readUserDefinedParam(uint32_t index, int32_t& value) {
    std::ifstream stream(ResolvePath_nid_no_patch(kParamJsonPath));
    if (!stream) return false;
    std::stringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();
    const std::string key = "\"userDefinedParam" + std::to_string(index) + "\"";
    const auto position = text.find(key);
    if (position == std::string::npos) return false;
    const auto colon = text.find(':', position + key.size());
    if (colon == std::string::npos) return false;
    value = static_cast<int32_t>(std::strtol(text.c_str() + colon + 1, nullptr, 10));
    return true;
}

int availableKilobytes(const char* mountPoint, size_t* available) {
    if (!available) return APP_CONTENT_ERROR_PARAMETER;
    std::error_code error;
    const auto path = ResolvePath_nid_no_patch(mountPoint);
    std::filesystem::create_directories(path, error);
    const auto space = std::filesystem::space(path, error);
    if (error) return APP_CONTENT_ERROR_BUSY;
    *available = static_cast<size_t>(space.available / 1024);
    return 0;
}

bool isMountPoint(const AppContentMountPoint* mountPoint, const char* expected) {
    return mountPoint && std::strncmp(mountPoint->data, expected, sizeof(mountPoint->data)) == 0;
}

}

static constexpr char TEMPORARY_MOUNT_POINT[] = "/temp0";
static constexpr char DOWNLOAD_MOUNT_POINT[] = "/download0";
static constexpr uint32_t TEMPORARY_DATA_OPTION_FORMAT = 1;

// Temporary data lives in <root>/temp0 on the host and survives until the title formats it.
static std::filesystem::path TemporaryDirectory(const AppContentMountPoint* mount_point) {
    if (!mount_point || std::strncmp(mount_point->data, TEMPORARY_MOUNT_POINT, sizeof(mount_point->data)) != 0) APS5_INVALID_ARG_EX;
    return ResolvePath_nid_no_patch(TEMPORARY_MOUNT_POINT);
}

static void ClearDirectory(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) std::filesystem::remove_all(entry.path());
}

extern "C" {

int APS5_VABI sceAppContentAddcontMount(uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label, AppContentMountPoint* mount_point) {
 (void)service_label;
 (void)entitlement_label;
 (void)mount_point;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAppContentAddcontUnmount(const AppContentMountPoint* mount_point) {
 (void)mount_point;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceAppContentAppParamGetInt(uint32_t param_id, int32_t* value) {
 if (!value) return APP_CONTENT_ERROR_PARAMETER;
 if (param_id == APP_CONTENT_APPPARAM_ID_SKU_FLAG) {
  *value = APP_CONTENT_APPPARAM_SKU_FLAG_FULL;
  return 0;
 }
 if (param_id < APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_1 || param_id > APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_4) return APP_CONTENT_ERROR_PARAMETER;
 int32_t parsed = 0;
 *value = readUserDefinedParam(param_id, parsed) ? parsed : 0;
 return 0;
}

int APS5_VABI sceAppContentDownloadDataGetAvailableSpaceKb(const AppContentMountPoint* mount_point, size_t* available_space_kb) {
 if (!isMountPoint(mount_point, kDownloadMountPoint)) return APP_CONTENT_ERROR_PARAMETER;
 if (const int result = availableKilobytes(kDownloadMountPoint, available_space_kb); result != 0) return result;
 // The title's declared download data size (param.json) caps the space, less what is already stored.
 const std::uint64_t quotaKb = GetAppDownloadDataSizeMiB_nid_postfix() * 1024u;
 if (quotaKb != 0) {
  std::uint64_t usedKb = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(ResolvePath_nid_no_patch(kDownloadMountPoint), error)) {
   if (entry.is_regular_file(error)) usedKb += (entry.file_size(error) + 1023u) / 1024u;
  }
  *available_space_kb = static_cast<size_t>(std::min<std::uint64_t>(quotaKb - std::min(quotaKb, usedKb), *available_space_kb));
 }
 return 0;
}

int APS5_VABI sceAppContentInitialize(const AppContentInitParam* init_param, AppContentBootParam* boot_param) {
 (void)init_param;
 if (!boot_param) return APP_CONTENT_ERROR_PARAMETER;
 *boot_param = AppContentBootParam{};
 return 0;
}

int APS5_VABI sceAppContentTemporaryDataFormat(const AppContentMountPoint* mount_point) {
 if (!isMountPoint(mount_point, kTemporaryMountPoint)) return APP_CONTENT_ERROR_PARAMETER;
 const auto path = ResolvePath_nid_no_patch(kTemporaryMountPoint);
 std::error_code error;
 std::filesystem::remove_all(path, error);
 std::filesystem::create_directories(path, error);
 return error ? APP_CONTENT_ERROR_BUSY : 0;
}

int APS5_VABI sceAppContentTemporaryDataGetAvailableSpaceKb(const AppContentMountPoint* mount_point, size_t* available_space_kb) {
 if (!isMountPoint(mount_point, kTemporaryMountPoint)) return APP_CONTENT_ERROR_PARAMETER;
 return availableKilobytes(kTemporaryMountPoint, available_space_kb);
}

int APS5_VABI sceAppContentTemporaryDataMount2(uint32_t option, AppContentMountPoint* mount_point) {
 if (!mount_point) return APP_CONTENT_ERROR_PARAMETER;
 if (option > kTemporaryDataOptionFormat) throw std::invalid_argument(std::string(__func__) + ": unknown option " + std::to_string(option));
 std::error_code error;
 const auto path = ResolvePath_nid_no_patch(kTemporaryMountPoint);
 if (option == kTemporaryDataOptionFormat) std::filesystem::remove_all(path, error);
 std::filesystem::create_directories(path, error);
 if (error) return APP_CONTENT_ERROR_BUSY;
 *mount_point = AppContentMountPoint{};
 std::strncpy(mount_point->data, kTemporaryMountPoint, sizeof(mount_point->data) - 1);
 return 0;
}

}
