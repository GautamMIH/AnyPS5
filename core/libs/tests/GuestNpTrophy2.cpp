#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libSceNpTrophy2/include/NpTrophy2Types.hpp"
#include <cstdlib>

extern "C" {
int APS5_VABI sceNpTrophy2RegisterUnlockCallback(void*, void*);
int APS5_VABI sceNpTrophy2UnregisterUnlockCallback();
int APS5_VABI sceNpTrophy2GetGameInfo(int, int, NpTrophy2GameDetails*, NpTrophy2GameData*);
}

static void Require(bool value) { if (!value) std::abort(); }

static void APS5_VABI OnUnlock(int, int, void*) {}

int main() {
    int userdata = 0;
    Require(sceNpTrophy2UnregisterUnlockCallback() == 0);
    Require(sceNpTrophy2RegisterUnlockCallback(reinterpret_cast<void*>(&OnUnlock), &userdata) == 0);
    Require(sceNpTrophy2UnregisterUnlockCallback() == 0);
    Require(sceNpTrophy2RegisterUnlockCallback(reinterpret_cast<void*>(&OnUnlock), &userdata) == 0);
    Require(sceNpTrophy2UnregisterUnlockCallback() == 0);

    NpTrophy2GameDetails details{};
    NpTrophy2GameData data{};
    Require(sceNpTrophy2GetGameInfo(1, 1, &details, nullptr) == 0);
    Require(details.num_trophies != 0);
    Require(sceNpTrophy2GetGameInfo(1, 1, nullptr, &data) == 0);
    // Without either output the call fails with the library's INVALID_ARGUMENT, which titles handle,
    // rather than throwing as upstream does.
    constexpr int invalidArgument = static_cast<int>(0x80551604u);
    Require(sceNpTrophy2GetGameInfo(1, 1, nullptr, nullptr) == invalidArgument);
}
