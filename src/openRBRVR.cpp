#include <d3d9.h>
#include <format>
#include <gtx/matrix_decompose.hpp>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <vector>
#include <winver.h>

#include "Config.hpp"
#include "D3D.hpp"
#include "Dx.hpp"
#include "Globals.hpp"
#include "Hook.hpp"
#include "Licenses.hpp"
#include "Menu.hpp"
#include "OpenVR.hpp"
#include "OpenXR.hpp"
#include "Quaternion.hpp"
#include "RBR.hpp"
#include "Util.hpp"
#include "VR.hpp"
#include "Version.hpp"
#include "Vertex.hpp"
#include "openRBRVR.hpp"

openRBRVR::openRBRVR(IRBRGame* g)
    : game(g)
{
    g::game = g;
    dbg("Hooking DirectX");

    dbg("Init checkpoint: before GetModuleHandle(d3d9.dll)");
    auto d3ddll = GetModuleHandle("d3d9.dll");
    dbg(d3ddll ? "Init checkpoint: d3d9.dll module found" : "Init checkpoint: d3d9.dll module missing");
    if (!d3ddll) {
        dbg("failed to get handle to d3d9.dll\n");
        return;
    }

    dbg("Init checkpoint: before reading d3d9.dll version");
    const auto d3d9dll_versioninfosize = GetFileVersionInfoSizeA("d3d9.dll", nullptr);
    dbg("Init checkpoint: d3d9.dll version size queried");
    if (d3d9dll_versioninfosize > 0) {
        const auto versioninfo = new uint8_t[d3d9dll_versioninfosize];
        if (GetFileVersionInfoA("d3d9.dll", 0, d3d9dll_versioninfosize, versioninfo) == TRUE) {
            uint32_t tsize;
            struct LANGANDCODEPAGE {
                uint16_t lang;
                uint16_t cp;
            }* t = nullptr;
            if (VerQueryValueA(versioninfo, "\\VarFileInfo\\Translation", reinterpret_cast<void**>(&t), &tsize) == TRUE && tsize >= sizeof(LANGANDCODEPAGE)) {
                const auto sub_block = std::format("\\StringFileInfo\\{:04x}{:04x}\\FileVersion", t[0].lang, t[0].cp);
                uint32_t size = 0;
                char* value = nullptr;
                if (VerQueryValueA(versioninfo, sub_block.c_str(), reinterpret_cast<void**>(&value), &size) && value) {
                    g::dxvk_version = std::string(value, value + size);
                }
            }
        }
        delete[] versioninfo;
    }

    dbg("Init checkpoint: before resolving Direct3DCreate9");
    auto d3dcreate = reinterpret_cast<decltype(&dx::Direct3DCreate9)>(GetProcAddress(d3ddll, "Direct3DCreate9"));
    dbg(d3dcreate ? "Init checkpoint: Direct3DCreate9 resolved" : "Init checkpoint: Direct3DCreate9 export missing");
    if (!d3dcreate) {
        dbg("failed to find address to Direct3DCreate9\n");
        return;
    }

    // These hooks are owned by the DLL and remain installed across plugin instances.
    // Hook::install re-enables a matching retained entry and creates missing entries,
    // allowing recovery after a partial rollback.
    try {
        dbg("Init checkpoint: before Direct3DCreate9 hook");
        g::hooks::create.install(d3dcreate, dx::Direct3DCreate9);
        dbg("Init checkpoint: Direct3DCreate9 hook installed");
        dbg("Init checkpoint: before RBR render hook");
        g::hooks::render.install(*reinterpret_cast<decltype(rbr::render)*>(rbr::get_render_function_addr()), rbr::render);
        dbg("Init checkpoint: RBR render hook installed");

        dbg("Init checkpoint: before resolving particle hooks");
        const auto addrs = rbr::get_render_particles_function_addrs();
        dbg("Init checkpoint: particle hook addresses resolved");
        g::hooks::render_particles.install(*reinterpret_cast<decltype(rbr::render_particles)*>(addrs[0]), rbr::render_particles);
        g::hooks::render_particles_2.install(*reinterpret_cast<decltype(rbr::render_particles_2)*>(addrs[1]), rbr::render_particles_2);
        g::hooks::render_particles_3.install(*reinterpret_cast<decltype(rbr::render_particles_3)*>(addrs[2]), rbr::render_particles_3);
        g::hooks::render_particles_4.install(*reinterpret_cast<decltype(rbr::render_particles_4)*>(addrs[3]), rbr::render_particles_4);
        dbg("Init checkpoint: all particle hooks installed");

        dbg("Init checkpoint: before camera target hook");
        g::hooks::set_camera_target.install(*reinterpret_cast<decltype(rbr::set_camera_target)*>(0x4663f0), rbr::set_camera_target);
        dbg("Init checkpoint: camera target hook installed");
        dbg("Init checkpoint: before windscreen hook");
        g::hooks::render_windscreen_effects.install(*reinterpret_cast<decltype(rbr::render_windscreen_effects)*>(rbr::get_address(0x452020)), rbr::render_windscreen_effects);
        dbg("Init checkpoint: all RBR hooks installed");
    } catch (const std::runtime_error& e) {
        // Disable every hook before attempting rollback. If any detour
        // cannot be disabled, retain the complete Hook ownership state so
        // its trampoline and target are not lost while it may still run.
        const bool hooksDisabled = disable_hooks(
            g::hooks::render_windscreen_effects,
            g::hooks::set_camera_target,
            g::hooks::render_particles_4,
            g::hooks::render_particles_3,
            g::hooks::render_particles_2,
            g::hooks::render_particles,
            g::hooks::render,
            g::hooks::create);

        if (hooksDisabled) {
            g::hooks::render_windscreen_effects.reset();
            g::hooks::set_camera_target.reset();
            g::hooks::render_particles_4.reset();
            g::hooks::render_particles_3.reset();
            g::hooks::render_particles_2.reset();
            g::hooks::render_particles.reset();
            g::hooks::render.reset();
            g::hooks::create.reset();
        } else {
            dbg("Could not disable every RBR hook; retaining hook ownership for retry");
        }

        dbg(e.what());
        MessageBoxA(nullptr, e.what(), "Hooking failed", MB_OK);
    }
    dbg("Init checkpoint: before loading Plugins/openRBRVR.toml and dxvk.conf");
    g::cfg = g::saved_cfg = Config::from_path("Plugins");
    dbg("Init checkpoint: configuration loaded");
    g::draw_overlay_border = (g::cfg.debug && g::cfg.debug_mode == 0);
}

openRBRVR::~openRBRVR()
{
    // IRBRGame is borrowed from the host. Stop exposing it to process-lifetime
    // hooks before tearing down this plugin instance.
    if (g::game == game) {
        g::game = nullptr;
    }

    // Do not let a later Present/transform callback observe a stale per-frame
    // VR target after the runtime has been destroyed.
    g::vr_render_target.reset();
    g::current_2d_render_target.reset();
    g::vr_error = false;

    if (g::vr) {
        delete g::vr;
        g::vr = nullptr;
    }

    // CreateDevice also creates the Bridge proxy when VR runtime startup
    // failed and g::vr is null. Runtime destructors normally release it,
    // but keep plugin teardown responsible for any remaining proxy.
    if (g::d3d_vr) {
        g::d3d_vr->Release();
        g::d3d_vr = nullptr;
    }

    if (g::original_render_target) {
        g::original_render_target->Release();
        g::original_render_target = nullptr;
    }
    if (g::original_depth_stencil_target) {
        g::original_depth_stencil_target->Release();
        g::original_depth_stencil_target = nullptr;
    }

    // RBR may request a new plugin instance after destroying the old one.
    if (g::openrbrvr == this) {
        g::openrbrvr = nullptr;
    }
}

void openRBRVR::HandleFrontEndEvents(char txtKeyboard, bool bUp, bool bDown, bool bLeft, bool bRight, bool bSelect)
{
    if (bSelect) {
        g::menu->select();
    }
    if (bUp) {
        g::menu->up();
    }
    if (bDown) {
        g::menu->down();
    }
    if (bLeft) {
        g::menu->left();
    }
    if (bRight) {
        g::menu->right();
    }
}

void openRBRVR::DrawMenuEntries(const std::ranges::forward_range auto& entries, float rowHeight)
{
    auto x = 0.0f;
    auto y = 0.0f;
    for (const auto& entry : entries) {
        if (entry.visible && !entry.visible.value()()) {
            // Skip hidden entries
            continue;
        }
        if (entry.font) {
            game->SetFont(entry.font.value());
        }
        if (entry.menu_color) {
            game->SetMenuColor(entry.menu_color.value());
        } else if (entry.color) {
            auto [r, g, b, a] = entry.color.value()();
            game->SetColor(r, g, b, a);
        }
        if (entry.position) {
            auto [newx, newy] = entry.position.value();
            x = newx;
            y = newy;
        }
        game->WriteText(x, y, entry.text().c_str());
        y += rowHeight;
    }
    game->SetFont(IRBRGame::EFonts::FONT_SMALL);
    game->SetColor(0.7f, 0.7f, 0.7f, 1.0f);
    game->WriteText(10.0f, 458.0f, std::format("openRBRVR {} - https://github.com/Detegr/openRBRVR", VERSION_STR).c_str());
}

void openRBRVR::DrawFrontEndPage()
{
    constexpr auto menuItemsStartHeight = 70.0f;
    const auto idx = g::menu->index();
    const auto isLicenseMenu = idx < 0;
    const auto visibleEntryCount = std::ranges::count_if(g::menu->entries(), [](const MenuEntry& e) {
        return !e.visible || e.visible.value()();
    });
    const auto endOfItems = menuItemsStartHeight + visibleEntryCount * g::menu->row_height();

    game->DrawBlackOut(0.0f, isLicenseMenu ? 88.0f : endOfItems, 800.0f, 10.0f);
    if (!isLicenseMenu) {
        game->DrawSelection(0.0f, menuItemsStartHeight - 2.0f + (static_cast<float>(g::menu->index()) * g::menu->row_height()), 440.0f);
    }

    game->SetFont(IRBRGame::EFonts::FONT_BIG);
    game->SetMenuColor(IRBRGame::EMenuColors::MENU_HEADING);
    game->WriteText(65.0f, 49.0f, g::menu->heading());

    const auto& entries = g::menu->entries();
    DrawMenuEntries(entries, g::menu->row_height());

    game->SetFont(IRBRGame::EFonts::FONT_BIG);
    game->SetMenuColor(IRBRGame::EMenuColors::MENU_TEXT);

    auto i = 0;
    if (!isLicenseMenu) {
        for (const auto& txt : entries[g::menu->entry_idx].long_text) {
            game->WriteText(65.0f, endOfItems + ((i + 1) * g::menu->row_height()), txt.c_str());
            i++;
        }
    }
}
