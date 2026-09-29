#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "SaveData.hpp"

static constexpr char SAVE_DIR[] = "_sd";

// Backups live beside the save root so directory searches never report them as save data.
static constexpr char BACKUP_DIR[] = "_sd_backup";

// OrbisSaveDataEventType::BACKUP (shadPS4 save_backup.h).
static constexpr std::uint32_t SAVE_DATA_EVENT_TYPE_BACKUP = 2;

static std::atomic<std::int32_t> g_transaction_counter{1};
static bool g_initialized = false;

// Completion events of asynchronous operations, read back through sceSaveDataGetEventResult.
static std::mutex g_event_mutex;
static std::deque<SaveDataEvent> g_events;

static std::string save_root() {
    return std::string(SAVE_DIR);
}

namespace {

// Save-data memory: one blob per user and slot under the save root, with a .param sidecar holding
// the last SaveDataParam the title wrote.
constexpr std::size_t MEM_MAX_SIZE = 0x1000000;  // 16 MiB
std::mutex g_mem_mutex;

std::string mem_path(std::int32_t user_id, std::uint32_t slot_id, const char* ext) {
    const auto name = std::to_string(user_id) + "_" + std::to_string(slot_id) + "." + ext;
    return (std::filesystem::path(save_root()) / "savedatamemory" / name).string();
}

bool file_size_of(const std::string& path, std::size_t* out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    *out = static_cast<std::size_t>(sz);
    return true;
}

// Writes to a temporary file, then renames it over the target, so a kill mid-write never leaves a
// torn save behind.
bool write_file_replace(const std::string& path, const std::vector<char>& data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        if (!data.empty()) {
            f.write(data.data(), static_cast<std::streamsize>(data.size()));
        }
        f.flush();
        if (!f) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

bool read_file_all(const std::string& path, std::vector<char>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    out.resize(n);
    if (n != 0) {
        f.read(out.data(), static_cast<std::streamsize>(n));
    }
    return static_cast<bool>(f);
}

}  // namespace

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
    // Order and results follow shadPS4: initialization, parameters, then BUSY while the directory
    // is mounted. The copy happens here rather than on a backup thread, so the completion event
    // is ready when the call returns.
    if (!g_initialized) return SAVE_DATA_ERROR_NOT_INITIALIZED;
    if (backup == nullptr || backup->dir_name == nullptr) return SAVE_DATA_ERROR_PARAMETER;
    const std::string dir_name(backup->dir_name->data, strnlen(backup->dir_name->data, sizeof(backup->dir_name->data)));
    const std::filesystem::path source = std::filesystem::path(save_root()) / dir_name;
    for (const auto& slot : g_slots) {
        if (slot.used && std::filesystem::path(slot.real_path) == source) return SAVE_DATA_ERROR_BUSY;
    }
    if (!dir_name.empty() && std::filesystem::is_directory(source)) {
        const std::filesystem::path target = std::filesystem::path(BACKUP_DIR) / dir_name;
        std::error_code error;
        std::filesystem::remove_all(target, error);
        std::filesystem::create_directories(target.parent_path(), error);
        std::filesystem::copy(source, target, std::filesystem::copy_options::recursive, error);
    }
    SaveDataEvent event{};
    event.type = SAVE_DATA_EVENT_TYPE_BACKUP;
    event.error_code = SAVE_DATA_OK;
    event.user_id = backup->user_id;
    if (backup->title_id != nullptr) event.title_id = *backup->title_id;
    event.dir_name = *backup->dir_name;
    std::lock_guard lock(g_event_mutex);
    g_events.push_back(event);
    return SAVE_DATA_OK;
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
    if (!g_initialized) return SAVE_DATA_ERROR_NOT_INITIALIZED;
    if (event == nullptr) return SAVE_DATA_ERROR_PARAMETER;
    std::lock_guard lock(g_event_mutex);
    if (g_events.empty()) return SAVE_DATA_ERROR_NOT_FOUND;
    *event = g_events.front();
    g_events.pop_front();
    return SAVE_DATA_OK;
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
    if (get_param == nullptr) {
        return SAVE_DATA_ERROR_PARAMETER;
    }
    if (!g_initialized) {
        return SAVE_DATA_ERROR_NOT_INITIALIZED;
    }
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    const std::string path = mem_path(get_param->user_id, get_param->slot_id, "bin");
    std::size_t size = 0;
    if (!file_size_of(path, &size)) {
        return SAVE_DATA_ERROR_MEMORY_NOT_READY;
    }
    const SaveDataMemoryData* d = get_param->data;
    if (d != nullptr && d->buf_size != 0) {
        if (d->buf == nullptr || d->offset > size || d->buf_size > size - d->offset) {
            return SAVE_DATA_ERROR_PARAMETER;
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return SAVE_DATA_ERROR_INTERNAL;
        }
        f.seekg(static_cast<std::streamoff>(d->offset));
        f.read(static_cast<char*>(d->buf), static_cast<std::streamsize>(d->buf_size));
        if (!f) {
            return SAVE_DATA_ERROR_INTERNAL;
        }
    }
    if (get_param->param != nullptr) {
        std::memset(get_param->param, 0, sizeof(SaveDataParam));
        std::vector<char> pd;
        if (read_file_all(mem_path(get_param->user_id, get_param->slot_id, "param"), pd)) {
            std::memcpy(get_param->param, pd.data(), std::min(pd.size(), sizeof(SaveDataParam)));
        }
    }
    if (get_param->icon != nullptr) {
        get_param->icon->data_size = 0;
    }
    return SAVE_DATA_OK;
}

// Initialization is idempotent (shadPS4).
int APS5_VABI sceSaveDataInitialize3(const void* init) {
    (void)init;
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
    for (const auto& mounted : g_slots) {
        if (mounted.used && mounted.real_path == real_path) return SAVE_DATA_ERROR_BUSY;
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
    // Games see /savedataN, as on the console (shadPS4); the filesystem serves it from real_path.
    const std::string mountPoint = "/savedata" + std::to_string(slot);
    MountGuestPath_nid_no_patch(mountPoint.c_str(), real_path);
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
    if (set_param == nullptr) {
        return SAVE_DATA_ERROR_PARAMETER;
    }
    if (!g_initialized) {
        return SAVE_DATA_ERROR_NOT_INITIALIZED;
    }
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    const std::string path = mem_path(set_param->user_id, set_param->slot_id, "bin");
    std::size_t size = 0;
    if (!file_size_of(path, &size)) {
        return SAVE_DATA_ERROR_MEMORY_NOT_READY;
    }
    // Validate every range first so a bad entry never leaves a partial write.
    const std::uint32_t n = set_param->data != nullptr ? (set_param->data_num != 0 ? set_param->data_num : 1u) : 0u;
    for (std::uint32_t i = 0; i < n; i++) {
        const SaveDataMemoryData& d = set_param->data[i];
        if (d.buf_size == 0) {
            continue;
        }
        if (d.buf == nullptr || d.offset > size || d.buf_size > size - d.offset) {
            return SAVE_DATA_ERROR_PARAMETER;
        }
    }
    if (n != 0) {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!f) {
            return SAVE_DATA_ERROR_INTERNAL;
        }
        for (std::uint32_t i = 0; i < n; i++) {
            const SaveDataMemoryData& d = set_param->data[i];
            if (d.buf_size == 0) {
                continue;
            }
            f.seekp(static_cast<std::streamoff>(d.offset));
            f.write(static_cast<const char*>(d.buf), static_cast<std::streamsize>(d.buf_size));
        }
        f.flush();
        if (!f) {
            return SAVE_DATA_ERROR_INTERNAL;
        }
    }
    if (set_param->param != nullptr) {
        std::vector<char> pd(sizeof(SaveDataParam));
        std::memcpy(pd.data(), set_param->param, sizeof(SaveDataParam));
        write_file_replace(mem_path(set_param->user_id, set_param->slot_id, "param"), pd);
    }
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataSetupSaveDataMemory2(const SaveDataMemorySetup2* setup_param, SaveDataMemorySetupResult* result) {
    if (setup_param == nullptr) {
        return SAVE_DATA_ERROR_PARAMETER;
    }
    if (!g_initialized) {
        return SAVE_DATA_ERROR_NOT_INITIALIZED;
    }
    if (setup_param->memory_size == 0 || setup_param->memory_size > MEM_MAX_SIZE) {
        return SAVE_DATA_ERROR_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    const std::string path = mem_path(setup_param->user_id, setup_param->slot_id, "bin");
    std::size_t existed = 0;
    const bool have = file_size_of(path, &existed);
    if (!have) {
        existed = 0;
    }
    // First run: create a zero-filled blob and report existed size 0 so the title treats it as a new save.
    if (!have || existed < setup_param->memory_size) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::vector<char> data;
        if (have) {
            read_file_all(path, data);
        }
        data.resize(setup_param->memory_size, 0);
        if (!write_file_replace(path, data)) {
            return SAVE_DATA_ERROR_INTERNAL;
        }
        if (setup_param->init_param != nullptr && (setup_param->option & 1u) != 0) {
            std::vector<char> pd(sizeof(SaveDataParam));
            std::memcpy(pd.data(), setup_param->init_param, sizeof(SaveDataParam));
            write_file_replace(mem_path(setup_param->user_id, setup_param->slot_id, "param"), pd);
        }
    }
    if (result != nullptr) {
        std::memset(result, 0, sizeof(*result));
        result->existed_memory_size = have ? existed : 0;
    }
    return SAVE_DATA_OK;
}

int APS5_VABI sceSaveDataSyncSaveDataMemory(const void* sync_param) {
    if (sync_param == nullptr) {
        return SAVE_DATA_ERROR_PARAMETER;
    }
    // Every Set already writes straight through to disk; sync only has to confirm the memory exists.
    const std::int32_t user_id = *static_cast<const std::int32_t*>(sync_param);
    const std::uint32_t slot_id = reinterpret_cast<const std::uint32_t*>(sync_param)[1];
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    std::size_t size = 0;
    if (!file_size_of(mem_path(user_id, slot_id, "bin"), &size)) {
        return SAVE_DATA_ERROR_MEMORY_NOT_READY;
    }
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
    if (mount_result != nullptr) {
        std::memset(mount_result, 0, sizeof(*mount_result));
    }
    // No PS4-to-PS5 transfer data exists on this console.
    return SAVE_DATA_ERROR_NOT_FOUND;
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
    UnmountGuestPath_nid_no_patch(g_slots[slot].mount_point.c_str());
    g_slots[slot] = MountSlot{};
    return SAVE_DATA_OK;
}

}
