#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "SaveData.hpp"

static constexpr char SAVE_DIR[] = "_sd";

static std::atomic<std::int32_t> g_transaction_counter{1};
static bool g_initialized = false;

static std::string save_root() {
    return std::string(SAVE_DIR);
}

static std::filesystem::path save_data_memory_path(int user_id, std::uint32_t slot_id) {
    return std::filesystem::path(save_root()) / "savedatamemory" / (std::to_string(user_id) + "_" + std::to_string(slot_id) + ".bin");
}

static bool dir_name_match(const char* str, const char* pattern) {
    if (pattern == nullptr || pattern[0] == '\0') {
        return true;
    }
    while (*str != '\0' && *pattern != '\0') {
        if (*pattern == '%') {
            for (const char* s = str;; s++) {
                if (dir_name_match(s, pattern + 1)) {
                    return true;
                }
                if (*s == '\0') {
                    break;
                }
            }
            return false;
        }
        if (*pattern == '_') {
            str++;
            pattern++;
            continue;
        }
        if (*pattern != *str) {
            return false;
        }
        str++;
        pattern++;
    }
    return *str == '\0' && *pattern == '\0';
}

extern "C" {

int APS5_VABI sceSaveDataBackup(const SaveDataBackup* backup) {
    (void)backup;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSaveDataCommit(const SaveDataCommitParam* param) {
    (void)param;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataCreateTransactionResource(uint32_t size) {
    (void)size;
    return g_transaction_counter.fetch_add(1);
}

int APS5_VABI sceSaveDataDelete(const SaveDataDelete* del) {
    if (del == nullptr || del->dir_name == nullptr) {
        throw std::runtime_error("sceSaveDataDelete: null argument");
    }
    const std::string path = save_root() + "/" + std::string(del->dir_name->data);
    if (std::filesystem::is_directory(path)) {
        std::filesystem::remove_all(path);
    }
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataDeleteTransactionResource(int32_t resource) {
    (void)resource;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataDirNameSearch(const SaveDataDirNameSearchCond* cond, SaveDataDirNameSearchResult* result) {
    if (cond == nullptr || result == nullptr) {
        throw std::runtime_error("sceSaveDataDirNameSearch: null argument");
    }
    result->hit_num = 0;
    result->set_num = 0;
    const char* pattern = (cond->dir_name != nullptr) ? cond->dir_name->data : nullptr;
    const std::string root = save_root();
    if (!std::filesystem::is_directory(root)) {
        return SAVE_DATA_OK;
    }
    std::uint32_t hit = 0;
    std::uint32_t set = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (!dir_name_match(name.c_str(), pattern)) {
            continue;
        }
        hit++;
        if (result->dir_names != nullptr && set < result->dir_names_num) {
            std::snprintf(result->dir_names[set].data, sizeof(result->dir_names[set].data), "%s", name.c_str());
            if (result->params != nullptr) {
                std::memset(&result->params[set], 0, sizeof(SaveDataParam));
            }
            set++;
        }
    }
    result->hit_num = hit;
    result->set_num = set;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataGetEventResult(const void* event_param, SaveDataEvent* event) {
    (void)event_param;
    (void)event;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSaveDataGetMountInfo(const SaveDataMountPoint* mount_point, SaveDataMountInfo* info) {
    if (mount_point == nullptr || info == nullptr) {
        throw std::runtime_error("sceSaveDataGetMountInfo: null argument");
    }
    if (find_slot_by_mount_point(mount_point->data) == -1) {
        return SAVE_DATA_ERROR_NOT_MOUNTED;
    }
    std::memset(info, 0, sizeof(*info));
    info->blocks = SAVE_DATA_BLOCKS_MAX;
    info->free_blocks = SAVE_DATA_BLOCKS_MAX;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataGetParam(const SaveDataMountPoint* mount_point, uint32_t param_type, void* param_buf, size_t param_buf_size, size_t* got_size) {
    (void)param_type;
    if (mount_point == nullptr || param_buf == nullptr) {
        throw std::runtime_error("sceSaveDataGetParam: null argument");
    }
    if (find_slot_by_mount_point(mount_point->data) == -1) {
        return SAVE_DATA_ERROR_NOT_MOUNTED;
    }
    std::memset(param_buf, 0, param_buf_size);
    if (got_size != nullptr) {
        *got_size = param_buf_size;
    }
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataGetSaveDataMemory2(SaveDataMemoryGet2* get_param) {
    if (get_param == nullptr) return SAVE_DATA_ERROR_PARAMETER;
    const auto path = save_data_memory_path(get_param->user_id, get_param->slot_id);
    if (!std::filesystem::exists(path)) return SAVE_DATA_ERROR_NOT_FOUND;
    if (get_param->data != nullptr) {
        std::ifstream stream(path, std::ios::binary);
        const auto& data = *get_param->data;
        if (data.buf == nullptr || data.offset + data.buf_size > std::filesystem::file_size(path)) return SAVE_DATA_ERROR_PARAMETER;
        stream.seekg(static_cast<std::streamoff>(data.offset));
        stream.read(static_cast<char*>(data.buf), static_cast<std::streamsize>(data.buf_size));
        if (!stream) return SAVE_DATA_ERROR_NOT_FOUND;
    }
    if (get_param->param != nullptr) std::memset(get_param->param, 0, sizeof(*get_param->param));
    if (get_param->icon != nullptr) get_param->icon->data_size = 0;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataInitialize3(const void* init) {
    (void)init;
    // NotImplemented_nid_no_patch(__func__);
    if (g_initialized) {
        return SAVE_DATA_ERROR_ALREADY_INITIALIZED;
    }
    g_initialized = true;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataLoadIcon(const SaveDataMountPoint* mount_point, SaveDataIcon* icon) {
    (void)icon;
    if (mount_point == nullptr) {
        throw std::runtime_error("sceSaveDataLoadIcon: null mount_point");
    }
    if (find_slot_by_mount_point(mount_point->data) == -1) {
        return SAVE_DATA_ERROR_NOT_MOUNTED;
    }
    if (icon != nullptr) {
        icon->data_size = 0;
    }
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataMount3(const SaveDataMount3* mount, SaveDataMountResult* mount_result) {
    if (mount == nullptr || mount_result == nullptr || mount->dir_name == nullptr) {
        throw std::runtime_error("sceSaveDataMount3: null argument");
    }
    std::memset(mount_result, 0, sizeof(*mount_result));
    const bool create = (mount->mount_mode & SAVE_DATA_MOUNT_MODE_CREATE) != 0;
    const bool create2 = (mount->mount_mode & SAVE_DATA_MOUNT_MODE_CREATE2) != 0;
    const bool rdonly = (mount->mount_mode & SAVE_DATA_MOUNT_MODE_RDONLY) != 0;
    const bool rdwr = (mount->mount_mode & SAVE_DATA_MOUNT_MODE_RDWR) != 0;
    const bool open = !create && !create2 && (rdonly || rdwr);
    if (!create && !create2 && !open) {
        throw std::runtime_error("sceSaveDataMount3: unknown mount_mode");
    }
    const auto* nameEnd = static_cast<const char*>(std::memchr(mount->dir_name->data, '\0', sizeof(mount->dir_name->data)));
    if (nameEnd == nullptr) {
        throw std::runtime_error("sceSaveDataMount3: unterminated directory name");
    }
    const std::string dirName(mount->dir_name->data, static_cast<std::size_t>(nameEnd - mount->dir_name->data));
    if (dirName.empty() || dirName == "." || dirName == ".." || dirName.find_first_of("/\\:") != std::string::npos) {
        throw std::runtime_error("sceSaveDataMount3: invalid directory name");
    }
    const std::string real_path = save_root() + "/" + dirName;
    const std::string mountPoint = "/" + real_path;
    if (mountPoint.size() >= sizeof(mount_result->mount_point.data)) {
        throw std::runtime_error("sceSaveDataMount3: directory path exceeds mount point capacity");
    }
    if (find_slot_by_mount_point(mountPoint.c_str()) != -1) {
        return SAVE_DATA_ERROR_BUSY;
    }
    const bool exists = std::filesystem::is_directory(real_path);
    if (create && exists) {
        return SAVE_DATA_ERROR_EXISTS;
    }
    if (open && !exists) {
        return SAVE_DATA_ERROR_NOT_FOUND;
    }
    int slot = find_free_slot();
    if (slot == -1) {
        return SAVE_DATA_ERROR_MOUNT_FULL;
    }
    if (create || create2) {
        std::filesystem::create_directories(real_path);
    }
    g_slots[slot].used = true;
    g_slots[slot].mount_point = mountPoint;
    g_slots[slot].real_path = real_path;
    std::memcpy(mount_result->mount_point.data, mountPoint.c_str(), mountPoint.size() + 1);
    mount_result->required_blocks = 0;
    mount_result->mount_status = (create || create2) ? 1u : 0u;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataPrepare(const SaveDataMountPoint* mount_point, const SaveDataPrepareParam* param) {
    (void)mount_point;
    (void)param;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataSaveIcon(const SaveDataMountPoint* mount_point, const SaveDataIcon* icon) {
    (void)icon;
    if (mount_point == nullptr) {
        throw std::runtime_error("sceSaveDataSaveIcon: null mount_point");
    }
    if (find_slot_by_mount_point(mount_point->data) == -1) {
        return SAVE_DATA_ERROR_NOT_MOUNTED;
    }
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataSaveIconByPath(const SaveDataMountPoint* mount_point, const char* path) {
    (void)mount_point;
    (void)path;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataSetParam(const SaveDataMountPoint* mount_point, uint32_t param_type, const void* param_buf, size_t param_buf_size) {
    (void)param_type;
    (void)param_buf;
    (void)param_buf_size;
    if (mount_point == nullptr) {
        throw std::runtime_error("sceSaveDataSetParam: null mount_point");
    }
    if (find_slot_by_mount_point(mount_point->data) == -1) {
        return SAVE_DATA_ERROR_NOT_MOUNTED;
    }
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataSetSaveDataMemory2(const SaveDataMemorySet2* set_param) {
    if (set_param == nullptr) return SAVE_DATA_ERROR_PARAMETER;
    const auto path = save_data_memory_path(set_param->user_id, set_param->slot_id);
    if (!std::filesystem::exists(path)) return SAVE_DATA_ERROR_NOT_FOUND;
    const std::uint64_t size = std::filesystem::file_size(path);
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    for (std::uint32_t index = 0; set_param->data != nullptr && index < set_param->data_num; ++index) {
        const auto& data = set_param->data[index];
        if (data.buf == nullptr || data.offset + data.buf_size > size) return SAVE_DATA_ERROR_PARAMETER;
        stream.seekp(static_cast<std::streamoff>(data.offset));
        stream.write(static_cast<const char*>(data.buf), static_cast<std::streamsize>(data.buf_size));
        if (!stream) return SAVE_DATA_ERROR_OUT_OF_MEMORY;
    }
    stream.flush();
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataSetupSaveDataMemory2(const SaveDataMemorySetup2* setup_param, SaveDataMemorySetupResult* result) {
    if (setup_param == nullptr || setup_param->memory_size == 0) return SAVE_DATA_ERROR_PARAMETER;
    const auto path = save_data_memory_path(setup_param->user_id, setup_param->slot_id);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const std::uint64_t existing = std::filesystem::exists(path) ? std::filesystem::file_size(path) : 0;
    if (existing < setup_param->memory_size) {
        std::ofstream(path, std::ios::binary | std::ios::app).close();
        std::filesystem::resize_file(path, setup_param->memory_size, error);
        if (error) return SAVE_DATA_ERROR_OUT_OF_MEMORY;
    }
    if (result != nullptr) {
        std::memset(result, 0, sizeof(*result));
        result->existed_memory_size = static_cast<std::size_t>(existing);
    }
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataSyncSaveDataMemory(const void* sync_param) {
    (void)sync_param;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataTerminate(void) {
    if (!g_initialized) {
        return SAVE_DATA_ERROR_NOT_INITIALIZED;
    }
    if (any_slot_used()) {
        return SAVE_DATA_ERROR_BUSY;
    }
    g_initialized = false;
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataTransferringMount(const SaveDataTransferringMount* mount, SaveDataMountResult* mount_result) {
    (void)mount;
    (void)mount_result;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSaveDataUmount2(uint32_t mode, const SaveDataMountPoint* mount_point) {
    (void)mode;
    if (mount_point == nullptr) {
        throw std::runtime_error("sceSaveDataUmount2: null mount_point");
    }
    int slot = find_slot_by_mount_point(mount_point->data);
    if (slot == -1) {
        return SAVE_DATA_ERROR_NOT_MOUNTED;
    }
    g_slots[slot] = MountSlot{};
    return SAVE_DATA_OK;
}

}
