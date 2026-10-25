#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <string>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "core/game_build.h"
#include "core/setup.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace wf::setup {
namespace fs = std::filesystem;

std::string verify(const fs::path& dir);
fs::path locate(const fs::path& chosen);
fs::path pick_folder(HWND owner);

namespace {

std::string narrow(const fs::path& p)
{
    std::u8string u = p.u8string();
    return std::string((const char*)u.data(), u.size());
}

class Panel
{
public:
    Panel(const wchar_t* title, int width, int height)
    {
        m_oldDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"WindfallSetup";
        RegisterClassW(&wc);
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        m_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr,
                                 inst, nullptr);
        if (!m_hwnd) return;
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
        UINT dpi = GetDpiForWindow(m_hwnd);
        float scale = dpi / 96.0f;
        RECT r{0, 0, (LONG)(width * scale), (LONG)(height * scale)};
        AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_ACCEPTFILES, dpi);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        int w = r.right - r.left, h = r.bottom - r.top;
        SetWindowPos(m_hwnd, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - h) / 2, w, h, SWP_NOZORDER);

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = m_hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
            if (D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &m_swap, &m_dev, nullptr, &m_ctx) == S_OK)
                break;
        if (!m_swap) return;
        create_target();

        IMGUI_CHECKVERSION();
        m_imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_imgui);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(scale);
        wchar_t windows[MAX_PATH];
        fs::path segoe = GetWindowsDirectoryW(windows, MAX_PATH) ? fs::path(windows) / "Fonts" / "segoeui.ttf" : fs::path();
        std::error_code ec;
        if (segoe.empty() || !fs::exists(segoe, ec) || !io.Fonts->AddFontFromFileTTF(narrow(segoe).c_str(), 18.0f * scale))
        {
            ImFontConfig cfg;
            cfg.SizePixels = 13.0f * scale;
            io.Fonts->AddFontDefault(&cfg);
        }
        ImGui_ImplWin32_Init(m_hwnd);
        ImGui_ImplDX11_Init(m_dev, m_ctx);
        ShowWindow(m_hwnd, SW_SHOW);
    }

    ~Panel()
    {
        if (m_imgui)
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(m_imgui);
        }
        if (m_rtv) m_rtv->Release();
        if (m_swap) m_swap->Release();
        if (m_ctx) m_ctx->Release();
        if (m_dev) m_dev->Release();
        if (m_hwnd) DestroyWindow(m_hwnd);
        if (m_oldDpi) SetThreadDpiAwarenessContext(m_oldDpi);
    }

    bool ok() const { return m_imgui != nullptr; }
    HWND hwnd() const { return m_hwnd; }
    std::function<void(const fs::path&)> onDrop;

    void run(const std::function<bool()>& frame)
    {
        while (!m_closed)
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (m_closed) break;
            bool hidden = IsIconic(m_hwnd);
            if (hidden) Sleep(50);
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::Begin("##setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            bool keep = frame();
            ImGui::End();
            ImGui::Render();
            if (!hidden && m_rtv)
            {
                const float clear[4] = {0, 0, 0, 1};
                m_ctx->OMSetRenderTargets(1, &m_rtv, nullptr);
                m_ctx->ClearRenderTargetView(m_rtv, clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                m_swap->Present(1, 0);
            }
            if (!keep) break;
        }
    }

private:
    void create_target()
    {
        ID3D11Texture2D* back = nullptr;
        if (m_swap->GetBuffer(0, IID_PPV_ARGS(&back)) == S_OK)
        {
            m_dev->CreateRenderTargetView(back, nullptr, &m_rtv);
            back->Release();
        }
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
        Panel* self = (Panel*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        switch (msg)
        {
        case WM_SIZE:
            if (self && self->m_swap && wp != SIZE_MINIMIZED)
            {
                if (self->m_rtv) self->m_rtv->Release();
                self->m_rtv = nullptr;
                self->m_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                self->create_target();
            }
            return 0;
        case WM_DPICHANGED:
        {
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DROPFILES:
        {
            HDROP drop = (HDROP)wp;
            wchar_t path[MAX_PATH * 4];
            if (self && self->onDrop && DragQueryFileW(drop, 0, path, (UINT)std::size(path))) self->onDrop(fs::path(path));
            DragFinish(drop);
            return 0;
        }
        case WM_CLOSE:
            if (self) self->m_closed = true;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND m_hwnd = nullptr;
    DPI_AWARENESS_CONTEXT m_oldDpi = nullptr;
    ID3D11Device* m_dev = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    IDXGISwapChain* m_swap = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;
    ImGuiContext* m_imgui = nullptr;
    bool m_closed = false;
};

void wrapped(const char* text, const ImVec4* color = nullptr)
{
    ImGui::PushTextWrapPos(0.0f);
    if (color) ImGui::TextColored(*color, "%s", text);
    else ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
}

const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f), kBad(1.0f, 0.45f, 0.40f, 1.0f), kDim(0.70f, 0.70f, 0.70f, 1.0f);

}

fs::path setup_screen(const fs::path& start)
{
    Panel window(L"Windfall setup", 640, 185);
    if (!window.ok())
    {
        MessageBoxW(nullptr, L"Windfall couldn't open its setup window.\n\nStart it with: windfall.exe --game \"<your Wind Waker HD folder>\"",
                    L"Windfall", MB_ICONERROR);
        return {};
    }
    fs::path dir;
    std::string status;
    bool ok = false, play = false;
    auto choose = [&](const fs::path& chosen) {
        if (chosen.empty()) return;
        dir = locate(chosen);
        status = verify(dir);
        ok = status.empty();
        if (ok) status = "Found The Wind Waker HD (USA). Ready to play.";
    };
    window.onDrop = choose;
    if (!start.empty()) choose(start);
    window.run([&] {
        ImGui::TextUnformatted("Select your Wind Waker HD install");
        wrapped("Your own dump of the Wii U game (USA): the folder that contains code, content and meta. You can also drop it on this window.", &kDim);
        ImGui::Spacing();
        std::string shown = narrow(dir);
        const char* browse = "Browse...";
        ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(browse).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x));
        ImGui::InputTextWithHint("##folder", "No folder selected", shown.data(), shown.size() + 1, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button(browse)) choose(pick_folder(window.hwnd()));
        if (!status.empty()) wrapped(status.c_str(), ok ? &kGood : &kBad);
        ImGui::Spacing();
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Play") || (ok && ImGui::IsKeyPressed(ImGuiKey_Enter)))
            play = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool quit = ImGui::Button("Quit");
        return !play && !quit;
    });
    return play && ok ? dir : fs::path();
}

namespace {

float build_fraction(const game_build::Progress& p, double secs)
{
    switch (p.stage.load())
    {
    case 0: return (float)(0.30 * std::min(0.97, secs / 70.0));
    case 1: return (float)(0.30 + 0.65 * (p.total ? (double)p.done / p.total : 0.0));
    case 2: return 0.96f;
    default: return 1.0f;
    }
}

}

bool build_screen(const fs::path& gameDir, const std::string& reason)
{
    std::unique_ptr<game_build::Progress> p;
    auto started = std::chrono::steady_clock::now();
    auto start_build = [&] {
        p = std::make_unique<game_build::Progress>();
        started = std::chrono::steady_clock::now();
        game_build::start(gameDir, *p);
    };
    std::string intro = reason.empty() ? std::string() : reason + ". ";
    if (!intro.empty()) intro[0] = (char)std::toupper((unsigned char)intro[0]);
    intro += "Windfall translates the code from your copy of the game and compiles it for this PC. This happens once, takes a few "
             "minutes and works offline. Nothing from the game is uploaded or shared.";
    bool ok = false;
    {
        Panel window(L"Windfall setup", 640, 210);
        start_build();
        if (!window.ok())
        {
            while (!p->finished) Sleep(100);
            return !p->failed;
        }
        window.run([&] {
            ImGui::TextUnformatted("Preparing the game for your PC");
            wrapped(intro.c_str(), &kDim);
            ImGui::Spacing();
            if (p->finished && !p->failed)
            {
                ok = true;
                return false;
            }
            if (p->finished)
            {
                std::string error;
                {
                    std::lock_guard lk(p->lock);
                    error = p->error;
                }
                wrapped(error.c_str(), &kBad);
                ImGui::Spacing();
                if (ImGui::Button("Try again")) start_build();
                ImGui::SameLine();
                return !ImGui::Button("Quit");
            }
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            int s = (int)secs;
            std::string status = p->stage == 0   ? std::format("Translating the game's code...   {}:{:02}", s / 60, s % 60)
                                 : p->stage == 1 ? std::format("Compiling: {} of {} parts   {}:{:02}", p->done.load(), p->total.load(), s / 60, s % 60)
                                                 : std::string("Putting it together...");
            ImGui::ProgressBar(build_fraction(*p, secs), ImVec2(-FLT_MIN, 0));
            ImGui::TextUnformatted(status.c_str());
            ImGui::Spacing();
            return !ImGui::Button("Cancel");
        });
    }
    p->cancel = true;
    while (!p->finished) Sleep(50);
    return ok;
}

}
