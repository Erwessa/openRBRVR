#include "API.hpp"
#include "Config.hpp"
#include "Dx.hpp"
#include "Globals.hpp"
#include "IPlugin.h"
#include "OpenXR.hpp"
#include "VR.hpp"
#include "openRBRVR.hpp"

#include <MinHook.h>

BOOL APIENTRY DllMain(HANDLE, DWORD, LPVOID)
{
    return TRUE;
}

extern "C" __declspec(dllexport) IPlugin* RBR_CreatePlugin(IRBRGame* game)
{
    if (!g::openrbrvr) {
        // Process-lifetime detours point into this DLL. Pin the module before
        // installing them so a host-side FreeLibrary cannot leave patched
        // targets jumping into unmapped code. This runs outside DllMain.
        static const DWORD modulePinError = []() -> DWORD {
            HMODULE module = nullptr;
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&RBR_CreatePlugin),
                    &module)) {
                return ERROR_SUCCESS;
            }
            return GetLastError();
        }();
        if (modulePinError != ERROR_SUCCESS) {
            dbg(std::format("Could not pin openRBRVR.dll (error {})", modulePinError));
            return nullptr;
        }

        // Initialize MinHook once, outside DllMain and before installing any hooks.
        static const MH_STATUS minHookStatus = MH_Initialize();
        if (minHookStatus != MH_OK) {
            MessageBoxA(nullptr, MH_StatusToString(minHookStatus), "MinHook initialization failed", MB_OK);
            return nullptr;
        }
        g::openrbrvr = new openRBRVR(game);
    }
    return g::openrbrvr;
}

extern "C" __declspec(dllexport) int64_t openRBRVR_Exec(ApiOperation ops, uint64_t value)
{
    dbg(std::format("Exec: {} {}", (uint64_t)ops, (uint64_t)value));

    if (ops == API_VERSION) {
        return 4;
    }
    if (ops & NOTIFY_VERTEX_SHADER) {
        auto shader = reinterpret_cast<IDirect3DVertexShader9*>(value);
        if (!dx::add_vertex_shader(shader)) {
            return -1;
        }
    }
    if (ops & MOVE_SEAT) {
        if (value > static_cast<uint64_t>(MOVE_SEAT_DOWN)) {
            return 1;
        }
        g::seat_movement_request.store(static_cast<SeatMovement>(value), std::memory_order_relaxed);
    }
    if (ops & RECENTER_VR_VIEW) {
        if (!g::vr) {
            return -1;
        }
        g::vr->reset_view();
    }
    if (ops & TOGGLE_DEBUG_INFO) {
        if (g::cfg.debug) {
            if (g::cfg.debug_mode == 1) {
                g::cfg.debug_mode = 0;
            } else {
                g::cfg.debug = false;
            }
        } else {
            g::cfg.debug = true;
            g::cfg.debug_mode = 1;
        }
        g::draw_overlay_border = (g::cfg.debug && g::cfg.debug_mode == 0);
    }
    if (ops & GET_VR_RUNTIME) {
        if (!g::vr) {
            return 0;
        }
        return static_cast<int64_t>(g::vr->get_runtime_type());
    }
    return 0;
}
