#pragma once

struct WFEmulatedController
{
    std::atomic_bool screenActive{false};
    std::atomic_bool screenActiveToggle{false};
    bool is_screen_active() const { return screenActive; }
    bool is_screen_active_toggle() const { return screenActiveToggle; }
    std::string get_profile_name() const { return {}; }
    bool is_battery_low() const { return false; }
    std::atomic<uint32> navButtons{0};
    enum NavButton : uint32 { kNavStart = 1, kNavA = 2, kNavB = 4, kNavLeft = 8, kNavRight = 16, kNavUp = 32, kNavDown = 64 };
    bool is_start_down() const { return navButtons & kNavStart; }
    bool is_a_down() const { return navButtons & kNavA; }
    bool is_b_down() const { return navButtons & kNavB; }
    bool is_left_down() const { return navButtons & kNavLeft; }
    bool is_right_down() const { return navButtons & kNavRight; }
    bool is_up_down() const { return navButtons & kNavUp; }
    bool is_down_down() const { return navButtons & kNavDown; }
};

struct WFMousePos { sint32 x, y; };

class InputManager
{
public:
    static constexpr int kMaxController = 8;
    static InputManager& instance() { static InputManager m; return m; }
    WFEmulatedController* get_vpad_controller(size_t channel) { return channel == 0 ? &m_gamepad : nullptr; }
    WFEmulatedController* get_controller(size_t index) { return index == 0 ? &m_gamepad : nullptr; }
    WFMousePos get_mouse_position(bool padWindow) const { return {m_mouseX, m_mouseY}; }
    std::optional<WFMousePos> get_left_down_mouse_info(bool* padWindow) const
    {
        *padWindow = false;
        if (!m_mouseLeftDown) return std::nullopt;
        return WFMousePos{m_mouseX, m_mouseY};
    }
    std::atomic<sint32> m_mouseX{-1}, m_mouseY{-1};
    std::atomic_bool m_mouseLeftDown{false};
private:
    WFEmulatedController m_gamepad;
};
