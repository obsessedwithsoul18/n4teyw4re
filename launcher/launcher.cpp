#include <Windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>

#include <string>
#include <string_view>
#include <filesystem>
#include <thread>
#include <atomic>
#include <vector>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

// ============================================================================
//  Palette
// ============================================================================
namespace pal
{
    // background layers
    constexpr ImVec4 bg       { 10/255.f,  9/255.f, 18/255.f, 1.f };
    constexpr ImVec4 panel    { 16/255.f, 14/255.f, 28/255.f, 1.f };
    constexpr ImVec4 card     { 20/255.f, 18/255.f, 34/255.f, 1.f };
    constexpr ImVec4 card_sel { 28/255.f, 16/255.f, 52/255.f, 1.f };
    constexpr ImVec4 sidebar  { 13/255.f, 11/255.f, 22/255.f, 1.f };

    // accent
    constexpr ImVec4 purple      { 140/255.f,  60/255.f, 255/255.f, 1.f };
    constexpr ImVec4 purple_dim  {  90/255.f,  35/255.f, 180/255.f, 1.f };
    constexpr ImVec4 purple_glow { 140/255.f,  60/255.f, 255/255.f, 0.18f };
    constexpr ImVec4 purple_line { 140/255.f,  60/255.f, 255/255.f, 0.45f };

    // text
    constexpr ImVec4 text_hi    { 240/255.f, 238/255.f, 255/255.f, 1.f };
    constexpr ImVec4 text_mid   { 170/255.f, 165/255.f, 200/255.f, 1.f };
    constexpr ImVec4 text_lo    {  90/255.f,  85/255.f, 120/255.f, 1.f };

    // status
    constexpr ImVec4 green  {  72/255.f, 199/255.f, 116/255.f, 1.f };
    constexpr ImVec4 red    { 235/255.f,  87/255.f,  87/255.f, 1.f };

    constexpr ImVec4 border     { 1.f, 1.f, 1.f, 0.07f };
    constexpr ImVec4 border_sel { 140/255.f, 60/255.f, 255/255.f, 0.70f };
}

static ImU32 col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
static ImU32 col_a(const ImVec4& c, float a)
{
    ImVec4 t = c; t.w = a; return ImGui::ColorConvertFloat4ToU32(t);
}

// ============================================================================
//  D3D11 / window globals
// ============================================================================
static ID3D11Device*           g_dev        = nullptr;
static ID3D11DeviceContext*    g_ctx        = nullptr;
static IDXGISwapChain*         g_sc         = nullptr;
static ID3D11RenderTargetView* g_rtv        = nullptr;
static HWND                    g_hwnd       = nullptr;
static bool                    g_running    = true;
static bool                    g_dragging   = false;
static POINT                   g_drag_off   = {};

static constexpr int WW = 900, WH = 580;

static bool create_device(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL fl_out{};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, fl, 1, D3D11_SDK_VERSION,
            &sd, &g_sc, &g_dev, &fl_out, &g_ctx)))
        return false;
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
    return g_rtv != nullptr;
}
static void cleanup_device()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_sc)  { g_sc->Release();  g_sc  = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}
static void rebuild_rtv(UINT w, UINT h)
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
}

// ============================================================================
//  Path / process helpers
// ============================================================================
static std::wstring exe_dir()
{
    wchar_t b[MAX_PATH]{};
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    return std::filesystem::path(b).parent_path().wstring();
}
static std::wstring path_of(std::wstring_view rel)
{
    return (std::filesystem::path(exe_dir()) / rel).wstring();
}
static bool file_exists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool run_wait(const std::wstring& exe, const std::wstring& args, DWORD& ec)
{
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}
static bool run_detached(const std::wstring& exe)
{
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}

// ============================================================================
//  State machine
// ============================================================================
enum class Step { Mode, Inject, Done };

static Step              g_step    = Step::Mode;
static bool              g_km_mode = false;   // false = UM (default, matches image)
static float             g_spin    = 0.f;
static std::atomic<bool> g_launch_done{ false };
static bool              g_launch_ok  = false;
static std::string       g_launch_msg;
static std::vector<std::string> g_log_lines;

static void push_log(const std::string& s)
{
    g_log_lines.push_back(s);
}

static void do_launch_thread(bool km)
{
    const std::wstring kdmapper = path_of(L"kdmapper_Release.exe");
    const std::wstring driver   = km
        ? path_of(L"..\\driver\\x64\\Release\\N4TEYW4RE.Driver.sys")
        : path_of(L"km.sys");
    const std::wstring payload  = km
        ? path_of(L"..\\build\\release\\bin\\n4teyw4re.exe")
        : path_of(L"um.exe");

    struct F { const char* label; const std::wstring& p; };
    F files[3] = { {"kdmapper", kdmapper}, {"driver", driver}, {"payload", payload} };
    for (auto& f : files)
    {
        if (!file_exists(f.p))
        {
            g_launch_msg = std::string("File not found: ") + f.label;
            g_launch_ok  = false;
            g_launch_done = true;
            return;
        }
    }

    push_log("[ >> ] Validating files...");
    Sleep(300);
    push_log("[ OK ] Files verified.");
    push_log("[ >> ] Mapping driver via kdmapper...");

    DWORD ec = 0;
    if (!run_wait(kdmapper, L"\"" + driver + L"\"", ec) || ec != 0)
    {
        push_log("[ !! ] kdmapper failed (code " + std::to_string(ec) + ")");
        g_launch_msg = "kdmapper failed with exit code " + std::to_string(ec);
        g_launch_ok  = false;
        g_launch_done = true;
        return;
    }

    push_log("[ OK ] Driver mapped successfully.");
    push_log("[ >> ] Waiting for driver initialisation...");
    Sleep(1200);
    push_log("[ OK ] Driver ready.");
    push_log("[ >> ] Launching payload...");

    if (!run_detached(payload))
    {
        push_log("[ !! ] Failed to start payload.");
        g_launch_msg = "Failed to launch payload executable.";
        g_launch_ok  = false;
        g_launch_done = true;
        return;
    }

    push_log("[ OK ] " + std::string(km ? "n4teyw4re.exe" : "um.exe") + " launched.");
    g_launch_msg = km ? "n4teyw4re launched successfully." : "UM client launched successfully.";
    g_launch_ok  = true;
    g_launch_done = true;
}

// ============================================================================
//  Draw helpers
// ============================================================================

// Scattered decorative + crosses in the background
static void draw_grid(ImDrawList* dl, ImVec2 p0, ImVec2 p1)
{
    // Fixed positions (fraction of window) — same layout as the reference image
    static const float pts[][2] = {
        {0.13f,0.12f},{0.38f,0.05f},{0.63f,0.05f},{0.87f,0.12f},
        {0.05f,0.50f},{0.95f,0.50f},
        {0.13f,0.88f},{0.38f,0.95f},{0.63f,0.95f},{0.87f,0.88f},
        {0.50f,0.50f},
    };
    const float W = p1.x - p0.x, H = p1.y - p0.y;
    const ImU32 c = col_a(pal::purple, 0.20f);
    for (auto& pt : pts)
    {
        const float x = p0.x + pt[0] * W;
        const float y = p0.y + pt[1] * H;
        constexpr float s = 5.f;
        dl->AddLine(ImVec2{x - s, y}, ImVec2{x + s, y}, c, 1.f);
        dl->AddLine(ImVec2{x, y - s}, ImVec2{x, y + s}, c, 1.f);
    }
}

// Monitor icon (UM)
static void draw_icon_monitor(ImDrawList* dl, ImVec2 c, float sz, ImU32 clr)
{
    const float hw = sz * 0.55f, hh = sz * 0.40f, r = sz * 0.08f;
    dl->AddRect(ImVec2{c.x - hw, c.y - hh}, ImVec2{c.x + hw, c.y + hh * 0.55f},
                clr, r, 0, 1.8f);
    // stand
    dl->AddLine(ImVec2{c.x, c.y + hh * 0.55f}, ImVec2{c.x, c.y + hh}, clr, 1.6f);
    dl->AddLine(ImVec2{c.x - hw * 0.4f, c.y + hh}, ImVec2{c.x + hw * 0.4f, c.y + hh}, clr, 1.6f);
}

// Chip icon (KM)
static void draw_icon_chip(ImDrawList* dl, ImVec2 c, float sz, ImU32 clr)
{
    const float h = sz * 0.42f;
    dl->AddRect(ImVec2{c.x - h, c.y - h}, ImVec2{c.x + h, c.y + h}, clr, sz * 0.07f, 0, 1.8f);
    // inner rect
    dl->AddRect(ImVec2{c.x - h * 0.5f, c.y - h * 0.5f},
                ImVec2{c.x + h * 0.5f, c.y + h * 0.5f}, clr, 2.f, 0, 1.2f);
    // pins — 3 per side
    const float pin_gap = h * 0.35f, pin_len = h * 0.30f;
    for (int i = -1; i <= 1; ++i)
    {
        const float off = i * pin_gap;
        dl->AddLine(ImVec2{c.x + off, c.y - h}, ImVec2{c.x + off, c.y - h - pin_len}, clr, 1.5f);
        dl->AddLine(ImVec2{c.x + off, c.y + h}, ImVec2{c.x + off, c.y + h + pin_len}, clr, 1.5f);
        dl->AddLine(ImVec2{c.x - h, c.y + off}, ImVec2{c.x - h - pin_len, c.y + off}, clr, 1.5f);
        dl->AddLine(ImVec2{c.x + h, c.y + off}, ImVec2{c.x + h + pin_len, c.y + off}, clr, 1.5f);
    }
}

// Checkmark badge (top-right of selected card)
static void draw_check_badge(ImDrawList* dl, ImVec2 pos, float r, bool filled)
{
    if (filled)
    {
        dl->AddCircleFilled(pos, r, col(pal::purple));
        dl->AddCircle(pos, r, col(pal::purple), 0, 1.3f);
        ImVec2 tick[3] = {
            ImVec2{pos.x - r * 0.45f, pos.y},
            ImVec2{pos.x - r * 0.10f, pos.y + r * 0.42f},
            ImVec2{pos.x + r * 0.50f, pos.y - r * 0.40f}
        };
        dl->AddPolyline(tick, 3, IM_COL32_WHITE, 0, 1.6f);
    }
    else
    {
        dl->AddCircle(pos, r, col_a(pal::purple, 0.35f), 0, 1.3f);
    }
}

// Spinner arc
static void draw_spinner(ImDrawList* dl, ImVec2 c, float r, float t, ImU32 clr)
{
    constexpr int   N   = 48;
    constexpr float ARC = 1.6f;
    for (int i = 0; i < N; ++i)
    {
        const float a0 = t + ARC * (float)i / N;
        const float a1 = t + ARC * (float)(i + 1) / N;
        const ImU32 ic = col_a(pal::purple, 0.12f + 0.88f * (float)i / N);
        dl->AddLine(
            ImVec2{c.x + cosf(a0) * r, c.y + sinf(a0) * r},
            ImVec2{c.x + cosf(a1) * r, c.y + sinf(a1) * r},
            ic, 2.8f);
    }
    (void)clr;
}

// Spaced-out letter rendering (for "SELECT EXECUTION MODE" style headers)
// We approximate letter-spacing by inserting thin invisible items.
static void spaced_text(const char* txt, float spacing, const ImVec4& color)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    const char* p = txt;
    while (*p)
    {
        char buf[2] = { *p, 0 };
        ImGui::TextUnformatted(buf);
        ImGui::SameLine(0.f, spacing);
        ++p;
    }
    // end the SameLine chain
    ImGui::NewLine();
    ImGui::PopStyleColor();
}

// ============================================================================
//  Sidebar
// ============================================================================
static void draw_sidebar(ImVec2 origin, float w, float h)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // sidebar background
    dl->AddRectFilled(origin, ImVec2{origin.x + w, origin.y + h}, col(pal::sidebar));
    // right border
    dl->AddLine(ImVec2{origin.x + w, origin.y},
                ImVec2{origin.x + w, origin.y + h},
                col_a(pal::purple, 0.15f), 1.f);

    // brand
    ImGui::SetCursorScreenPos(ImVec2{origin.x + 18.f, origin.y + 22.f});
    ImGui::PushStyleColor(ImGuiCol_Text, pal::text_hi);
    ImGui::SetWindowFontScale(1.15f);
    ImGui::TextUnformatted("N4TEY");
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2{origin.x + 18.f, origin.y + 42.f});
    ImGui::PushStyleColor(ImGuiCol_Text, pal::text_lo);
    ImGui::TextUnformatted("CS2 EXTERNAL LOADER");
    ImGui::PopStyleColor();

    // steps
    struct SideStep { const char* title; const char* sub; Step step; };
    const SideStep steps[] = {
        { "MODE",   "Select execution method", Step::Mode   },
        { "CONFIG", "Load or create config.",  Step::Mode   },  // future
        { "INJECT", "Start execution ...",     Step::Inject },
        { "DONE",   "You're ready.",           Step::Done   },
    };

    float sy = origin.y + 110.f;
    for (int i = 0; i < 4; ++i)
    {
        const bool active  = (i == 0 && g_step == Step::Mode)   ||
                             (i == 2 && g_step == Step::Inject)  ||
                             (i == 3 && g_step == Step::Done);
        const bool reached = (i == 0) ||
                             (i == 2 && g_step >= Step::Inject) ||
                             (i == 3 && g_step == Step::Done);

        // dot
        const ImVec2 dot = ImVec2{origin.x + 22.f, sy + 7.f};
        if (active)
        {
            dl->AddCircleFilled(dot, 5.f, col(pal::purple));
            // left accent bar
            dl->AddRectFilled(ImVec2{origin.x, sy - 2.f},
                              ImVec2{origin.x + 3.f, sy + 22.f},
                              col(pal::purple));
        }
        else
        {
            dl->AddCircle(dot, 4.f,
                reached ? col_a(pal::purple, 0.55f) : col_a(pal::purple, 0.22f), 0, 1.2f);
        }

        ImGui::SetCursorScreenPos(ImVec2{origin.x + 34.f, sy - 1.f});
        ImGui::PushStyleColor(ImGuiCol_Text, active ? pal::text_hi : pal::text_lo);
        ImGui::TextUnformatted(steps[i].title);
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2{origin.x + 34.f, sy + 14.f});
        ImGui::PushStyleColor(ImGuiCol_Text, pal::text_lo);
        ImGui::TextUnformatted(steps[i].sub);
        ImGui::PopStyleColor();

        sy += 58.f;
    }

    // version footer
    ImGui::SetCursorScreenPos(ImVec2{origin.x + 16.f, origin.y + h - 44.f});
    ImGui::PushStyleColor(ImGuiCol_Text, pal::text_lo);
    ImGui::TextUnformatted("VERSION 1.0.0");
    ImGui::SetCursorScreenPos(ImVec2{origin.x + 16.f, origin.y + h - 28.f});
    ImGui::TextUnformatted("PRIVATE BUILD");
    ImGui::PopStyleColor();
}

// ============================================================================
//  MODE page
// ============================================================================
static void draw_mode_page(ImVec2 origin, float w, float h)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // top label
    {
        const char* top = "COUNTER STRIKE 2";
        const ImVec2 tsz = ImGui::CalcTextSize(top);
        ImGui::SetCursorScreenPos(ImVec2{origin.x + (w - tsz.x) * 0.5f, origin.y + 28.f});
        ImGui::PushStyleColor(ImGuiCol_Text, pal::text_lo);
        ImGui::TextUnformatted(top);
        ImGui::PopStyleColor();
    }

    // big title — hand-spaced
    {
        const char* title = "SELECT EXECUTION MODE";
        // Measure total width with spacing
        constexpr float sp = 4.5f;
        float tw = 0.f;
        for (const char* p = title; *p; ++p)
        {
            char b[2] = { *p, 0 };
            tw += ImGui::CalcTextSize(b).x + sp;
        }
        ImGui::SetCursorScreenPos(ImVec2{origin.x + (w - tw) * 0.5f, origin.y + 50.f});
        ImGui::PushStyleColor(ImGuiCol_Text, pal::text_hi);
        ImGui::SetWindowFontScale(1.38f);
        spaced_text(title, sp, pal::text_hi);
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopStyleColor();
    }

    // subtitle
    {
        const char* sub = "Choose how you want to run the external.";
        const ImVec2 ssz = ImGui::CalcTextSize(sub);
        ImGui::SetCursorScreenPos(ImVec2{origin.x + (w - ssz.x) * 0.5f, origin.y + 92.f});
        ImGui::PushStyleColor(ImGuiCol_Text, pal::text_mid);
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
    }

    // top-right tagline
    {
        ImGui::SetCursorScreenPos(ImVec2{origin.x + w - 130.f, origin.y + 24.f});
        ImGui::PushStyleColor(ImGuiCol_Text, pal::text_lo);
        ImGui::TextUnformatted("BUILT DIFFERENT");
        ImGui::SetCursorScreenPos(ImVec2{origin.x + w - 130.f, origin.y + 38.f});
        ImGui::TextUnformatted("FOR A HIGHER EDGE");
        ImGui::PopStyleColor();
        // short accent line beneath
        const float lx = origin.x + w - 130.f;
        const float ly = origin.y + 52.f;
        dl->AddLine(ImVec2{lx, ly}, ImVec2{lx + 60.f, ly}, col(pal::purple), 1.f);
    }

    // ---- cards ----
    constexpr float CARD_W = 240.f, CARD_H = 260.f;
    const float card_y = origin.y + 118.f;
    const float total_cards = CARD_W * 2.f + 24.f;
    const float card_x0 = origin.x + (w - total_cards) * 0.5f;
    const float card_x1 = card_x0 + CARD_W + 24.f;

    // Helper — draws one card, returns true if clicked
    auto draw_card = [&](float cx, bool is_um) -> bool
    {
        const bool selected = (is_um ? !g_km_mode : g_km_mode);
        const ImVec2 p0 = ImVec2{cx, card_y};
        const ImVec2 p1 = ImVec2{cx + CARD_W, card_y + CARD_H};

        const bool hov = ImGui::IsMouseHoveringRect(p0, p1);
        const bool clk = hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        // glow behind selected card
        if (selected)
        {
            for (int g = 8; g >= 1; --g)
            {
                const float expand = (float)g * 2.5f;
                dl->AddRectFilled(
                    ImVec2{p0.x - expand, p0.y - expand},
                    ImVec2{p1.x + expand, p1.y + expand},
                    col_a(pal::purple, 0.025f * (9 - g)), 16.f);
            }
        }

        // card fill
        const ImU32 fill = selected
            ? col(pal::card_sel)
            : (hov ? col_a(pal::card_sel, 0.5f) : col(pal::card));
        dl->AddRectFilled(p0, p1, fill, 12.f);

        // border
        const ImU32 bord = selected
            ? col(pal::border_sel)
            : (hov ? col_a(pal::border_sel, 0.35f) : col(pal::border));
        dl->AddRect(p0, p1, bord, 12.f, 0, 1.4f);

        // icon
        const ImVec2 icon_c = ImVec2{cx + CARD_W * 0.5f, card_y + 68.f};
        const ImU32  icon_c32 = selected ? col(pal::purple) : col_a(pal::purple, 0.55f);
        if (is_um) draw_icon_monitor(dl, icon_c, 42.f, icon_c32);
        else       draw_icon_chip   (dl, icon_c, 42.f, icon_c32);

        // checkmark badge
        draw_check_badge(dl, ImVec2{p1.x - 22.f, p0.y + 22.f}, 11.f, selected);

        // card title
        const char* title = is_um ? "USERMODE" : "KERNEL MODE";
        constexpr float tsp = 3.5f;
        float tw = 0.f;
        for (const char* p = title; *p; ++p)
        {
            char b[2] = {*p, 0};
            tw += ImGui::CalcTextSize(b).x + tsp;
        }
        ImGui::SetCursorScreenPos(ImVec2{cx + (CARD_W - tw) * 0.5f, card_y + 118.f});
        ImGui::SetWindowFontScale(1.05f);
        spaced_text(title, tsp, selected ? pal::text_hi : pal::text_mid);
        ImGui::SetWindowFontScale(1.f);

        // tagline
        const char* tag = is_um ? "FAST. STEALTHY. STABLE." : "DEEP ACCESS. MAXIMUM CONTROL.";
        const ImVec2 tgsz = ImGui::CalcTextSize(tag);
        ImGui::SetCursorScreenPos(ImVec2{cx + (CARD_W - tgsz.x) * 0.5f, card_y + 142.f});
        ImGui::PushStyleColor(ImGuiCol_Text, pal::text_lo);
        ImGui::TextUnformatted(tag);
        ImGui::PopStyleColor();

        // divider
        dl->AddLine(ImVec2{p0.x + 16.f, card_y + 162.f},
                    ImVec2{p1.x - 16.f, card_y + 162.f},
                    col_a(pal::purple, 0.18f), 1.f);

        // bullet features
        const char* feats_um[] = {
            "Lower detection risk",
            "Faster injection",
            "Recommended for most users"
        };
        const char* feats_km[] = {
            "Higher privilege access",
            "Advanced functionality",
            "Use only if you know what you're doing"
        };
        const char** feats = is_um ? feats_um : feats_km;
        for (int i = 0; i < 3; ++i)
        {
            const float fy = card_y + 176.f + i * 24.f;
            dl->AddCircleFilled(ImVec2{cx + 22.f, fy + 7.f}, 3.5f,
                selected ? col(pal::purple) : col_a(pal::purple, 0.45f));
            ImGui::SetCursorScreenPos(ImVec2{cx + 34.f, fy});
            ImGui::PushStyleColor(ImGuiCol_Text,
                selected ? pal::text_mid : pal::text_lo);
            ImGui::TextUnformatted(feats[i]);
            ImGui::PopStyleColor();
        }

        // invisible hit region
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton(is_um ? "##card_um" : "##card_km",
                               ImVec2{CARD_W, CARD_H});
        return clk;
    };

    if (draw_card(card_x0, true))  g_km_mode = false;  // UM
    if (draw_card(card_x1, false)) g_km_mode = true;   // KM

    // ---- CONTINUE button ----
    constexpr float BTN_W = 220.f, BTN_H = 46.f;
    const float btn_x = origin.x + (w - BTN_W) * 0.5f;
    const float btn_y = origin.y + h - 80.f;

    const ImVec2 bp0 = ImVec2{btn_x, btn_y};
    const ImVec2 bp1 = ImVec2{btn_x + BTN_W, btn_y + BTN_H};
    const bool   bhov = ImGui::IsMouseHoveringRect(bp0, bp1);
    const bool   bclk = bhov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    // glow
    if (bhov)
        dl->AddRectFilled(ImVec2{bp0.x - 4.f, bp0.y - 4.f},
                          ImVec2{bp1.x + 4.f, bp1.y + 4.f},
                          col_a(pal::purple, 0.12f), 28.f);
    dl->AddRectFilled(bp0, bp1,
        bhov ? col(pal::purple_dim) : col_a(pal::purple_dim, 0.55f), 24.f);
    dl->AddRect(bp0, bp1,
        bhov ? col(pal::purple) : col_a(pal::purple, 0.55f), 24.f, 0, 1.4f);

    // label + arrow
    const char* btn_txt = "CONTINUE";
    float btw = 0.f;
    for (const char* p = btn_txt; *p; ++p)
    {
        char b[2] = {*p, 0};
        btw += ImGui::CalcTextSize(b).x + 3.5f;
    }
    btw += 20.f; // arrow
    ImGui::SetCursorScreenPos(ImVec2{bp0.x + (BTN_W - btw) * 0.5f,
                                     bp0.y + (BTN_H - ImGui::GetTextLineHeight()) * 0.5f});
    spaced_text("CONTINUE", 3.5f, bhov ? pal::text_hi : pal::text_mid);
    // arrow — drawn after, same line row
    dl->AddLine(ImVec2{bp0.x + (BTN_W + btw) * 0.5f - 12.f,
                       btn_y + BTN_H * 0.5f},
                ImVec2{bp0.x + (BTN_W + btw) * 0.5f,
                       btn_y + BTN_H * 0.5f},
                bhov ? col(pal::purple) : col_a(pal::purple, 0.6f), 1.5f);

    // hit area
    ImGui::SetCursorScreenPos(bp0);
    ImGui::InvisibleButton("##continue", ImVec2{BTN_W, BTN_H});

    // bottom tagline
    ImGui::SetCursorScreenPos(ImVec2{origin.x + w - 140.f, origin.y + h - 40.f});
    ImGui::PushStyleColor(ImGuiCol_Text, pal::text_lo);
    ImGui::TextUnformatted("SAME GAME.");
    ImGui::SetCursorScreenPos(ImVec2{origin.x + w - 140.f, origin.y + h - 24.f});
    ImGui::TextUnformatted("DIFFERENT OUTCOMES.");
    ImGui::PopStyleColor();
    dl->AddLine(ImVec2{origin.x + w - 140.f, origin.y + h - 10.f},
                ImVec2{origin.x + w - 80.f,  origin.y + h - 10.f},
                col(pal::purple), 1.f);

    if (bclk)
    {
        g_step       = Step::Inject;
        g_spin       = 0.f;
        g_log_lines.clear();
        g_launch_done = false;
        std::thread(do_launch_thread, g_km_mode).detach();
    }
}

// ============================================================================
//  INJECT page
// ============================================================================
static void draw_inject_page(ImVec2 origin, float w, float h)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // title
    {
        const char* t = "INITIALISING";
        float tw = 0.f;
        for (const char* p = t; *p; ++p) { char b[2]={*p,0}; tw += ImGui::CalcTextSize(b).x + 4.f; }
        ImGui::SetCursorScreenPos(ImVec2{origin.x + (w - tw) * 0.5f, origin.y + 38.f});
        ImGui::SetWindowFontScale(1.3f);
        spaced_text(t, 4.f, pal::text_hi);
        ImGui::SetWindowFontScale(1.f);
    }

    const char* sub = g_km_mode ? "Mapping driver and launching n4teyw4re..."
                                 : "Mapping driver and launching um.exe...";
    {
        const ImVec2 ssz = ImGui::CalcTextSize(sub);
        ImGui::SetCursorScreenPos(ImVec2{origin.x + (w - ssz.x) * 0.5f, origin.y + 76.f});
        ImGui::PushStyleColor(ImGuiCol_Text, pal::text_mid);
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
    }

    // spinner
    g_spin += ImGui::GetIO().DeltaTime * 2.8f;
    const ImVec2 sc = ImVec2{origin.x + w * 0.5f, origin.y + 160.f};
    draw_spinner(dl, sc, 28.f, g_spin, 0);

    // log lines box
    const ImVec2 lb0 = ImVec2{origin.x + 40.f, origin.y + 210.f};
    const ImVec2 lb1 = ImVec2{origin.x + w - 40.f, origin.y + h - 60.f};
    dl->AddRectFilled(lb0, lb1, col(pal::card), 6.f);
    dl->AddRect(lb0, lb1, col_a(pal::purple, 0.20f), 6.f, 0, 1.f);

    float ly = lb0.y + 10.f;
    const auto& lines = g_log_lines;
    // show last N lines that fit
    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    const int max_lines = (int)((lb1.y - lb0.y - 20.f) / line_h);
    int start = (int)lines.size() > max_lines ? (int)lines.size() - max_lines : 0;
    for (int i = start; i < (int)lines.size(); ++i)
    {
        ImGui::SetCursorScreenPos(ImVec2{lb0.x + 12.f, ly});
        // colour by prefix
        ImVec4 lc = pal::text_mid;
        if (lines[i].find("[ OK ]") != std::string::npos) lc = pal::green;
        else if (lines[i].find("[ !! ]") != std::string::npos) lc = pal::red;
        else if (lines[i].find("[ >> ]") != std::string::npos) lc = pal::purple;
        ImGui::PushStyleColor(ImGuiCol_Text, lc);
        ImGui::TextUnformatted(lines[i].c_str());
        ImGui::PopStyleColor();
        ly += line_h;
    }

    // check completion
    if (g_launch_done.load())
        g_step = Step::Done;
}

// ============================================================================
//  DONE page
// ============================================================================
static void draw_done_page(ImVec2 origin, float w, float h)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float cx = origin.x + w * 0.5f;
    const float cy = origin.y + h * 0.5f - 50.f;

    // ring
    if (g_launch_ok)
    {
        dl->AddCircleFilled(ImVec2{cx, cy}, 40.f, col_a(pal::green, 0.10f));
        dl->AddCircle      (ImVec2{cx, cy}, 40.f, col(pal::green), 0, 1.8f);
        ImVec2 tick[3] = {
            ImVec2{cx - 14.f, cy},
            ImVec2{cx - 4.f,  cy + 11.f},
            ImVec2{cx + 16.f, cy - 13.f}
        };
        dl->AddPolyline(tick, 3, col(pal::green), 0, 2.5f);
    }
    else
    {
        dl->AddCircleFilled(ImVec2{cx, cy}, 40.f, col_a(pal::red, 0.10f));
        dl->AddCircle      (ImVec2{cx, cy}, 40.f, col(pal::red), 0, 1.8f);
        dl->AddLine(ImVec2{cx-13.f,cy-13.f},ImVec2{cx+13.f,cy+13.f},col(pal::red),2.5f);
        dl->AddLine(ImVec2{cx+13.f,cy-13.f},ImVec2{cx-13.f,cy+13.f},col(pal::red),2.5f);
    }

    const char* status = g_launch_ok ? "SUCCESS" : "FAILED";
    float sw = 0.f;
    for (const char* p = status; *p; ++p) { char b[2]={*p,0}; sw += ImGui::CalcTextSize(b).x + 4.f; }
    ImGui::SetCursorScreenPos(ImVec2{cx - sw * 0.5f, cy + 52.f});
    ImGui::SetWindowFontScale(1.2f);
    spaced_text(status, 4.f, g_launch_ok ? pal::green : pal::red);
    ImGui::SetWindowFontScale(1.f);

    const ImVec2 msz = ImGui::CalcTextSize(g_launch_msg.c_str());
    ImGui::SetCursorScreenPos(ImVec2{cx - msz.x * 0.5f, cy + 82.f});
    ImGui::PushStyleColor(ImGuiCol_Text, pal::text_mid);
    ImGui::TextUnformatted(g_launch_msg.c_str());
    ImGui::PopStyleColor();

    // close button
    constexpr float BW = 160.f, BH = 40.f;
    const ImVec2 bp0 = ImVec2{cx - BW * 0.5f, cy + 120.f};
    const ImVec2 bp1 = ImVec2{cx + BW * 0.5f, cy + 120.f + BH};
    const bool bhov  = ImGui::IsMouseHoveringRect(bp0, bp1);
    const bool bclk  = bhov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    dl->AddRectFilled(bp0, bp1, bhov ? col(pal::purple_dim) : col_a(pal::purple_dim, 0.4f), 20.f);
    dl->AddRect      (bp0, bp1, bhov ? col(pal::purple) : col_a(pal::purple, 0.4f), 20.f, 0, 1.2f);

    const char* btnlbl = g_launch_ok ? "CLOSE" : "BACK";
    const ImVec2 blsz  = ImGui::CalcTextSize(btnlbl);
    ImGui::SetCursorScreenPos(ImVec2{bp0.x + (BW - blsz.x) * 0.5f,
                                     bp0.y + (BH - blsz.y) * 0.5f});
    ImGui::PushStyleColor(ImGuiCol_Text, bhov ? pal::text_hi : pal::text_mid);
    ImGui::TextUnformatted(btnlbl);
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(bp0);
    ImGui::InvisibleButton("##done_btn", ImVec2{BW, BH});

    if (bclk)
    {
        if (g_launch_ok) g_running = false;
        else             g_step = Step::Mode;
    }
}

// ============================================================================
//  Top-right chrome buttons (−, □, ×)
// ============================================================================
static void draw_chrome(ImVec2 win_min, float win_w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float R = 10.f, GAP = 24.f;
    const float y = win_min.y + 18.f;
    const float x3 = win_min.x + win_w - 22.f;
    const float x2 = x3 - GAP;
    const float x1 = x2 - GAP;

    auto chrome_btn = [&](float cx, char sym) -> bool
    {
        const ImVec2 p0 = ImVec2{cx - R, y - R};
        const ImVec2 p1 = ImVec2{cx + R, y + R};
        const bool hov  = ImGui::IsMouseHoveringRect(p0, p1);
        const bool clk  = hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (hov) dl->AddCircleFilled(ImVec2{cx, y}, R, col_a(pal::purple, 0.25f));
        const ImU32 lc = hov ? col(pal::text_hi) : col(pal::text_lo);
        if (sym == '-')
            dl->AddLine(ImVec2{cx - 5.f, y}, ImVec2{cx + 5.f, y}, lc, 1.4f);
        else if (sym == 'o')
            dl->AddRect(ImVec2{cx - 5.f, y - 4.f}, ImVec2{cx + 5.f, y + 4.f}, lc, 1.f, 0, 1.2f);
        else // x
        {
            dl->AddLine(ImVec2{cx - 5.f, y - 4.f}, ImVec2{cx + 5.f, y + 4.f}, lc, 1.4f);
            dl->AddLine(ImVec2{cx + 5.f, y - 4.f}, ImVec2{cx - 5.f, y + 4.f}, lc, 1.4f);
        }
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton(&sym, ImVec2{R * 2.f, R * 2.f});
        return clk;
    };

    if (chrome_btn(x1, '-')) ShowWindow(g_hwnd, SW_MINIMIZE);
    if (chrome_btn(x2, 'o')) {} // maximise: no-op for fixed-size launcher
    if (chrome_btn(x3, 'x')) g_running = false;
}

// ============================================================================
//  Root render
// ============================================================================
static void render_ui()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2{0.f, 0.f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, pal::bg);
    ImGui::PushStyleColor(ImGuiCol_Border,   pal::border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2{0.f, 0.f});

    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoResize    |
        ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar   | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl       = ImGui::GetWindowDrawList();
    const ImVec2 win_min = ImGui::GetWindowPos();
    const float  W       = io.DisplaySize.x;
    const float  H       = io.DisplaySize.y;

    // window border
    dl->AddRect(win_min, ImVec2{win_min.x + W, win_min.y + H},
                col_a(pal::purple, 0.30f), 0.f, 0, 1.f);

    // grid dots
    draw_grid(dl, win_min, ImVec2{win_min.x + W, win_min.y + H});

    // sidebar
    constexpr float SIDE_W = 180.f;
    draw_sidebar(win_min, SIDE_W, H);

    // chrome
    draw_chrome(win_min, W);

    // content area
    const ImVec2 content_origin = ImVec2{win_min.x + SIDE_W, win_min.y};
    const float  content_w = W - SIDE_W;

    switch (g_step)
    {
    case Step::Mode:   draw_mode_page  (content_origin, content_w, H); break;
    case Step::Inject: draw_inject_page(content_origin, content_w, H); break;
    case Step::Done:   draw_done_page  (content_origin, content_w, H); break;
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ============================================================================
//  Win32 proc + drag
// ============================================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;

    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        // Start drag only if we click on the top non-interactive zone
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        if (pt.y < 40 && pt.x < WW - 80)
        {
            g_dragging = true;
            g_drag_off = pt;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragging)
        {
            POINT cur{};
            GetCursorPos(&cur);
            SetWindowPos(hwnd, nullptr,
                cur.x - g_drag_off.x, cur.y - g_drag_off.y,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) { g_dragging = false; ReleaseCapture(); }
        return 0;
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (g_dev && wp != SIZE_MINIMIZED)
            rebuild_rtv(LOWORD(lp), HIWORD(lp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
//  Admin elevation
// ============================================================================
static bool ensure_admin()
{
    HANDLE tok{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{}; DWORD ret{};
    GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &ret);
    CloseHandle(tok);
    if (e.TokenIsElevated) return true;

    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.nShow  = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
    return false;
}

// ============================================================================
//  WinMain
// ============================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    if (!ensure_admin()) return 0;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"n4teyw4re.launcher";
    RegisterClassExW(&wc);

    const int sx = GetSystemMetrics(SM_CXSCREEN);
    const int sy = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_LAYERED,
        wc.lpszClassName, L"N4TEYW4RE",
        WS_POPUP | WS_VISIBLE,
        (sx - WW) / 2, (sy - WH) / 2, WW, WH,
        nullptr, nullptr, hInst, nullptr);

    SetLayeredWindowAttributes(g_hwnd, 0, 250, LWA_ALPHA);

    if (!create_device(g_hwnd))
    {
        DestroyWindow(g_hwnd);
        UnregisterClassW(wc.lpszClassName, hInst);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding    = 0.f;
    st.FrameRounding     = 6.f;
    st.ItemSpacing       = ImVec2{8.f, 6.f};
    st.FramePadding      = ImVec2{8.f, 4.f};
    st.Colors[ImGuiCol_WindowBg] = pal::bg;
    st.Colors[ImGuiCol_Text]     = pal::text_hi;
    st.Colors[ImGuiCol_Border]   = pal::border;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    while (g_running)
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        render_ui();
        ImGui::Render();

        constexpr float clear[4]{ 10/255.f, 9/255.f, 18/255.f, 1.f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
