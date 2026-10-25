#pragma once

namespace WindowSystem
{
    struct WindowHandleInfo
    {
        enum class Backend { X11, Wayland, Cocoa, Windows } backend;
        void* display = nullptr;
        void* surface = nullptr;
    };

    enum struct PlatformKeyCodes : uint32 { LCONTROL, RCONTROL, TAB, ESCAPE };

    struct WindowInfo
    {
        std::atomic_bool app_active{true};
        std::atomic_int32_t width, height;
        std::atomic_int32_t phys_width, phys_height;
        std::atomic<double> dpi_scale{1.0};
        std::atomic_bool pad_open{false};
        std::atomic_int32_t pad_width, pad_height;
        std::atomic_int32_t phys_pad_width, phys_pad_height;
        std::atomic<double> pad_dpi_scale{1.0};
        std::atomic_bool pad_maximized = false;
        std::atomic_bool is_fullscreen{false};
        std::atomic_bool debugger_focused{false};
        WindowHandleInfo window_main;
        WindowHandleInfo window_pad;
        WindowHandleInfo canvas_main;
        WindowHandleInfo canvas_pad;
        void set_keystate(uint32 key, bool state)
        {
            std::lock_guard lock(keycode_mutex);
            keydown[key] = state;
        }
        bool get_keystate(uint32 key)
        {
            std::lock_guard lock(keycode_mutex);
            auto it = keydown.find(key);
            return it != keydown.end() && it->second;
        }
        template<typename Fn> void iter_keystates(Fn fn)
        {
            std::lock_guard lock(keycode_mutex);
            for (auto& el : keydown) fn(el);
        }
    private:
        std::mutex keycode_mutex;
        std::unordered_map<uint32, bool> keydown;
    };

    WindowInfo& GetWindowInfo();
    void UpdateWindowTitles(bool isIdle, bool isLoading, double fps);
    void GetWindowSize(int& w, int& h);
    void GetPadWindowSize(int& w, int& h);
    void GetWindowPhysSize(int& w, int& h);
    void GetPadWindowPhysSize(int& w, int& h);
    double GetWindowDPIScale();
    double GetPadDPIScale();
    bool IsPadWindowOpen();
    bool IsKeyDown(uint32 key);
    bool IsKeyDown(PlatformKeyCodes key);
    bool IsFullScreen();
    inline void ShowErrorDialog(std::string_view message, std::string_view title = "") {}
}
