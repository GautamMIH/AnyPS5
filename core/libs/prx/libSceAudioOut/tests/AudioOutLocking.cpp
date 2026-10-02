#include "SceTypes.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceAudioOutInit();
int APS5_VABI sceAudioOutOpen(int userId, int type, int index, std::uint32_t len, std::uint32_t freq, std::uint32_t param);
int APS5_VABI sceAudioOutClose(int handle);
int APS5_VABI sceAudioOutOutput(int handle, const void* ptr);
int APS5_VABI sceAudioOutGetPortState(int handle, AudioOutPortState* state);
}

static void require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "AudioOut locking check failed: %s\n", message);
    std::abort();
}

// While one thread is paced by a port's output (a block of one second without an audio device), the
// library's other calls return at once: the wait holds the port's output lock, not the table's.
int main() {
    // No audio device: outputs are paced by sleeping for each block's duration.
    setenv("SDL_AUDIODRIVER", "anyps5-none", 1);
    require(sceAudioOutInit() == 0, "init");
    constexpr std::uint32_t Samples = 48000;
    const int handle = sceAudioOutOpen(0, 0, 0, Samples, 48000, 1);
    require(handle > 0, "open");
    std::vector<std::int16_t> block(Samples * 2, 0);
    std::atomic<bool> started{false};
    std::thread output([&] {
        require(sceAudioOutOutput(handle, block.data()) == static_cast<int>(Samples), "first output");
        started = true;
        require(sceAudioOutOutput(handle, block.data()) == static_cast<int>(Samples), "second output");
    });
    while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto before = std::chrono::steady_clock::now();
    AudioOutPortState state{};
    require(sceAudioOutGetPortState(handle, &state) == 0, "port state");
    const auto waited = std::chrono::steady_clock::now() - before;
    require(waited < std::chrono::milliseconds(100), "sceAudioOutGetPortState waited for another thread's paced output");
    output.join();
    require(sceAudioOutClose(handle) == 0, "close");
    std::puts("AudioOut locking tests passed");
    return 0;
}
