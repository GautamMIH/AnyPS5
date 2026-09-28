#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

// A dumped or installed title is fully present on local storage, so PlayGo reports every chunk the
// title defines as already installed on fast local storage, an empty to-do list and completed progress.

namespace {

constexpr int PLAYGO_OK = 0;
constexpr int PLAYGO_ERROR_NOT_INITIALIZED = static_cast<int>(0x80B2000E);
constexpr int PLAYGO_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80B2000F);
constexpr int PLAYGO_ERROR_BAD_HANDLE = static_cast<int>(0x80B20012);
constexpr int PLAYGO_ERROR_BAD_POINTER = static_cast<int>(0x80B20013);
constexpr int PLAYGO_ERROR_BAD_SIZE = static_cast<int>(0x80B20014);
constexpr int PLAYGO_ERROR_BAD_CHUNK_ID = static_cast<int>(0x80B20015);
constexpr int PLAYGO_ERROR_BAD_SPEED = static_cast<int>(0x80B20016);
constexpr int PLAYGO_ERROR_BAD_LOCUS = static_cast<int>(0x80B20019);

constexpr int kHandle = 1;
constexpr std::int8_t kLocusLocalFast = 3;
constexpr std::int32_t kInstallSpeedFull = 2;

std::mutex stateMutex;
bool initialized = false;
bool opened = false;
std::int32_t installSpeed = kInstallSpeedFull;

int checkHandle(int handle) {
    if (!initialized) return PLAYGO_ERROR_NOT_INITIALIZED;
    if (!opened || handle != kHandle) return PLAYGO_ERROR_BAD_HANDLE;
    return PLAYGO_OK;
}

// The package's chunk table is not part of the dump, so the chunk set comes from the title's
// playgo-chunkdefs.xml: every listed chunk plus chunks 0 through the default chunk. A title without
// it has only chunk 0. Games probe chunk IDs and size arrays from the result, so unknown IDs are rejected.
const std::set<uint16_t>& validChunks() {
    static const std::set<uint16_t> chunks = [] {
        std::set<uint16_t> result{0};
        std::ifstream file(ResolvePath_nid_no_patch("/app0/playgo-chunkdefs.xml"));
        if (!file) return result;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        static const std::regex chunk(R"re(<chunk\s+id="(\d+)")re");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), chunk); it != std::sregex_iterator(); ++it)
            result.insert(static_cast<uint16_t>(std::stoul((*it)[1].str())));
        static const std::regex defaultChunk(R"re(default_chunk="(\d+)")re");
        std::smatch match;
        if (std::regex_search(text, match, defaultChunk)) {
            const auto last = std::stoul(match[1].str());
            for (unsigned long id = 0; id <= last && id <= 0xFFFF; ++id) result.insert(static_cast<uint16_t>(id));
        }
        return result;
    }();
    return chunks;
}

bool isValidChunk(uint16_t chunkId) {
    return validChunks().contains(chunkId);
}

int checkChunks(const uint16_t* chunkIds, uint32_t count) {
    if (chunkIds == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    if (count == 0) return PLAYGO_ERROR_BAD_SIZE;
    for (uint32_t i = 0; i < count; ++i)
        if (!isValidChunk(chunkIds[i])) return PLAYGO_ERROR_BAD_CHUNK_ID;
    return PLAYGO_OK;
}

std::uint64_t installedSize() {
    static const std::uint64_t size = [] {
        std::uint64_t total = 0;
        std::error_code error;
        const auto root = ResolvePath_nid_no_patch("/app0");
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::follow_directory_symlink, error), end; !error && it != end; it.increment(error)) {
            std::error_code sizeError;
            if (it->is_regular_file(sizeError)) {
                const auto bytes = it->file_size(sizeError);
                if (!sizeError) total += bytes;
            }
        }
        return total;
    }();
    return size;
}

}

extern "C" {

int APS5_VABI scePlayGoInitialize(const PlayGoInitParams* init) {
    if (init == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    std::lock_guard lock(stateMutex);
    if (initialized) return PLAYGO_ERROR_ALREADY_INITIALIZED;
    initialized = true;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoTerminate(void) {
    std::lock_guard lock(stateMutex);
    if (!initialized) return PLAYGO_ERROR_NOT_INITIALIZED;
    initialized = false;
    opened = false;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoOpen(int* out_handle, const void* param) {
    (void)param;
    if (out_handle == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    std::lock_guard lock(stateMutex);
    if (!initialized) return PLAYGO_ERROR_NOT_INITIALIZED;
    opened = true;
    *out_handle = kHandle;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoClose(int handle) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    opened = false;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoGetChunkId(int handle, uint16_t* out_chunk_id_list, uint32_t number_of_entries, uint32_t* out_entries) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (out_entries == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    const auto& chunks = validChunks();
    if (out_chunk_id_list == nullptr) {
        *out_entries = static_cast<uint32_t>(chunks.size());
        return PLAYGO_OK;
    }
    if (number_of_entries == 0) return PLAYGO_ERROR_BAD_SIZE;
    uint32_t written = 0;
    for (const auto id : chunks) {
        if (written == number_of_entries) break;
        out_chunk_id_list[written++] = id;
    }
    *out_entries = written;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoGetInstallChunkId(int handle, uint16_t* out_chunk_id_list, uint32_t number_of_entries, uint32_t* out_entries) {
    return scePlayGoGetChunkId(handle, out_chunk_id_list, number_of_entries, out_entries);
}

int APS5_VABI scePlayGoGetEta(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries, int64_t* out_eta) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (out_eta == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    if (const int error = checkChunks(chunk_ids, number_of_entries)) return error;
    *out_eta = 0;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoGetInstallSpeed(int handle, int32_t* out_speed) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (out_speed == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    *out_speed = installSpeed;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoSetInstallSpeed(int handle, int32_t speed) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (speed < 0 || speed > kInstallSpeedFull) return PLAYGO_ERROR_BAD_SPEED;
    installSpeed = speed;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoGetLanguageMask(int handle, uint64_t* out_language_mask) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (out_language_mask == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    *out_language_mask = ~0ull;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoGetLocus(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries, int8_t* out_loci) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (out_loci == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    if (const int error = checkChunks(chunk_ids, number_of_entries)) return error;
    for (uint32_t i = 0; i < number_of_entries; ++i) out_loci[i] = kLocusLocalFast;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoGetProgress(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries, PlayGoProgress* out_progress) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (out_progress == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    if (const int error = checkChunks(chunk_ids, number_of_entries)) return error;
    out_progress->total_size = installedSize();
    out_progress->progress_size = out_progress->total_size;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoGetToDoList(int handle, PlayGoToDo* out_todo_list, uint32_t number_of_entries, uint32_t* out_entries) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (out_todo_list == nullptr || out_entries == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    if (number_of_entries == 0) return PLAYGO_ERROR_BAD_SIZE;
    *out_entries = 0;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoSetToDoList(int handle, const PlayGoToDo* todo_list, uint32_t number_of_entries) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (todo_list == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    if (number_of_entries == 0) return PLAYGO_ERROR_BAD_SIZE;
    for (uint32_t i = 0; i < number_of_entries; ++i) {
        if (!isValidChunk(todo_list[i].chunk_id)) return PLAYGO_ERROR_BAD_CHUNK_ID;
        if (todo_list[i].locus < 0 || todo_list[i].locus > kLocusLocalFast) return PLAYGO_ERROR_BAD_LOCUS;
    }
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoPrefetch(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries, int8_t minimum_locus) {
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (minimum_locus < 0 || minimum_locus > kLocusLocalFast) return PLAYGO_ERROR_BAD_LOCUS;
    return checkChunks(chunk_ids, number_of_entries);
}

int APS5_VABI scePlayGoGetOptionalChunk(int handle, int32_t type, PlayGoOptionalChunk* option) {
    (void)type;
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (option == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    option->bitmask = 0;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoGetSupportedOptionalChunk(int handle, int32_t type, PlayGoOptionalChunk* option) {
    (void)type;
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (option == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    option->bitmask = 0;
    return PLAYGO_OK;
}

int APS5_VABI scePlayGoPrefetchOptionalChunk(int handle, int32_t type, const PlayGoOptionalChunk* option) {
    (void)type;
    std::lock_guard lock(stateMutex);
    if (const int error = checkHandle(handle)) return error;
    if (option == nullptr) return PLAYGO_ERROR_BAD_POINTER;
    // Everything is already local, so there is nothing to fetch.
    return PLAYGO_OK;
}

}
