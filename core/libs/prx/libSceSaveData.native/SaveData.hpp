#ifndef CORE_LIBS_PRX_LIBSCESAVEDATANATIVE_SAVEDATA_HPP
#define CORE_LIBS_PRX_LIBSCESAVEDATANATIVE_SAVEDATA_HPP

#include <array>
#include <cstdint>
#include <string>

// Error codes as in shadPS4's savedata_error.h.
constexpr int SAVE_DATA_OK = 0;
constexpr int SAVE_DATA_ERROR_PARAMETER = static_cast<int>(0x809F0000);
constexpr int SAVE_DATA_ERROR_NOT_INITIALIZED = static_cast<int>(0x809F0001);
constexpr int SAVE_DATA_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x809F0005);
constexpr int SAVE_DATA_ERROR_OUT_OF_MEMORY = static_cast<int>(0x809F0002);
constexpr int SAVE_DATA_ERROR_BUSY = static_cast<int>(0x809F0003);
constexpr int SAVE_DATA_ERROR_NOT_MOUNTED = static_cast<int>(0x809F0004);
constexpr int SAVE_DATA_ERROR_EXISTS = static_cast<int>(0x809F0007);
constexpr int SAVE_DATA_ERROR_NOT_FOUND = static_cast<int>(0x809F0008);
constexpr int SAVE_DATA_ERROR_INTERNAL = static_cast<int>(0x809F000B);
constexpr int SAVE_DATA_ERROR_MOUNT_FULL = static_cast<int>(0x809F000C);
constexpr int SAVE_DATA_ERROR_MEMORY_NOT_READY = static_cast<int>(0x809F0012);

constexpr std::uint32_t SAVE_DATA_MOUNT_MODE_RDONLY = 1;
constexpr std::uint32_t SAVE_DATA_MOUNT_MODE_RDWR = 2;
constexpr std::uint32_t SAVE_DATA_MOUNT_MODE_CREATE = 4;
constexpr std::uint32_t SAVE_DATA_MOUNT_MODE_CREATE2 = 32;

constexpr std::uint64_t SAVE_DATA_BLOCKS_MAX = 32768;
constexpr std::uint32_t SAVE_DATA_PARAM_TYPE_ALL = 0;
constexpr std::uint32_t SAVE_DATA_PARAM_TYPE_TITLE = 1;
constexpr std::uint32_t SAVE_DATA_PARAM_TYPE_SUB_TITLE = 2;
constexpr std::uint32_t SAVE_DATA_PARAM_TYPE_DETAIL = 3;
constexpr std::uint32_t SAVE_DATA_PARAM_TYPE_USER_PARAM = 4;
constexpr std::uint32_t SAVE_DATA_PARAM_TYPE_MTIME = 5;

constexpr std::size_t SAVE_DATA_MOUNT_SLOTS = 16;

struct MountSlot {
    bool used = false;
    std::string mount_point;
    std::string real_path;
};

inline std::array<MountSlot, SAVE_DATA_MOUNT_SLOTS> g_slots;

inline int find_slot_by_mount_point(const char* mount_point) {
    for (int i = 0; i < static_cast<int>(SAVE_DATA_MOUNT_SLOTS); i++) {
        if (g_slots[i].used && g_slots[i].mount_point == mount_point) {
            return i;
        }
    }
    return -1;
}

inline int find_free_slot() {
    for (int i = 0; i < static_cast<int>(SAVE_DATA_MOUNT_SLOTS); i++) {
        if (!g_slots[i].used) {
            return i;
        }
    }
    return -1;
}

inline bool any_slot_used() {
    for (const auto& s : g_slots) {
        if (s.used) {
            return true;
        }
    }
    return false;
}

#endif
