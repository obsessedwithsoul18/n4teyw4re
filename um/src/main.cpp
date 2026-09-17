/*
 * N4TEYW4RE — user-mode client
 *
 * Communicates with the companion kernel-mode driver (km/) to read and write
 * CS2 process memory without touching any game module from user space.
 *
 * Features
 * --------
 *  [INS]    Bunnyhop      — hold SPACE, jump fires the instant you land
 *  [DEL]    Triggerbot    — auto-fires when your crosshair is over an enemy
 *  [HOME]   Aimbot        — locks onto the nearest enemy head (FOV / smooth
 *                           tunable via AIMBOT_FOV / AIMBOT_SMOOTH below)
 *  [PGUP]   Rapid-fire    — removes the fire-rate cap via dwForceAttack
 *  [PGDN]   Auto-crouch   — holds crouch while left mouse is held
 *  [END]    Anti-recoil   — compensates m_aimPunchAngle every tick
 *  [F12]    Exit
 *
 * Build: x64, C++20, administrator required.
 */

#include <iostream>
#include <cmath>
#include <numbers>
#include <algorithm>
#include <chrono>
#include <thread>
#include <atomic>

#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>

#include "client.dll.hpp"
#include "offsets.hpp"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

static constexpr float AIMBOT_FOV      = 8.0f;   // degrees
static constexpr float AIMBOT_SMOOTH   = 0.12f;   // 1.0 = snap, lower = smooth
static constexpr int   TRIGGER_HOLD_MS = 20;       // ms to keep fire button down
static constexpr int   LOOP_SLEEP_MS   = 1;        // main loop delay

// ---------------------------------------------------------------------------
// Toggle keys
// ---------------------------------------------------------------------------

static constexpr int KEY_BHOP        = VK_INSERT;
static constexpr int KEY_TRIGGERBOT  = VK_DELETE;
static constexpr int KEY_AIMBOT      = VK_HOME;
static constexpr int KEY_RAPIDFIRE   = VK_PRIOR;  // Page Up
static constexpr int KEY_AUTOCROUCH  = VK_NEXT;   // Page Down
static constexpr int KEY_ANTIRECOIL  = VK_END;
static constexpr int KEY_EXIT        = VK_F12;

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

struct Vec3 {
    float x{}, y{}, z{};
    Vec3  operator-(const Vec3& o) const noexcept { return { x-o.x, y-o.y, z-o.z }; }
    float length2d()               const noexcept { return std::sqrtf(x*x + y*y); }
};

// CS2 view angle: pitch (up/down), yaw (left/right), roll (unused)
struct QAngle {
    float pitch{}, yaw{}, roll{};
};

static constexpr float k_deg = 180.0f / std::numbers::pi_v<float>;

static QAngle vec_to_angle(const Vec3& d) noexcept {
    return { -std::atan2f(d.z, d.length2d()) * k_deg,
              std::atan2f(d.y, d.x)           * k_deg,
              0.0f };
}

static float yaw_delta(float a, float b) noexcept {
    float d = a - b;
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

static float angle_fov(const QAngle& a, const QAngle& b) noexcept {
    const float dp = a.pitch - b.pitch;
    const float dy = yaw_delta(a.yaw, b.yaw);
    return std::sqrtf(dp*dp + dy*dy);
}

static void clamp_angle(QAngle& a) noexcept {
    a.pitch = std::clamp(a.pitch, -89.0f, 89.0f);
    while (a.yaw >  180.0f) a.yaw -= 360.0f;
    while (a.yaw < -180.0f) a.yaw += 360.0f;
    a.roll = 0.0f;
}

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------

static DWORD find_pid(const wchar_t* name) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e{ .dwSize = sizeof(e) };
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(name, e.szExeFile) == 0) { pid = e.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

static std::uintptr_t find_module(DWORD pid, const wchar_t* mod) {
    std::uintptr_t base = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W e{ .dwSize = sizeof(e) };
    for (BOOL ok = Module32FirstW(snap, &e); ok; ok = Module32NextW(snap, &e))
        if (wcsstr(mod, e.szModule)) { base = reinterpret_cast<std::uintptr_t>(e.modBaseAddr); break; }
    CloseHandle(snap);
    return base;
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

namespace drv {

    namespace code {
        constexpr ULONG attach = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x696, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
        constexpr ULONG read   = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x697, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
        constexpr ULONG write  = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x698, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
    }

    struct Req {
        HANDLE process_id{};
        PVOID  target{};
        PVOID  buffer{};
        SIZE_T size{};
        SIZE_T return_size{};
    };

    static HANDLE h = INVALID_HANDLE_VALUE;

    bool open() {
        h = CreateFileW(L"\\\\.\\kernel_drivers", GENERIC_READ, 0,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return h != INVALID_HANDLE_VALUE;
    }

    void close() {
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    }

    bool attach(DWORD pid) {
        Req r{ .process_id = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pid)) };
        return DeviceIoControl(h, code::attach, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr) != FALSE;
    }

    template<class T>
    T read(std::uintptr_t addr) {
        T v{};
        Req r{ .target = reinterpret_cast<PVOID>(addr), .buffer = &v, .size = sizeof(T) };
        DeviceIoControl(h, code::read, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);
        return v;
    }

    template<class T>
    bool write(std::uintptr_t addr, const T& v) {
        Req r{ .target = reinterpret_cast<PVOID>(addr),
               .buffer = const_cast<PVOID>(static_cast<const void*>(&v)),
               .size   = sizeof(T) };
        return DeviceIoControl(h, code::write, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr) != FALSE;
    }

} // namespace drv

// ---------------------------------------------------------------------------
// Entity helpers
// ---------------------------------------------------------------------------

// CHandle → raw pointer via entity list.
// Entity list layout: list[chunk_index] → chunk; chunk[slot] → controller ptr.
// chunk_index = index >> 9,  slot = index & 0x1FF,  stride = 120 bytes.
static std::uintptr_t handle_to_ptr(std::uintptr_t elist, std::uint32_t handle) {
    if (handle == 0xFFFFFFFFu) return 0;
    const std::uint32_t idx = handle & 0x7FFF;
    if (idx == 0 || idx >= 0x200) return 0;
    const std::uintptr_t chunk = drv::read<std::uintptr_t>(elist + 8 * (idx >> 9) + 16);
    if (!chunk) return 0;
    return drv::read<std::uintptr_t>(chunk + 120 * (idx & 0x1FF));
}

// World-space origin via CGameSceneNode::m_vecAbsOrigin
static Vec3 pawn_origin(std::uintptr_t pawn) {
    const std::uintptr_t node = drv::read<std::uintptr_t>(pawn + C_BaseEntity::m_pGameSceneNode);
    return node ? drv::read<Vec3>(node + CGameSceneNode::m_vecAbsOrigin) : Vec3{};
}

// Approximate head (origin + ~75 z)
static Vec3 pawn_head(std::uintptr_t pawn) {
    Vec3 o = pawn_origin(pawn);
    o.z += 75.0f;
    return o;
}

// ---------------------------------------------------------------------------
// Feature toggles  (non-atomic bools are fine — single thread reads/writes)
// ---------------------------------------------------------------------------

static bool g_bhop       = false;
static bool g_trigger    = false;
static bool g_aimbot     = false;
static bool g_rapidfire  = false;
static bool g_autocrouch = false;
static bool g_antirecoil = false;

struct Toggle {
    int    vk;
    bool&  flag;
    bool   prev  = false;
    const char* label;

    void poll() {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !prev) {
            flag = !flag;
            std::cout << "  [" << label << "] " << (flag ? "ON" : "OFF") << '\n';
        }
        prev = down;
    }
};

// ---------------------------------------------------------------------------
// Mouse movement for aimbot (SendInput — no driver needed)
// ---------------------------------------------------------------------------

static void move_mouse(int dx, int dy) {
    if (!dx && !dy) return;
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dx      = static_cast<LONG>(dx);
    in.mi.dy      = static_cast<LONG>(dy);
    SendInput(1, &in, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// Bunnyhop
// ---------------------------------------------------------------------------

static void do_bhop(std::uintptr_t client, std::uintptr_t pawn) {
    if (!g_bhop || !pawn) return;
    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) return;

    const std::uint32_t flags = drv::read<std::uint32_t>(pawn + C_BaseEntity::m_fFlags);
    const DWORD force         = drv::read<DWORD>(client + client_dll::dwForceJump);

    if (flags & 1u)              // on ground — inject jump
        drv::write<DWORD>(client + client_dll::dwForceJump, 65537);
    else if (force == 65537)     // left ground — release
        drv::write<DWORD>(client + client_dll::dwForceJump, 256);
}

// ---------------------------------------------------------------------------
// Triggerbot
// ---------------------------------------------------------------------------

static void do_triggerbot(std::uintptr_t client, std::uintptr_t pawn, int my_team) {
    if (!g_trigger || !pawn) return;

    // m_iIDEntIndex: raw entity index currently under the crosshair (0 = none)
    const std::int32_t xhair = drv::read<std::int32_t>(pawn + C_CSPlayerPawnBase::m_iIDEntIndex);
    if (xhair <= 0) return;

    const std::uintptr_t elist = drv::read<std::uintptr_t>(client + client_dll::dwEntityList);
    if (!elist) return;

    // xhair is a direct entity list index (not a CHandle)
    const std::uintptr_t chunk = drv::read<std::uintptr_t>(elist + 8 * (xhair >> 9) + 16);
    if (!chunk) return;
    const std::uintptr_t ctrl  = drv::read<std::uintptr_t>(chunk + 120 * (xhair & 0x1FF));
    if (!ctrl) return;

    if (!drv::read<bool>(ctrl + CCSPlayerController::m_bPawnIsAlive)) return;

    const std::uint32_t h_pawn = drv::read<std::uint32_t>(ctrl + CCSPlayerController::m_hPlayerPawn);
    const std::uintptr_t tpawn = handle_to_ptr(elist, h_pawn);
    if (!tpawn) return;

    const int team = drv::read<std::uint8_t>(tpawn + C_BaseEntity::m_iTeamNum);
    if (team == my_team) return;

    drv::write<DWORD>(client + client_dll::dwForceAttack, 65537);
    std::this_thread::sleep_for(std::chrono::milliseconds(TRIGGER_HOLD_MS));
    drv::write<DWORD>(client + client_dll::dwForceAttack, 256);
}

// ---------------------------------------------------------------------------
// Aimbot
// ---------------------------------------------------------------------------

static void do_aimbot(std::uintptr_t client, std::uintptr_t pawn, int my_team) {
    if (!g_aimbot || !pawn) return;

    Vec3 eye = pawn_origin(pawn);
    eye.z += 64.0f;   // approximate eye height

    const QAngle view = drv::read<QAngle>(client + client_dll::dwViewAngles);

    const std::uintptr_t elist = drv::read<std::uintptr_t>(client + client_dll::dwEntityList);
    if (!elist) return;

    // Read highest entity index from the GameEntitySystem
    const std::uintptr_t ges = drv::read<std::uintptr_t>(client + client_dll::dwGameEntitySystem);
    const std::uint32_t  max_ent = ges
        ? drv::read<std::uint32_t>(ges + client_dll::dwGameEntitySystem_getHighestEntityIndex)
        : 64u;
    const std::uint32_t limit = std::min(max_ent, static_cast<std::uint32_t>(64));

    std::uintptr_t best_pawn = 0;
    float          best_fov  = AIMBOT_FOV;
    Vec3           best_head {};

    for (std::uint32_t i = 1; i <= limit; ++i) {
        const std::uintptr_t chunk = drv::read<std::uintptr_t>(elist + 8 * (i >> 9) + 16);
        if (!chunk) continue;
        const std::uintptr_t ctrl = drv::read<std::uintptr_t>(chunk + 120 * (i & 0x1FF));
        if (!ctrl) continue;

        if (!drv::read<bool>(ctrl + CCSPlayerController::m_bPawnIsAlive)) continue;

        const std::uint32_t h_pawn = drv::read<std::uint32_t>(ctrl + CCSPlayerController::m_hPlayerPawn);
        const std::uintptr_t tpawn = handle_to_ptr(elist, h_pawn);
        if (!tpawn || tpawn == pawn) continue;

        if (drv::read<std::uint8_t>(tpawn + C_BaseEntity::m_iTeamNum) == my_team) continue;
        if (drv::read<std::int32_t>(tpawn + C_BaseEntity::m_iHealth) <= 0) continue;

        const Vec3  head    = pawn_head(tpawn);
        const Vec3  delta   = head - eye;
        const QAngle angle  = vec_to_angle(delta);
        const float  fov    = angle_fov(view, angle);

        if (fov < best_fov) { best_fov = fov; best_pawn = tpawn; best_head = head; }
    }

    if (!best_pawn) return;

    const QAngle target = vec_to_angle(best_head - eye);
    const float  dp     = target.pitch - view.pitch;
    const float  dy     = yaw_delta(target.yaw, view.yaw);

    // Scale degrees → mouse counts.
    // CS2 default: ~0.022 deg per count at sensitivity 1.0.
    constexpr float DEG_PER_COUNT = 0.022f;
    const int mx = static_cast<int>((dy * AIMBOT_SMOOTH) / DEG_PER_COUNT);
    const int my = static_cast<int>((dp * AIMBOT_SMOOTH) / DEG_PER_COUNT);
    move_mouse(mx, my);
}

// ---------------------------------------------------------------------------
// Rapid-fire
// ---------------------------------------------------------------------------

static void do_rapidfire(std::uintptr_t client) {
    if (!g_rapidfire) return;
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;
    drv::write<DWORD>(client + client_dll::dwForceAttack, 65537);
}

// ---------------------------------------------------------------------------
// Auto-crouch
// ---------------------------------------------------------------------------

static void do_autocrouch(std::uintptr_t client) {
    if (!g_autocrouch) return;
    const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    drv::write<DWORD>(client + client_dll::dwForceCrouch, lmb ? 65537 : 256);
}

// ---------------------------------------------------------------------------
// Anti-recoil
// ---------------------------------------------------------------------------

static void do_antirecoil(std::uintptr_t client, std::uintptr_t pawn) {
    if (!g_antirecoil || !pawn) return;
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;

    const std::int32_t shots = drv::read<std::int32_t>(pawn + C_CSPlayerPawnBase::m_iShotsFired);
    if (shots < 2) return;  // first shot has no pattern to compensate

    // Punch angle the engine applies (visual is 2× the networked value)
    const QAngle punch = drv::read<QAngle>(pawn + C_CSPlayerPawn::m_aimPunchAngle);

    QAngle view = drv::read<QAngle>(client + client_dll::dwViewAngles);
    view.pitch -= punch.pitch * 2.0f;
    view.yaw   -= punch.yaw   * 2.0f;
    clamp_angle(view);

    drv::write<QAngle>(client + client_dll::dwViewAngles, view);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::cout <<
        "N4TEYW4RE | UM client\n"
        "======================\n"
        "  INS   Bunnyhop\n"
        "  DEL   Triggerbot\n"
        "  HOME  Aimbot\n"
        "  PGUP  Rapid-fire\n"
        "  PGDN  Auto-crouch\n"
        "  END   Anti-recoil\n"
        "  F12   Exit\n"
        "======================\n\n";

    const DWORD pid = find_pid(L"cs2.exe");
    if (!pid) {
        std::cout << "[-] cs2.exe not found.\n";
        std::cin.get(); return 1;
    }
    std::cout << "[+] cs2 PID " << pid << '\n';

    if (!drv::open()) {
        std::cout << "[-] Could not open \\\\.\\kernel_drivers — is the driver loaded?\n";
        std::cin.get(); return 1;
    }
    std::cout << "[+] Driver opened.\n";

    if (!drv::attach(pid)) {
        std::cout << "[-] Attach failed.\n";
        drv::close(); std::cin.get(); return 1;
    }
    std::cout << "[+] Attached to cs2.\n";

    const std::uintptr_t client = find_module(pid, L"client.dll");
    if (!client) {
        std::cout << "[-] client.dll not found.\n";
        drv::close(); std::cin.get(); return 1;
    }
    std::cout << "[+] client.dll @ 0x" << std::hex << client << std::dec << "\n\n";
    std::cout << "Running — toggle features with the keys above.\n";

    Toggle toggles[] = {
        { KEY_BHOP,        g_bhop,       false, "Bunnyhop"    },
        { KEY_TRIGGERBOT,  g_trigger,    false, "Triggerbot"  },
        { KEY_AIMBOT,      g_aimbot,     false, "Aimbot"      },
        { KEY_RAPIDFIRE,   g_rapidfire,  false, "Rapid-fire"  },
        { KEY_AUTOCROUCH,  g_autocrouch, false, "Auto-crouch" },
        { KEY_ANTIRECOIL,  g_antirecoil, false, "Anti-recoil" },
    };

    bool running = true;
    while (running) {
        if (GetAsyncKeyState(KEY_EXIT) & 0x8000) {
            std::cout << "\n[*] F12 — exiting.\n";
            break;
        }

        for (auto& t : toggles) t.poll();

        const std::uintptr_t pawn =
            drv::read<std::uintptr_t>(client + client_dll::dwLocalPlayerPawn);

        const int my_team = pawn
            ? static_cast<int>(drv::read<std::uint8_t>(pawn + C_BaseEntity::m_iTeamNum))
            : 0;

        do_bhop       (client, pawn);
        do_triggerbot (client, pawn, my_team);
        do_aimbot     (client, pawn, my_team);
        do_rapidfire  (client);
        do_autocrouch (client);
        do_antirecoil (client, pawn);

        std::this_thread::sleep_for(std::chrono::milliseconds(LOOP_SLEEP_MS));
    }

    // restore force offsets to neutral before exit
    if (client) {
        drv::write<DWORD>(client + client_dll::dwForceJump,   256);
        drv::write<DWORD>(client + client_dll::dwForceAttack, 256);
        drv::write<DWORD>(client + client_dll::dwForceCrouch, 256);
    }

    drv::close();
    std::cout << "[+] Done.\n";
    std::cin.get();
    return 0;
}
