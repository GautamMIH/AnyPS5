#include "../include/Pthread.hpp"
#include "../include/PthreadSync.hpp"
#include "prx/libc/include/General.hpp"
#include <array>
#include <cstdint>
#include <mutex>

namespace {

constexpr int kMaxKeys = 256;
constexpr int kDestructorIterations = 4;
constexpr int kErrorAgain = 35;

struct KeySlot {
    bool Used = false;
    std::uint64_t Generation = 0;
    pthread_key_destructor_func_t Destructor = nullptr;
};

std::mutex& keyMutex() {
    static std::mutex instance;
    return instance;
}

std::array<KeySlot, kMaxKeys>& keySlots() {
    static std::array<KeySlot, kMaxKeys> instance;
    return instance;
}

bool slotIndex(const PthreadKey key, std::size_t& index) {
    if (key < 1 || key > kMaxKeys)
        return false;
    index = static_cast<std::size_t>(key - 1);
    return true;
}

struct ThreadValues {
    std::array<void*, kMaxKeys> Values{};
    std::array<std::uint64_t, kMaxKeys> Generations{};

    ~ThreadValues() {
        for (int iteration = 0; iteration < kDestructorIterations; ++iteration) {
            bool called = false;
            for (std::size_t index = 0; index < kMaxKeys; ++index) {
                void* value = Values[index];
                if (value == nullptr)
                    continue;
                pthread_key_destructor_func_t destructor = nullptr;
                {
                    const std::lock_guard lock(keyMutex());
                    const auto& slot = keySlots()[index];
                    if (slot.Used && slot.Generation == Generations[index])
                        destructor = slot.Destructor;
                }
                Values[index] = nullptr;
                if (destructor != nullptr) {
                    destructor(value);
                    called = true;
                }
            }
            if (!called)
                return;
        }
    }
};

ThreadValues& threadValues() {
    thread_local ThreadValues instance;
    return instance;
}

}

namespace PthreadSync {

int KeyCreate(PthreadKey* key, pthread_key_destructor_func_t destructor) {
    if (key == nullptr)
        return kErrorInvalid;
    const std::lock_guard lock(keyMutex());
    auto& slots = keySlots();
    for (std::size_t index = 0; index < kMaxKeys; ++index) {
        if (slots[index].Used)
            continue;
        slots[index].Used = true;
        ++slots[index].Generation;
        slots[index].Destructor = destructor;
        *key = static_cast<PthreadKey>(index + 1);
        return 0;
    }
    return kErrorAgain;
}

int KeyDelete(const PthreadKey key) {
    std::size_t index = 0;
    if (!slotIndex(key, index))
        return kErrorInvalid;
    const std::lock_guard lock(keyMutex());
    auto& slot = keySlots()[index];
    if (!slot.Used)
        return kErrorInvalid;
    slot.Used = false;
    slot.Destructor = nullptr;
    return 0;
}

void* KeyGet(const PthreadKey key) {
    std::size_t index = 0;
    if (!slotIndex(key, index))
        return nullptr;
    std::uint64_t generation;
    {
        const std::lock_guard lock(keyMutex());
        const auto& slot = keySlots()[index];
        if (!slot.Used)
            return nullptr;
        generation = slot.Generation;
    }
    auto& values = threadValues();
    return values.Generations[index] == generation ? values.Values[index] : nullptr;
}

int KeySet(const PthreadKey key, void* value) {
    std::size_t index = 0;
    if (!slotIndex(key, index))
        return kErrorInvalid;
    std::uint64_t generation;
    {
        const std::lock_guard lock(keyMutex());
        const auto& slot = keySlots()[index];
        if (!slot.Used)
            return kErrorInvalid;
        generation = slot.Generation;
    }
    auto& values = threadValues();
    values.Values[index] = value;
    values.Generations[index] = generation;
    return 0;
}

}

extern "C" {

int APS5_VABI scePthreadKeyCreate(PthreadKey* key, pthread_key_destructor_func_t destructor) {
    return PthreadSync::SceError(PthreadSync::KeyCreate(key, destructor));
}

int APS5_VABI scePthreadKeyDelete(PthreadKey key) {
    return PthreadSync::SceError(PthreadSync::KeyDelete(key));
}

void* APS5_VABI scePthreadGetspecific(PthreadKey key) {
    return PthreadSync::KeyGet(key);
}

int APS5_VABI scePthreadSetspecific(PthreadKey key, void* value) {
    return PthreadSync::SceError(PthreadSync::KeySet(key, value));
}

}
