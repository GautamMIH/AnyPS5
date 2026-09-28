#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_PADINPUT_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_PADINPUT_HPP

#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "prx/libScePad/include/InputMapping.hpp"
#include <array>
#include <chrono>
#include <vector>

class DisplayWindow;

class PadInput {
public:
    PadInput();
    ~PadInput();
    void HandleEvent(const SDL_Event& event, DisplayWindow& window);
    void Update();

private:
    void publish();
    void setMouseMode(bool enabled);
    void openFirstAvailableController();
    void openController(int deviceIndex);
    void closeController();
    void loadScript();
    std::uint32_t scriptedButtons(std::chrono::steady_clock::time_point now) const;
    // Debug aid: ANYPS5_SCRIPTED_INPUT="30:cross,34.5:cross:0.5,..." holds a pad button for the given
    // seconds (default 0.2) starting that many seconds after input starts.
    struct ScriptedPress {
        double at;
        double hold;
        std::uint32_t button;
    };
    std::vector<ScriptedPress> script;
    std::chrono::steady_clock::time_point scriptStart = std::chrono::steady_clock::now();
    std::uint32_t scriptedPressed = 0;
    std::vector<Pad::InputBinding> bindings;
    std::vector<bool> pressed;
    std::vector<std::chrono::steady_clock::time_point> wheelReleaseTimes;
    std::array<std::uint8_t, 2> mouseStick{128, 128};
    std::chrono::steady_clock::time_point nextMousePoll{};
    SDL_GameController* controller = nullptr;
    bool mouseEnabled = false;
};

#endif
