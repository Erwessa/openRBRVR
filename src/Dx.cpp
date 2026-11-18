#include "Dx.hpp"
#include "Globals.hpp"
#include "IPlugin.h"
#include "OpenVR.hpp"
#include "OpenXR.hpp"
#include "RBR.hpp"
#include "Util.hpp"
#include "Version.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <memory>
#include <array>
#include <gtx/matrix_decompose.hpp>
#include <unordered_set>

using MultiViewAddFn = int (*)(uint32_t*, uint32_t, uint32_t*, uint32_t*);
using MultiViewPatchFn = int (*)(uint32_t*, uint32_t, uint32_t*, uint32_t*, uint32_t, uint32_t, int8_t);
using MultiViewOptimizeFn = int (*)(uint32_t*, uint32_t, uint32_t*, uint32_t*);

// Compilation unit global variables
namespace g {
    static std::chrono::steady_clock::time_point second_start;
    static int fps;
    static int current_frames;
    static thread_local bool applying_state_block;
    static std::vector<IDirect3DVertexShader9*> base_game_shaders;
    static std::vector<IDirect3DVertexShader9*> base_game_multiview_shaders;
    // Keep the identity of the 40th RBR shader independent of external registrations.
    static IDirect3DVertexShader9* base_game_shader_39 = nullptr;
    static std::vector<IDirect3DVertexShader9*> external_shaders;
    static std::vector<IDirect3DVertexShader9*> external_multiview_shaders;
    static MultiViewPatchFn spirv_change_multiview_access;
    static MultiViewOptimizeFn spirv_optimize_multiview;
    static std::vector<IDirect3DVertexShader9*> original_btb_shaders;
    static std::vector<IDirect3DVertexShader9*> multiview_btb_shaders;
    static std::unordered_map<IDirect3DVertexShader9*, std::vector<uint32_t>> patched_btb_shaders;
    static std::unordered_set<IDirect3DVertexShader9*> optimized_btb_shaders;
    struct DeferredShaderConstant {
        uint32_t start_register;
        std::array<float, 16> values;
    };
    static std::vector<DeferredShaderConstant> deferred_shader_constants;
    static std::mutex deferred_shader_constants_mutex;
    static std::atomic<int> failed_multiview_base_game_shaders { 0 };
    static std::atomic<int> failed_multiview_external_shaders { 0 };
    static std::atomic<int> failed_multiview_btb_shaders { 0 };
    static std::atomic<int> failed_multiview_btb_shader_optimizations { 0 };
}

namespace dx {
    namespace shader {
        static M4 current_projection_matrix;
        static M4 current_projection_matrix_inverse;
    }

    namespace fixedfunction {
        static D3DMATRIX current_projection_matrix[4];
        static D3DMATRIX current_view_matrix[4];
        static D3DMATRIX current_world_matrix;
        static D3DMATRIX current_projview_matrix[4];
        static D3DMATRIX current_viewproj_matrix[4];
        static D3DMATRIX current_worldview_matrix[4];
        static D3DMATRIX current_worldviewproj_matrix[4];
    }

    using rbr::GameMode;

    void free_btb_shaders()
    {
        for (auto& s : g::original_btb_shaders) {
            s->Release();
        }
        for (auto& s : g::multiview_btb_shaders) {
            s->Release();
        }

        g::original_btb_shaders.clear();
        g::multiview_btb_shaders.clear();
        g::optimized_btb_shaders.clear();
        g::patched_btb_shaders.clear();
    }

    // Call the RBR render function with a texture as the render target
    // Even though the render pipeline changes the render target while rendering,
    // the original render target is respected and restored at the end of the pipeline.
    void render_vr_eye(void* p, RenderTarget eye, bool clear)
    {
        g::vr_render_target = eye;
        if (g::vr->prepare_vr_rendering(g::d3d_dev, eye, clear)) {
            g::hooks::render.call(p);
            g::vr->render_hands(g::d3d_dev, eye);
            g::vr->finish_vr_rendering(g::d3d_dev, eye);
        } else {
            dbg("Failed to set 3D render target");
            if (g::original_render_target && g::d3d_dev->SetRenderTarget(0, g::original_render_target) != D3D_OK) {
                dbg("Failed to restore original render target after VR eye setup failure");
            }
            if (g::original_depth_stencil_target && g::d3d_dev->SetDepthStencilSurface(g::original_depth_stencil_target) != D3D_OK) {
                dbg("Failed to restore original depth stencil surface after VR eye setup failure");
            }
        }
        g::vr_render_target = std::nullopt;
    }

    static bool optimize_spirv_shader(IDirect3DVertexShader9* s)
    {
        uint32_t spirv_size = 0;
        if (FAILED(g::d3d_vr->GetSPIRVShaderCode(s, nullptr, &spirv_size)) || spirv_size == 0)
            return false;

        std::vector<uint32_t> spirv(spirv_size);
        if (FAILED(g::d3d_vr->GetSPIRVShaderCode(s, spirv.data(), &spirv_size)))
            return false;

        uint32_t patched_spirv_size = 0;
        if (g::spirv_optimize_multiview(spirv.data(), spirv.size(), nullptr, &patched_spirv_size) != 0
            || patched_spirv_size == 0) {
            return false;
        }

        std::vector<uint32_t> patched_spirv(patched_spirv_size);
        if (g::spirv_optimize_multiview(spirv.data(), spirv.size(), patched_spirv.data(), &patched_spirv_size) != 0)
            return false;

        return SUCCEEDED(g::d3d_vr->PatchSPIRVToVertexShader(s, patched_spirv.data(), patched_spirv_size));
    }

    static bool patch_spirv_shader(IDirect3DVertexShader9* s, uint32_t f_idx, uint32_t data_start_register, bool optimize)
    {
        uint32_t spirv_size = 0;
        if (FAILED(g::d3d_vr->GetSPIRVShaderCode(s, nullptr, &spirv_size)) || spirv_size == 0)
            return false;

        std::vector<uint32_t> spirv(spirv_size);
        if (FAILED(g::d3d_vr->GetSPIRVShaderCode(s, spirv.data(), &spirv_size)))
            return false;

        uint32_t patched_spirv_size = 0;
        if (g::spirv_change_multiview_access(spirv.data(), spirv.size(), nullptr, &patched_spirv_size, f_idx, data_start_register, optimize) != 0
            || patched_spirv_size == 0) {
            return false;
        }

        std::vector<uint32_t> patched_spirv(patched_spirv_size);
        if (g::spirv_change_multiview_access(spirv.data(), spirv.size(), patched_spirv.data(), &patched_spirv_size, f_idx, data_start_register, optimize) != 0)
            return false;

        return SUCCEEDED(g::d3d_vr->PatchSPIRVToVertexShader(s, patched_spirv.data(), patched_spirv_size));
    }

    static bool patch_spirv_shader_registers(IDirect3DVertexShader9* s)
    {
        // Make room for the extra data
        if (FAILED(g::d3d_vr->SetShaderConstantCount(s, g::base_shader_data_end_register * 2)))
            return false;

        // MVP matrix
        if (!patch_spirv_shader(s, 0, g::base_shader_data_end_register, false)) {
            char hash[64] = { 0 };
            g::d3d_vr->GetShaderHash(s, (char**)&hash);
            dbg(std::format("Error patching a shader: {}", hash));
            return false;
        }

        // Skybox/fog
        if (!patch_spirv_shader(s, 20, g::base_shader_data_end_register, true)) {
            char hash[64] = { 0 };
            g::d3d_vr->GetShaderHash(s, (char**)&hash);
            dbg(std::format("Error patching a shader: {}", hash));
            return false;
        }

        return true;
    }

    bool add_vertex_shader(IDirect3DVertexShader9* shader)
    {
        if (!shader) {
            dbg("Cannot register a null vertex shader");
            return false;
        }

        // Registration is idempotent: a caller may notify us about a shader
        // that was already captured by CreateVertexShader or registered earlier.
        if (std::find(g::base_game_shaders.cbegin(), g::base_game_shaders.cend(), shader) != g::base_game_shaders.cend())
            return true;

        UINT fn_size;

        if (shader->GetFunction(nullptr, &fn_size) != D3D_OK) {
            dbg("Failed to get vertex shader function");
            return false;
        }

        std::vector<DWORD> bytecode(fn_size / sizeof(DWORD));
        if (shader->GetFunction(bytecode.data(), &fn_size) != D3D_OK) {
            dbg("Failed to get vertex shader function");
            return false;
        }

        // Pop one off the end as we're adding more bytecode
        bytecode.pop_back();

        // Add a zero-length comment opcode in the shader code to make DXVK
        // handle this as a new shader and not reuse the one we just created
        bytecode.push_back(65534);
        bytecode.push_back(65535);

        // Treat shaders from other plugins as base game shaders
        // Nobody will want to edit BTB shaders :D
        g::base_game_shaders.push_back(shader);

        if (multiview_supported() && g::d3d_dev && g::hooks::create_vertex_shader.src) {
            IDirect3DVertexShader9* modified_shader = nullptr;
            const auto ret = g::hooks::create_vertex_shader.call(g::d3d_dev, bytecode.data(), &modified_shader);
            if (FAILED(ret) || !modified_shader) {
                g::base_game_multiview_shaders.push_back(shader);
                g::failed_multiview_external_shaders++;
            } else if (patch_spirv_shader_registers(modified_shader)) {
                g::base_game_multiview_shaders.push_back(modified_shader);
            } else {
                // The newly-created shader is not retained when falling back
                // to the original shader, so release its COM reference.
                modified_shader->Release();
                g::base_game_multiview_shaders.push_back(shader);
                g::failed_multiview_external_shaders++;
            }
        } else {
            // Keep the vectors index-aligned when multiview or the device
            // shader hook is not ready. In particular, CreateDevice creates
            // the D3D9VR proxy before installing the shader hooks.
            g::base_game_multiview_shaders.push_back(shader);
            if (multiview_supported())
                g::failed_multiview_external_shaders++;
        }

        return true;
    }

    HRESULT __stdcall CreateVertexShader(IDirect3DDevice9* This, const DWORD* pFunction, IDirect3DVertexShader9** ppShader)
    {
        if (!g::game)
            return g::hooks::create_vertex_shader.call(This, pFunction, ppShader);

        if (!g::spirv_change_multiview_access || !g::spirv_optimize_multiview) {
            if (auto patcher = LoadLibrary("Plugins/openRBRVR/multiviewpatcher.dll"); patcher) {
                g::spirv_change_multiview_access = reinterpret_cast<MultiViewPatchFn>(GetProcAddress(patcher, "ChangeSPIRVMultiViewDataAccessLocation"));
                g::spirv_optimize_multiview = reinterpret_cast<MultiViewOptimizeFn>(GetProcAddress(patcher, "OptimizeSPIRV"));
            }
        }

        static int i = 0;
        static std::array<size_t, 40> base_game_shader_indices {};

        auto ret = g::hooks::create_vertex_shader.call(g::d3d_dev, pFunction, ppShader);
        if (FAILED(ret) || !ppShader || !*ppShader) {
            return FAILED(ret) ? ret : E_FAIL;
        }

        if (i < 40) {
            // These are the base game shaders for RBR that need
            // to be patched with the VR projection. Keep their indices
            // separate from external shaders registered through the API.
            base_game_shader_indices[i] = g::base_game_shaders.size();
            g::base_game_shaders.push_back(*ppShader);
            if (i == 39)
                g::base_game_shader_39 = *ppShader;
        } else {
            // Same for BTB shaders that are loaded during the stage load
            g::original_btb_shaders.push_back(*ppShader);
            (*ppShader)->AddRef();
        }

        if (multiview_supported()) {
            // Determine shader bytecode length. Valid shaders end at double word with value 65535.
            const DWORD* pp;
            for (pp = pFunction; *pp != 65535; ++pp) { }

            std::vector<DWORD> bytecode(pFunction, pp);

            // Add a zero-length comment opcode in the shader code to make DXVK
            // handle this as a new shader and not reuse the one we just created
            bytecode.push_back(65534);
            bytecode.push_back(65535);

            // Create the same shader again for multiview patching
            IDirect3DVertexShader9* multiview_shader = nullptr;
            const auto multiview_ret = g::hooks::create_vertex_shader.call(g::d3d_dev, bytecode.data(), &multiview_shader);

            if (FAILED(multiview_ret) || !multiview_shader) {
                // Keep the original shader as a safe fallback. Its lifetime is
                // already covered by the game's reference (base game) or the
                // explicit cache reference acquired above (BTB).
                if (i < 40) {
                    g::base_game_multiview_shaders.push_back(*ppShader);
                    g::failed_multiview_base_game_shaders++;
                } else {
                    // free_btb_shaders owns and releases every entry in this
                    // vector, including this fallback reference.
                    (*ppShader)->AddRef();
                    g::multiview_btb_shaders.push_back(*ppShader);
                    g::failed_multiview_btb_shaders++;
                }
            } else if (i < 40) {
                g::base_game_multiview_shaders.push_back(multiview_shader);
            } else {
                g::multiview_btb_shaders.push_back(multiview_shader);
            }
        }

        i++;
        if (multiview_supported() && i == 40) {
            // Base game shaders are loaded, patch them for multiview and run the SPIR-V optimizer

            // Maximum number of float constants that is discovered to be present in a shader.
            // Base game shaders have maximum of 90-something constants and those could be calculated,
            // but BTB stage shaders aren't loaded at startup, so we're making an educated guess here.
            g::base_shader_data_end_register = 110;

            for (auto j = 0; j < i; ++j) {
                const auto shader_index = base_game_shader_indices[j];
                const auto original_shader = g::base_game_shaders[shader_index];
                const auto multiview_shader = g::base_game_multiview_shaders[shader_index];

                if (multiview_shader == original_shader) {
                    // Creation already fell back to the game-owned original;
                    // do not patch it in place.
                    continue;
                }

                if (!patch_spirv_shader_registers(multiview_shader)) {
                    // If the patching fails, use the original shader
                    // This will probably cause rendering glitches but should not crash the game
                    multiview_shader->Release();
                    g::base_game_multiview_shaders[shader_index] = original_shader;
                    g::failed_multiview_base_game_shaders++;
                }
            }
        }

        return ret;
    }

    HRESULT __stdcall GetVertexShader(IDirect3DDevice9* This, IDirect3DVertexShader9** pShader)
    {
        if (!g::game)
            return g::hooks::get_vertex_shader.call(This, pShader);

        const auto ret = g::hooks::get_vertex_shader.call(This, pShader);
        if (multiview_rendering_enabled()) {
            auto shader_it = std::find(g::base_game_shaders.cbegin(), g::base_game_shaders.cend(), *pShader);
            if (shader_it != g::base_game_shaders.cend()) {
                const auto index = std::distance(g::base_game_shaders.cbegin(), shader_it);
                auto shader = g::base_game_multiview_shaders[index];
                shader->AddRef();
                (*pShader)->Release();
                *pShader = shader;
                return ret;
            }

            shader_it = std::find(g::original_btb_shaders.cbegin(), g::original_btb_shaders.cend(), *pShader);
            if (shader_it != g::original_btb_shaders.cend()) {
                const auto index = std::distance(g::original_btb_shaders.cbegin(), shader_it);
                auto shader = g::multiview_btb_shaders[index];
                shader->AddRef();
                (*pShader)->Release();
                *pShader = shader;
                return ret;
            }
        }
        return ret;
    }

    HRESULT __stdcall SetVertexShader(IDirect3DDevice9* This, IDirect3DVertexShader9* pShader)
    {
        if (!g::game)
            return g::hooks::set_vertex_shader.call(This, pShader);

        IDirect3DVertexShader9* shader = pShader;
        if (multiview_rendering_enabled()) {
            auto shader_it = std::find(g::base_game_shaders.cbegin(), g::base_game_shaders.cend(), pShader);
            if (shader_it != g::base_game_shaders.cend()) {
                const auto index = std::distance(g::base_game_shaders.cbegin(), shader_it);
                shader = g::base_game_multiview_shaders[index];
            } else {
                shader_it = std::find(g::original_btb_shaders.cbegin(), g::original_btb_shaders.cend(), pShader);
                if (shader_it != g::original_btb_shaders.cend()) {
                    const auto index = std::distance(g::original_btb_shaders.cbegin(), shader_it);
                    shader = g::multiview_btb_shaders[index];
                }
            }
        }

        const auto ret = g::hooks::set_vertex_shader.call(This, shader);

        // If the shader was a null pointer while SetVertexShaderConstantF
        // we have been collecting the data to g::deferred_shader_constants
        // As we now have the shader present, apply the constants so we run
        // our shader patching and data relocation correctly.
        if (SUCCEEDED(ret) && pShader) {
            // Detach the pending batch while holding the map lock, then replay
            // it without the lock because SetVertexShaderConstantF re-enters
            // this hook and performs D3D9/Bridge work.
            decltype(g::deferred_shader_constants) deferred_constants;
            {
                std::lock_guard lock(g::deferred_shader_constants_mutex);
                deferred_constants.swap(g::deferred_shader_constants);
            }

            for (auto it = deferred_constants.cbegin(); it != deferred_constants.cend(); ++it) {
                // We can just call our patched SetVertexShaderConstantF function
                // It will handle patching the (BTB) shader etc. correctly
                const HRESULT constantResult = SetVertexShaderConstantF(This, it->start_register, it->values.data(), 4);
                if (FAILED(constantResult)) {
                    // Preserve this and later values for a future successful bind.
                    // Do not overwrite newer writes that arrived while this batch
                    // was being replayed.
                    std::lock_guard lock(g::deferred_shader_constants_mutex);
                    g::deferred_shader_constants.insert(
                        g::deferred_shader_constants.begin(), it, deferred_constants.cend());
                    break;
                }
            }
        }

        return ret;
    }

    static void draw_debug_info(uint64_t cpu_frametime_us)
    {
        // The RBR game interface is borrowed from the host and may be cleared
        // while process-lifetime D3D9 hooks remain installed.
        if (!g::game)
            return;

        g::game->SetColor(1, 0, 1, 1.0);
        g::game->SetFont(IRBRGame::EFonts::FONT_DEBUG);

        float i = 0.0f;
        auto t = g::vr->get_frame_timing();
        const float cpuTime = cpu_frametime_us / 1000.0f;
        static float gpu_total = 0.0f;

        if (g::vr->get_runtime_type() == OPENXR) {
            if (t.gpu_total > 0.0) {
                // Cache non-zero GPU total value as we won't get a new value for every frame
                gpu_total = t.gpu_total * 1000.0f;
            }
        }

        if (g::cfg.debug_mode == 0) {
            g::game->WriteText(0, 18 * ++i, std::format("openRBRVR {}", VERSION_STR).c_str());
            g::game->WriteText(0, 18 * ++i, g::dxvk_version.c_str());
            g::game->WriteText(0, 18 * ++i, std::format("VR runtime: {}", g::vr->get_runtime_type() == OPENVR ? "OpenVR (SteamVR)" : "OpenXR").c_str());

            if (g::vr->get_runtime_type() == OPENVR) {
                static uint32_t totalDroppedFrames = 0;
                static uint32_t totalMispresentedFrames = 0;
                totalDroppedFrames += t.dropped_frames;
                totalMispresentedFrames += t.mispresented_frames;

                g::game->WriteText(0, 18 * ++i,
                    std::format("CPU: render time: {:.2f}ms, present: {:.2f}ms, waitforpresent: {:.2f}ms",
                        cpuTime,
                        t.cpu_present_call,
                        t.cpu_wait_for_present)
                        .c_str());
                g::game->WriteText(0, 18 * ++i,
                    std::format("GPU: presubmit {:.2f}ms, postSubmit: {:.2f}ms, total: {:.2f}ms",
                        t.gpu_pre_submit,
                        t.gpu_post_submit,
                        t.gpu_total)
                        .c_str());
                g::game->WriteText(0, 18 * ++i,
                    std::format("Compositor: CPU: {:.2f}ms, GPU: {:.2f}ms, submit: {:.2f}ms",
                        t.compositor_cpu,
                        t.compositor_gpu,
                        t.compositor_submit_frame)
                        .c_str());
                g::game->WriteText(0, 18 * ++i,
                    std::format("Total mispresented frames: {}, Total dropped frames: {}, Reprojection flags: {:X}",
                        totalMispresentedFrames,
                        totalDroppedFrames,
                        t.reprojection_flags)
                        .c_str());
            } else {
                g::game->WriteText(0, 18 * ++i, std::format("CPU: render time: {:.2f}ms", cpuTime).c_str());
                g::game->WriteText(0, 18 * ++i, std::format("GPU: render time: {:.2f}ms", gpu_total).c_str());
            }

            g::game->WriteText(0, 18 * ++i, std::format("Mods: {} {}", rbr_rx::is_loaded() ? "RBRRX" : "", rbrhud::is_loaded() ? "RBRHUD" : "").c_str());
            g::game->WriteText(0, 18 * ++i, std::format("Render context: {}", g::vr->get_current_render_context_name()).c_str());
            const auto& [lw, lh] = g::vr->get_render_resolution(LeftEye);
            const auto& [rw, rh] = g::vr->get_render_resolution(RightEye);
            g::game->WriteText(0, 18 * ++i, std::format("Render resolution: {}x{} (left), {}x{} (right)", lw, lh, rw, rh).c_str());
            if (g::vr->is_using_quad_view_rendering()) {
                const auto& [flw, flh] = g::vr->get_render_resolution(FocusLeft);
                const auto& [frw, frh] = g::vr->get_render_resolution(FocusRight);
                g::game->WriteText(0, 18 * ++i, std::format("                         {}x{} (focus left), {}x{} (focus right)", flw, flh, frw, frh).c_str());
                const auto qv = reinterpret_cast<OpenXR*>(g::vr)->native_quad_views;
                g::game->WriteText(0, 18 * ++i, std::format("{}oveated rendering {}", qv ? "Native F" : "F", qv ? "" : "via Quad-Views-Foveated OpenXR layer").c_str());
            }
            if (g::vr->is_using_quad_view_rendering()) {
                g::game->WriteText(0, 18 * ++i, std::format("Anti-aliasing: {}x, peripheral: {}x", static_cast<int>(g::vr->get_current_render_context()->msaa), static_cast<int>(g::cfg.peripheral_msaa)).c_str());
            } else {
                g::game->WriteText(0, 18 * ++i, std::format("Anti-aliasing: {}x", static_cast<int>(g::vr->get_current_render_context()->msaa)).c_str());
            }
            if (multiview_rendering_enabled()) {
                g::game->WriteText(0, 18 * ++i, std::format("Multiview rendering").c_str());
                const int failedBaseGameShaders = g::failed_multiview_base_game_shaders.load(std::memory_order_relaxed);
                const int failedExternalShaders = g::failed_multiview_external_shaders.load(std::memory_order_relaxed);
                const int failedBtbShaders = g::failed_multiview_btb_shaders.load(std::memory_order_relaxed);
                const int failedBtbShaderOptimizations = g::failed_multiview_btb_shader_optimizations.load(std::memory_order_relaxed);
                if (failedBaseGameShaders > 0)
                    g::game->WriteText(0, 18 * ++i, std::format("  Failed base game shaders: {}", failedBaseGameShaders).c_str());
                if (failedExternalShaders > 0)
                    g::game->WriteText(0, 18 * ++i, std::format("  Failed external shaders: {}", failedExternalShaders).c_str());
                if (failedBtbShaders > 0)
                    g::game->WriteText(0, 18 * ++i, std::format("  Failed BTB shaders: {}", failedBtbShaders).c_str());
                if (failedBtbShaderOptimizations > 0)
                    g::game->WriteText(0, 18 * ++i, std::format("  Failed BTB shader optimizations: {}", failedBtbShaderOptimizations).c_str());
            }
            g::game->WriteText(0, 18 * ++i, std::format("Anisotropic filtering: {}x", g::cfg.anisotropy).c_str());
            g::game->WriteText(0, 18 * ++i, std::format("Current stage ID: {}", rbr::get_current_stage_id()).c_str());
        } else {
            float frameTime;
            if (g::vr->get_runtime_type() == OPENVR) {
                frameTime = std::max<float>(cpuTime, t.gpu_total);
            } else {
                frameTime = std::max<float>(cpuTime, gpu_total);
            }

            if (g::fps > 0 && g::target_fps > 0) {
                const auto target = 1000.0 / g::target_fps;
                if (frameTime / target < 0.7) {
                    g::game->SetColor(0.7f, 1.0f, 0, 1.0f);
                } else if (g::fps < (g::target_fps - 1)) {
                    g::game->SetColor(1.0f, 0.0f, 0, 1.0f);
                } else {
                    g::game->SetColor(1.0f, 0.6f, 0, 1.0f);
                }

                g::game->WriteText(0, 0, std::format("{}", g::fps).c_str());
            }
        }
    }

    // Render `renderTarget2d` on a plane for both eyes
    static void render_vr_overlay(RenderTarget render_target_2d, bool clear)
    {
        const auto& size = render_target_2d == GameMenu ? 1.0f : g::cfg.overlay_size;
        const auto& translation = render_target_2d == Overlay ? g::cfg.overlay_translation : (g::cfg.menu_scene ? glm::vec3 { 0.0f, -0.1f, 0.65f - g::cfg.menu_size } : glm::vec3 { 0.0f, -0.1f, 0.65f - g::cfg.menu_size });
        const auto& horizon_lock = render_target_2d == Overlay ? std::make_optional(rbr::get_horizon_lock_matrix()) : std::nullopt;
        const auto& texture = g::vr->get_texture(render_target_2d);

        if (g::vr->prepare_vr_rendering(g::d3d_dev, LeftEye, clear)) {
            render_menu_quad(g::d3d_dev, g::vr, texture, LeftEye, render_target_2d, size, translation, horizon_lock);
            g::vr->finish_vr_rendering(g::d3d_dev, LeftEye);
        } else {
            dbg("Failed to render left eye overlay");
        }

        if (g::vr->is_using_quad_view_rendering()) {
            if (g::vr->prepare_vr_rendering(g::d3d_dev, FocusLeft, clear)) {
                render_menu_quad(g::d3d_dev, g::vr, texture, FocusLeft, render_target_2d, size, translation, horizon_lock);
                g::vr->finish_vr_rendering(g::d3d_dev, FocusLeft);
            } else {
                dbg("Failed to render left eye overlay");
            }
        }

        if (!multiview_rendering_enabled()) {
            if (g::vr->prepare_vr_rendering(g::d3d_dev, RightEye, clear)) {
                render_menu_quad(g::d3d_dev, g::vr, texture, RightEye, render_target_2d, size, translation, horizon_lock);
                g::vr->finish_vr_rendering(g::d3d_dev, RightEye);
            } else {
                dbg("Failed to render left eye overlay");
            }
            if (g::vr->is_using_quad_view_rendering()) {
                if (g::vr->prepare_vr_rendering(g::d3d_dev, FocusRight, clear)) {
                    render_menu_quad(g::d3d_dev, g::vr, texture, FocusRight, render_target_2d, size, translation, horizon_lock);
                    g::vr->finish_vr_rendering(g::d3d_dev, FocusRight);
                } else {
                    dbg("Failed to render left eye overlay");
                }
            }
        }
    }

    HRESULT __stdcall Present(IDirect3DDevice9* This, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion)
    {
        if (!g::game)
            return g::hooks::present.call(This, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);

        auto* vrContext = g::vr ? g::vr->get_current_render_context() : nullptr;
        if (g::vr && !g::vr_error && vrContext) [[likely]] {
            auto shouldRender = g::current_2d_render_target && !(rbr::is_loading_btb_stage() && !g::cfg.draw_loading_screen);
            if (shouldRender) {
                if (g::draw_overlay_border && vrContext->overlay_border) {
                    render_overlay_border(g::d3d_dev, vrContext->overlay_border);
                }
                g::vr->finish_vr_rendering(g::d3d_dev, g::current_2d_render_target.value());
                render_vr_overlay(g::current_2d_render_target.value(), !rbr::is_rendering_3d());
            }
        }

        if (g::original_render_target && g::d3d_dev->SetRenderTarget(0, g::original_render_target) != D3D_OK) {
            dbg("Failed to reset render target to original");
        }
        if (g::original_depth_stencil_target && g::d3d_dev->SetDepthStencilSurface(g::original_depth_stencil_target) != D3D_OK) {
            dbg("Failed to reset depth stencil surface to original");
        }
        auto ret = 0;
        if (g::vr && !g::vr_error && vrContext) {
            auto game_mode = rbr::get_game_mode();
            const auto companion_eye = static_cast<RenderTarget>(g::cfg.companion_eye + (g::vr->is_using_quad_view_rendering() ? 2 : 0));
            if (g::cfg.companion_mode == CompanionMode::Static) {
                if (game_mode == GameMode::Driving || game_mode == GameMode::Pause) {
                    // Render the overlay over the 3D content, if we're running the static view on desktop window
                    // Otherwise, the help texts, pause menu, pacenote plugin UI etc. won't be visible
                    render_companion_window_from_render_target(g::d3d_dev, g::vr, Overlay);
                } else if (game_mode == GameMode::PreStage) {
                    render_companion_window_from_render_target(g::d3d_dev, g::vr, g::cfg.render_prestage_3d ? companion_eye : GameMenu);
                } else if (game_mode != GameMode::Driving) {
                    render_companion_window_from_render_target(g::d3d_dev, g::vr, rbr::is_rendering_3d() && game_mode != GameMode::MainMenu ? companion_eye : GameMenu);
                }
            } else if (g::cfg.companion_mode == CompanionMode::VREye || game_mode != GameMode::Driving) {
                render_companion_window_from_render_target(g::d3d_dev, g::vr, (rbr::is_rendering_3d() && rbr::get_game_mode() != GameMode::MainMenu) ? companion_eye : GameMenu);
            }
            g::vr->prepare_frames_for_hmd(g::d3d_dev);
        }

        auto frame_end = std::chrono::steady_clock::now();
        auto cpu_frame_time = std::chrono::duration_cast<std::chrono::microseconds>(frame_end - g::frame_start);

        ret = g::hooks::present.call(g::d3d_dev, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        g::current_frames++;

        if (g::d3d_vr) {
            for (auto& it : g::patched_btb_shaders) {
                if (optimize_spirv_shader(it.first)) {
                    g::optimized_btb_shaders.insert(it.first);
                } else {
                    dbg("Shader optimization failed!");
                    g::failed_multiview_btb_shader_optimizations++;
                }
            }
        }
        g::patched_btb_shaders.clear();

        if (FAILED(ret)) {
            dbg(std::format("D3D9 Present failed (HRESULT 0x{:08X}); skipping OpenVR frame submission", static_cast<uint32_t>(ret)));
        }
        if (g::vr) {
            if (SUCCEEDED(ret) && !g::vr_error) {
                g::vr->submit_frames_to_hmd(g::d3d_dev);

                if (g::cfg.debug) {
                    draw_debug_info(cpu_frame_time.count());
                }
            } else {
                g::vr->discard_prepared_frames();
            }
        }

        if (g::original_render_target) {
            g::original_render_target->Release();
            g::original_render_target = nullptr;
        }
        if (g::original_depth_stencil_target) {
            g::original_depth_stencil_target->Release();
            g::original_depth_stencil_target = nullptr;
        }

        if (std::chrono::steady_clock::now() - g::second_start > std::chrono::seconds(1)) {
            g::second_start = std::chrono::steady_clock::now();
            g::fps = g::current_frames - 1;
            g::target_fps = std::max(g::fps, g::target_fps);
            g::current_frames = 0;
        }

        g::vr_error = false;
        return ret;
    }

    static bool set_btb_vertex_shader_constant(IDirect3DDevice9* This, UINT StartRegister, uint32_t start_index, const D3DMATRIX* matrices, const float* pConstantData, UINT Vector4fCount, HRESULT& result)
    {
        const auto left = g::vr_render_target.value_or(LeftEye);
        const auto right = render_target_counterpart(left);
        if (memcmp(pConstantData, &matrices[left], sizeof(D3DMATRIX)) == 0) {
            // A D3DMATRIX contains exactly four float4 rows. The caller may pass
            // five vectors because the fifth value is handled separately below.
            result = g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, StartRegister + start_index, reinterpret_cast<const float*>(matrices[left].m), 4);
            const HRESULT rightResult = g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, StartRegister + start_index + 4, reinterpret_cast<const float*>(matrices[right].m), 4);
            if (SUCCEEDED(result) && FAILED(rightResult))
                result = rightResult;

            if (Vector4fCount > 4) {
                // There's this one weird shader that has the camera position as fifth Vector4f element after the matrix.
                // Copy the original data in the original location as the fifth element can be same for each view as it's moving the skybox along the camera.
                const HRESULT originalResult = g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, StartRegister, pConstantData, Vector4fCount);
                if (SUCCEEDED(result) && FAILED(originalResult))
                    result = originalResult;
            }
            return true;
        }
        return false;
    }
    HRESULT __stdcall SetVertexShaderConstantF(IDirect3DDevice9* This, UINT StartRegister, const float* pConstantData, UINT Vector4fCount)
    {
        if (!g::game)
            return g::hooks::set_vertex_shader_constant_f.call(This, StartRegister, pConstantData, Vector4fCount);

        IDirect3DVertexShader9* shader = nullptr;
        const auto shaderResult = g::d3d_dev->GetVertexShader(&shader);
        // GetVertexShader returns an AddRef'd interface. Keep that reference
        // scoped so every path (including the ordinary pass-through path)
        // releases it.
        const auto releaseShader = [](IDirect3DVertexShader9* value) {
            if (value)
                value->Release();
        };
        [[maybe_unused]] std::unique_ptr<IDirect3DVertexShader9, decltype(releaseShader)> shaderRef(shader, releaseShader);
        if (shaderResult != D3D_OK) {
            dbg("Could not get vertex shader");
            return shaderResult;
        }

        auto is_base_shader = true;
        if (rbr::is_on_btb_stage()) {
            const auto& shaders = multiview_rendering_enabled() ? g::base_game_multiview_shaders : g::base_game_shaders;
            is_base_shader = std::find(shaders.cbegin(), shaders.cend(), shader) != shaders.end();
        }

        auto reg = multiview_rendering_enabled() ? StartRegister + g::base_shader_data_end_register : StartRegister;
        if (!shader && Vector4fCount == 4) {
            // DirectX allows setting shader constants even though the shader isn't bound (yet)
            // Therefore we need to defer setting the constants for such shaders in order to be able to
            // correctly patch them for multiview and supply the data accordingly.
            std::array<float, 16> m;
            std::copy_n(pConstantData, 16, m.begin());
            {
                std::lock_guard lock(g::deferred_shader_constants_mutex);
                g::deferred_shader_constants.push_back({ StartRegister, m });
            }
        } else if (shader && is_base_shader && Vector4fCount == 4) {

            if (g::vr_render_target) {
                const auto target = g::vr_render_target.value();
                if (StartRegister == 0) {
                    const auto orig = glm::transpose(m4_from_shader_constant_ptr(pConstantData));
                    const auto& projection = g::vr->get_projection(target);
                    const auto& eyepos = g::vr->get_eye_pos(target);
                    const auto& pose = g::vr->get_pose(target);

                    // MVP matrix
                    // MV = P^-1 * MVP
                    // MVP[VRRenderTarget] = P[VRRenderTarget] * MV
                    const auto mv = shader::current_projection_matrix_inverse * orig;
                    const auto mvp = glm::transpose(projection * eyepos * pose * g::flip_z_matrix * rbr::get_horizon_lock_matrix() * mv);
                    auto ret = g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, reg, glm::value_ptr(mvp), Vector4fCount);

                    if (multiview_rendering_enabled()) {
                        const auto right = render_target_counterpart(target);
                        const auto& projection = g::vr->get_projection(right);
                        const auto& eyepos = g::vr->get_eye_pos(right);
                        const auto& pose = g::vr->get_pose(right);
                        const auto mvp = glm::transpose(projection * eyepos * pose * g::flip_z_matrix * rbr::get_horizon_lock_matrix() * mv);
                        ret |= g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, reg + 4, glm::value_ptr(mvp), Vector4fCount);
                    }
                    return ret;
                } else if (StartRegister == 20) {
                    // Sky/fog
                    // It seems this parameter contains the orientation of the car and the
                    // skybox and fog is rendered correctly only in the direction where the car
                    // points at. By rotating this with the HMD's rotation, the skybox and possible
                    // fog is rendered correctly.
                    const auto orig = glm::transpose(m4_from_shader_constant_ptr(pConstantData));
                    glm::vec4 perspective;
                    glm::vec3 scale, translation, skew;
                    glm::quat orientation;

                    // Always use left eye for the pose, as some effects (like the "darkness" effect in Mitterbach Tarmac night version)
                    // may render differently in each eye, especially if the object is far away, which makes it look awful.
                    // For the fog, the orientation is close enough for both eyes when always rendered with the same eye.
                    glm::decompose(g::vr->get_pose(LeftEye), scale, orientation, translation, skew, perspective);
                    const auto m = glm::transpose(glm::mat4_cast(glm::conjugate(orientation)) * orig);

                    auto ret = g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, reg, glm::value_ptr(m), Vector4fCount);
                    if (multiview_rendering_enabled()) {
                        ret |= g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, reg + 4, glm::value_ptr(m), Vector4fCount);
                    }

                    return ret;
                }
            } else if (multiview_rendering_enabled() && (StartRegister == 0 || StartRegister == 20)) {
                // Place the data in the multiview locations also when rendering the main menu (g::vr_render_target is not set)
                g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, reg, pConstantData, Vector4fCount);
                return g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, reg + 4, pConstantData, Vector4fCount);
            }
        } else if (multiview_rendering_enabled() && shader && !is_base_shader && (Vector4fCount == 4 || Vector4fCount == 5)) {
            // Multiview BTB shader data passing
            // Without multiview the matrices used are from the fixed function pipeline, so there's nothing to do.
            // However, with multiview we need again to first patch the shaders that were loaded after the game was
            // loaded, and afterwards we need to supply the data into the correct location.

            if (memcmp(pConstantData, &fixedfunction::current_world_matrix, sizeof(D3DMATRIX)) == 0) {
                // World matrix is the same for both perspectives, no need to do anything
                return g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, StartRegister, pConstantData, Vector4fCount);
            }

            std::vector<uint32_t> new_registers;
            std::vector<uint32_t>& regs = new_registers;
            auto patched_shader = g::patched_btb_shaders.find(shader);
            if (patched_shader != g::patched_btb_shaders.end()) {
                regs = patched_shader->second;
            }

            const auto this_register_patched = std::find(regs.cbegin(), regs.cend(), StartRegister) != regs.cend();

            if (!g::optimized_btb_shaders.contains(shader)) {
                bool patchSucceeded = true;
                if (regs.empty()) {
                    if (FAILED(g::d3d_vr->SetShaderConstantCount(shader, g::base_shader_data_end_register * 2))) {
                        dbg("Failed to extend BTB shader constant registers");
                        g::failed_multiview_btb_shaders++;
                        patchSucceeded = false;
                    } else if (!patch_spirv_shader(shader, StartRegister, g::base_shader_data_end_register, false)) {
                        dbg("Shader patch failed!");
                        g::failed_multiview_btb_shaders++;
                        patchSucceeded = false;
                    }
                } else if (!this_register_patched && !patch_spirv_shader(shader, StartRegister, g::base_shader_data_end_register, false)) {
                    dbg("Shader patch failed!");
                    g::failed_multiview_btb_shaders++;
                    patchSucceeded = false;
                }

                // Only record a register after its SPIR-V transformation succeeded.
                if (patchSucceeded && !this_register_patched) {
                    regs.push_back(StartRegister);
                    g::patched_btb_shaders[shader] = regs;
                }
            }


            // We don't know which one of these this matrix is, so we test it against known candidates.
            // These are the different ones passed to shaders, gathered from the BTB stage list of RSF.
            // Some custom stages might have other shaders that pass the data in a different format.
            // Those won't work without handling them here, but such is life.
            HRESULT matrixResult = D3D_OK;
            if (set_btb_vertex_shader_constant(This, StartRegister, g::base_shader_data_end_register, fixedfunction::current_projection_matrix, pConstantData, Vector4fCount, matrixResult)
                || set_btb_vertex_shader_constant(This, StartRegister, g::base_shader_data_end_register, fixedfunction::current_view_matrix, pConstantData, Vector4fCount, matrixResult)
                || set_btb_vertex_shader_constant(This, StartRegister, g::base_shader_data_end_register, fixedfunction::current_projview_matrix, pConstantData, Vector4fCount, matrixResult)
                || set_btb_vertex_shader_constant(This, StartRegister, g::base_shader_data_end_register, fixedfunction::current_viewproj_matrix, pConstantData, Vector4fCount, matrixResult)
                || set_btb_vertex_shader_constant(This, StartRegister, g::base_shader_data_end_register, fixedfunction::current_worldview_matrix, pConstantData, Vector4fCount, matrixResult)
                || set_btb_vertex_shader_constant(This, StartRegister, g::base_shader_data_end_register, fixedfunction::current_worldviewproj_matrix, pConstantData, Vector4fCount, matrixResult)) {
                return matrixResult;
            }

            const HRESULT leftResult = g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, reg, pConstantData, Vector4fCount);
            const HRESULT rightResult = g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, reg + 4, pConstantData, Vector4fCount);
            return FAILED(leftResult) ? leftResult : rightResult;
        }

        return g::hooks::set_vertex_shader_constant_f.call(g::d3d_dev, StartRegister, pConstantData, Vector4fCount);
    }

    static void update_btb_comparison_matrices(RenderTarget left, RenderTarget right)
    {
        const auto world = m4_from_d3d(fixedfunction::current_world_matrix);
        const auto update_matrices = [&world](RenderTarget tgt) {
            const auto proj = m4_from_d3d(fixedfunction::current_projection_matrix[tgt]);
            const auto view = m4_from_d3d(fixedfunction::current_view_matrix[tgt]);

            fixedfunction::current_projview_matrix[tgt] = d3d_from_m4(glm::transpose(view * proj));
            fixedfunction::current_viewproj_matrix[tgt] = d3d_from_m4(glm::transpose(proj * view));
            fixedfunction::current_worldview_matrix[tgt] = d3d_from_m4(glm::transpose(view * world));
            fixedfunction::current_worldviewproj_matrix[tgt] = d3d_from_m4(glm::transpose(proj * view * world));
        };

        update_matrices(left);
        update_matrices(right);
    }

    HRESULT __stdcall SetTransform(IDirect3DDevice9* This, D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix)
    {
        if (!g::game)
            return g::hooks::set_transform.call(This, State, pMatrix);

        if (g::vr_render_target) {
            const auto target = g::vr_render_target.value();

            if (State == D3DTS_PROJECTION) {
                // Store the inverse of the matrix passed to the function in order to cancel it out later on
                shader::current_projection_matrix = m4_from_d3d(*pMatrix);
                shader::current_projection_matrix_inverse = glm::inverse(shader::current_projection_matrix);

                // Change projection matrix to current VR render target matrix
                fixedfunction::current_projection_matrix[target] = d3d_from_m4(g::vr->get_projection(target));
                auto ret = g::hooks::set_transform.call(g::d3d_dev, D3DTS_PROJECTION_LEFT, &fixedfunction::current_projection_matrix[target]);

                if (multiview_rendering_enabled()) {
                    const auto multiview_target = render_target_counterpart(target);
                    fixedfunction::current_projection_matrix[multiview_target] = d3d_from_m4(g::vr->get_projection(multiview_target));
                    update_btb_comparison_matrices(target, multiview_target);
                    ret |= g::hooks::set_transform.call(g::d3d_dev, D3DTS_PROJECTION_RIGHT, &fixedfunction::current_projection_matrix[multiview_target]);
                }

                return ret;
            } else if (State == D3DTS_VIEW) {
                fixedfunction::current_view_matrix[target] = d3d_from_m4(
                    g::vr->get_eye_pos(target) * g::vr->get_pose(target) * g::flip_z_matrix * rbr::get_horizon_lock_matrix() * m4_from_d3d(*pMatrix));
                auto ret = g::hooks::set_transform.call(g::d3d_dev, D3DTS_VIEW_LEFT, &fixedfunction::current_view_matrix[target]);

                if (multiview_rendering_enabled()) {
                    const auto multiview_target = render_target_counterpart(target);
                    fixedfunction::current_view_matrix[multiview_target] = d3d_from_m4(
                        g::vr->get_eye_pos(multiview_target) * g::vr->get_pose(multiview_target) * g::flip_z_matrix * rbr::get_horizon_lock_matrix() * m4_from_d3d(*pMatrix));
                    update_btb_comparison_matrices(target, multiview_target);
                    ret |= g::hooks::set_transform.call(g::d3d_dev, D3DTS_VIEW_RIGHT, &fixedfunction::current_view_matrix[multiview_target]);
                }

                return ret;
            } else if (multiview_rendering_enabled() && State == D3DTS_WORLD) {
                // These matrices are needed for BTB shader constants in multiview case
                const auto multiview_target = render_target_counterpart(target);
                fixedfunction::current_world_matrix = *pMatrix;
                update_btb_comparison_matrices(target, multiview_target);
            }
        } else if (multiview_rendering_enabled()) {
            // Update left eye matrices as the 2D plane texture is drawn using the data from the left eye location
            if (State == D3DTS_PROJECTION) {
                fixedfunction::current_projection_matrix[LeftEye] = *pMatrix;
                fixedfunction::current_projection_matrix[FocusLeft] = *pMatrix;
                fixedfunction::current_projection_matrix[RightEye] = *pMatrix;
                fixedfunction::current_projection_matrix[FocusRight] = *pMatrix;

                update_btb_comparison_matrices(LeftEye, RightEye);
                update_btb_comparison_matrices(FocusLeft, FocusRight);

                // Update right view because some other plugin may draw on the 2D plane
                g::hooks::set_transform.call(g::d3d_dev, D3DTS_PROJECTION_RIGHT, pMatrix);
            } else if (State == D3DTS_VIEW) {
                fixedfunction::current_view_matrix[LeftEye] = *pMatrix;
                fixedfunction::current_view_matrix[FocusLeft] = *pMatrix;
                fixedfunction::current_view_matrix[RightEye] = *pMatrix;
                fixedfunction::current_view_matrix[FocusRight] = *pMatrix;

                update_btb_comparison_matrices(LeftEye, RightEye);
                update_btb_comparison_matrices(FocusLeft, FocusRight);

                // Update right view because some other plugin may draw on the 2D plane
                g::hooks::set_transform.call(g::d3d_dev, D3DTS_VIEW_RIGHT, pMatrix);
            } else if (State == D3DTS_WORLD) {
                fixedfunction::current_world_matrix = *pMatrix;
                update_btb_comparison_matrices(LeftEye, RightEye);
                update_btb_comparison_matrices(FocusLeft, FocusRight);
            }
        }

        return g::hooks::set_transform.call(g::d3d_dev, State, pMatrix);
    }

    HRESULT __stdcall BTB_SetRenderTarget(IDirect3DDevice9* This, DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
    {
        if (!g::game)
            return g::hooks::btb_set_render_target.call(This, RenderTargetIndex, pRenderTarget);

        // This was found purely by luck after testing all kinds of things.
        // For some reason, if this call is called with the original This pointer (from RBRRX)
        // plugins switching the render target (i.e. RBRHUD) will cause the stage geometry
        // to not be rendered at all. Routing the call to the D3D device created by openRBRVR,
        // it seems to work correctly.
        return g::d3d_dev->SetRenderTarget(RenderTargetIndex, pRenderTarget);
    }

    HRESULT __stdcall DrawPrimitive(IDirect3DDevice9* This, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
    {
        if (!g::game)
            return g::hooks::draw_primitive.call(This, PrimitiveType, StartVertex, PrimitiveCount);

        if (rbr::is_on_btb_stage()) {
            IDirect3DVertexShader9* shader = nullptr;
            if (SUCCEEDED(g::d3d_dev->GetVertexShader(&shader)) && shader) {
                bool should_skip_drawing = false;
                if (!multiview_rendering_enabled()) {
                    // Shader #39 causes strange "shadows" on BTB stages
                    // Clearly visible during CFH, and otherwise visible too when looking up
                    // Probably some projection matrix issue, but changing the projection matrix like
                    // we do normally had no effect, so on BTB stages we just won't draw this primitive with this shader.
                    // With multiview this bug seems to not occur so we also do this exception when multiview isn't enabled.
                    should_skip_drawing = g::base_game_shader_39 && shader == g::base_game_shader_39;
                }
                shader->Release();
                if (should_skip_drawing) {
                    return 0;
                }
            }
        }
        return g::hooks::draw_primitive.call(This, PrimitiveType, StartVertex, PrimitiveCount);
    }

    HRESULT __stdcall DrawIndexedPrimitive(IDirect3DDevice9* This, D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
    {
        if (!g::game)
            return g::hooks::draw_indexed_primitive.call(This, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);

        IDirect3DVertexShader9* shader = nullptr;
        IDirect3DBaseTexture9* texture = nullptr;
        const HRESULT shader_result = g::d3d_dev->GetVertexShader(&shader);
        const HRESULT texture_result = g::d3d_dev->GetTexture(0, &texture);
        if (FAILED(shader_result) || FAILED(texture_result)) {
            if (shader)
                shader->Release();
            if (texture)
                texture->Release();
            return g::hooks::draw_indexed_primitive.call(
                g::d3d_dev, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
        }
        if (g::vr_render_target && !shader && !texture) {
            if (!rbr::is_using_cockpit_camera()) {
                // Don't draw these if we're not in a cockpit camera.
                // In this mode, a black transparent square is drawn in front of the car
                // if car shadows are enabled.
                return 0;
            }
        } else {
            if (shader) {
                shader->Release();
            }
            if (texture)
                texture->Release();
            if (g::vr_render_target && rbr::is_rendering_wet_windscreen()) {
                const bool is_windscreen = BaseVertexIndex == 0 && NumVertices == 4;
                if (is_windscreen) {
                    // TODO: This method of detecting the windscreen is quite bad.
                    //
                    // It would be better to detect it from the texture I think.
                    // However, the texture is such that it is not loaded in the hooked
                    // load_texture function. This will do unless some rendering bugs are found.
                    //
                    // We can't render the windscreen for two reasons:
                    // - The windscreen needs to be rendered with cockpit projection to not clip it too early,
                    //   but doing so makes its Z-values be wrong compared to the stage. Maybe another projection
                    //   for it would work, but here comes the second point:
                    // - It looks pretty bad anyway, and it is just a 2D plane floating somewhere in front of the
                    //   opening for a windscreen. In VR where your FoV is large the effect does not really work
                    //   that well.
                    return 0;
                }
            }
        }
        return g::hooks::draw_indexed_primitive.call(g::d3d_dev, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
    }

    HRESULT __stdcall EndStateBlock(IDirect3DDevice9* This, IDirect3DStateBlock9** ppSB)
    {
        if (!g::game)
            return g::hooks::end_state_block.call(This, ppSB);

        const auto ret = g::hooks::end_state_block.call(This, ppSB);

        // EndStateBlock can fail without returning a state block. Keep the
        // hook installed so a later successful call can still be intercepted.
        if (SUCCEEDED(ret) && ppSB && *ppSB) {
            // State blocks normally share one vtable. Keep the existing hook
            // instead of trying to register the same MinHook target again.
            if (!g::hooks::apply_state_block.src) {
                auto vtbl = get_vtable<IDirect3DStateBlock9Vtbl>(*ppSB);
                try {
                    g::hooks::apply_state_block.install(vtbl->Apply, ApplyStateBlock);
                } catch (const std::runtime_error& e) {
                    dbg(e.what());
                    return ret;
                }
            }

            // A failed removal leaves EndStateBlock detoured. Keep its
            // trampoline and retry removal on the next successful call.
            const auto status = g::hooks::end_state_block.reset();
            if (status != MH_OK) {
                dbg(std::format("Could not remove EndStateBlock hook: {}", MH_StatusToString(status)));
            }
        }

        return ret;
    }

    HRESULT __stdcall ApplyStateBlock(IDirect3DStateBlock9* This)
    {
        if (!g::game)
            return g::hooks::apply_state_block.call(This);

        struct ApplyingStateBlockScope {
            bool previous;

            ApplyingStateBlockScope()
                : previous(g::applying_state_block) {
                g::applying_state_block = true;
            }

            ~ApplyingStateBlockScope() {
                g::applying_state_block = previous;
            }
        } scope;

        return g::hooks::apply_state_block.call(This);
    }

    HRESULT __stdcall SetRenderState(IDirect3DDevice9* This, D3DRENDERSTATETYPE State, DWORD Value)
    {
        if (!g::game)
            return g::hooks::set_render_state.call(This, State, Value);

        DWORD val = Value;

        if (rbr::should_use_reverse_z_buffer() && !g::applying_state_block) [[likely]] {
            if (State == D3DRS_ZFUNC) {
                // Invert the function if reverse Z buffer is in use
                switch (Value) {
                    case D3DCMP_LESSEQUAL:
                        val = D3DCMP_GREATEREQUAL;
                        break;
                    case D3DCMP_LESS:
                        val = D3DCMP_GREATER;
                        break;
                    case D3DCMP_GREATEREQUAL:
                        val = D3DCMP_LESSEQUAL;
                        break;
                    case D3DCMP_GREATER:
                        val = D3DCMP_LESS;
                        break;
                    default:
                        val = Value;
                }
            } else if (State == D3DRS_DEPTHBIAS) {
                // Invert depth bias if reversed Z buffer is in use
                float fval = -*reinterpret_cast<float*>(&Value);
                val = *reinterpret_cast<DWORD*>(&fval);
            }
        }

        return g::hooks::set_render_state.call(This, State, val);
    }

    HRESULT __stdcall Clear(IDirect3DDevice9* This, DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
    {
        if (!g::game)
            return g::hooks::clear.call(This, Count, pRects, Flags, Color, Z, Stencil);

        // Invert the Z value if reverse Z buffer is in use

        if (rbr::should_use_reverse_z_buffer()) [[likely]] {
            return g::hooks::clear.call(This, Count, pRects, Flags, Color, 1.0f - Z, Stencil);
        } else {
            return g::hooks::clear.call(This, Count, pRects, Flags, Color, Z, Stencil);
        }
    }

    HRESULT __stdcall Reset(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters)
    {
        if (!g::game)
            return g::hooks::reset.call(This, pPresentationParameters);

        if (This != g::d3d_dev || !g::vr || g::vr->get_runtime_type() != OPENVR)
            return g::hooks::reset.call(This, pPresentationParameters);

        auto* openvr = static_cast<OpenVR*>(g::vr);

        // Do not release any plugin-owned resource or forward Reset unless
        // the Bridge Server confirms that all Vulkan work has completed.
        const HRESULT shutdownResult = openvr->shutdown_device_resources(true);
        if (FAILED(shutdownResult))
            return shutdownResult;

        if (g::original_render_target) {
            g::original_render_target->Release();
            g::original_render_target = nullptr;
        }
        if (g::original_depth_stencil_target) {
            g::original_depth_stencil_target->Release();
            g::original_depth_stencil_target = nullptr;
        }
        g::current_2d_render_target.reset();
        g::vr_render_target.reset();
        g::vr_error = false;

        // Shader patch bookkeeping is frame/device state. Do not carry a
        // partially patched or pending optimization batch across Reset.
        g::patched_btb_shaders.clear();
        g::optimized_btb_shaders.clear();
        {
            std::lock_guard lock(g::deferred_shader_constants_mutex);
            g::deferred_shader_constants.clear();
        }

        const HRESULT resetResult = g::hooks::reset.call(This, pPresentationParameters);
        if (FAILED(resetResult)) {
            // The device is not usable after a failed Reset. Drop the VR
            // interface rather than letting later hooks access empty contexts.
            delete g::vr;
            g::vr = nullptr;
            return resetResult;
        }

        // Reset discards device state; restore the small set of states that
        // the plugin establishes during initial device creation.
        This->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, 1);
        for (auto sampler = D3DVERTEXTEXTURESAMPLER0;
             sampler <= D3DVERTEXTEXTURESAMPLER3; ++sampler) {
            This->SetSamplerState(sampler, D3DSAMP_MAXANISOTROPY, g::cfg.anisotropy);
        }

        IDirect3DSurface9* backBuffer = nullptr;
        D3DSURFACE_DESC backBufferDesc = {};
        HRESULT result = This->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (SUCCEEDED(result)) {
            result = backBuffer
                ? backBuffer->GetDesc(&backBufferDesc)
                : E_FAIL;
        }
        if (backBuffer) {
            backBuffer->Release();
            backBuffer = nullptr;
        }

        if (FAILED(result) || backBufferDesc.Width == 0 || backBufferDesc.Height == 0) {
            dbg(std::format("OpenVR reset recovery could not query the backbuffer (HRESULT 0x{:08X})",
                static_cast<uint32_t>(result)));
            delete g::vr;
            g::vr = nullptr;
            return resetResult;
        }

        result = CreateD3D9VR(This, &g::d3d_vr);
        if (FAILED(result) || !g::d3d_vr) {
            dbg(std::format("OpenVR reset recovery could not recreate the D3D9VR proxy (HRESULT 0x{:08X})",
                static_cast<uint32_t>(result)));
            delete g::vr;
            g::vr = nullptr;
            return resetResult;
        }

        try {
            openvr->init(This, &g::d3d_vr, backBufferDesc.Width, backBufferDesc.Height);
        } catch (const std::exception& e) {
            dbg(std::format("OpenVR reset recovery failed: {}", e.what()));
            delete g::vr;
            g::vr = nullptr;
        }

        return resetResult;
    }

    HRESULT __stdcall CreateDevice(
        IDirect3D9* This,
        UINT Adapter,
        D3DDEVTYPE DeviceType,
        HWND hFocusWindow,
        DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface)
    {
        if (!g::game)
            return g::hooks::create_device.call(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);

        // This plugin keeps one global D3D9 device and one set of device hooks.
        // A second CreateDevice call would replace the VR interface and render
        // resources while the existing hooks still target the first device.
        if (g::d3d_dev != nullptr) {
            dbg("A second D3D9 device is not supported by the global VR device state");
            if (ppReturnedDeviceInterface)
                *ppReturnedDeviceInterface = nullptr;
            return D3DERR_INVALIDCALL;
        }

        IDirect3DDevice9* dev = nullptr;

        pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        if (multiview_rendering_enabled()) {
            // Unused BehaviorFlag in D3D9 used here to indicate that multiview support for fixed-function pipeline should be enabled
            BehaviorFlags |= 0x10000;
        }

        auto ret = g::hooks::create_device.call(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, &dev);

        if (multiview_rendering_enabled()) {
            BehaviorFlags &= ~0x10000;
        }

        if (FAILED(ret)) {
            dbg("D3D initialization failed: CreateDevice");

            // OpenVR is initialized before CreateDevice; discard it if device creation fails.
            if (g::vr && g::vr->get_runtime_type() == OPENVR) {
                delete g::vr;
                g::vr = nullptr;
            }
            g::d3d_vr = nullptr;
            return ret;
        }

        D3DCAPS9 caps;
        dev->GetDeviceCaps(&caps);

        if (static_cast<int32_t>(caps.MaxAnisotropy) < g::cfg.anisotropy) {
            g::cfg.anisotropy = caps.MaxAnisotropy;
        }

        *ppReturnedDeviceInterface = dev;
        g::d3d_vr = nullptr;
        const auto vr_result = CreateD3D9VR(dev, &g::d3d_vr);
        if (FAILED(vr_result)) {
            if (g::d3d_vr) {
                g::d3d_vr->Release();
                g::d3d_vr = nullptr;
            }
            dbg(std::format("Direct3DCreateVR failed: 0x{:08X}", static_cast<uint32_t>(vr_result)));
        }

        dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, 1);
        for (auto i = D3DVERTEXTEXTURESAMPLER0; i <= D3DVERTEXTEXTURESAMPLER3; ++i) {
            dev->SetSamplerState(i, D3DSAMP_MAXANISOTROPY, g::cfg.anisotropy);
        }

        auto devvtbl = get_vtable<IDirect3DDevice9Vtbl>(dev);
        try {
            if (g::vr && g::vr->get_runtime_type() == OPENVR)
                g::hooks::reset.install(devvtbl->Reset, Reset);
            g::hooks::set_vertex_shader_constant_f.install(devvtbl->SetVertexShaderConstantF, SetVertexShaderConstantF);
            g::hooks::set_transform.install(devvtbl->SetTransform, SetTransform);
            g::hooks::present.install(devvtbl->Present, Present);
            g::hooks::create_vertex_shader.install(devvtbl->CreateVertexShader, CreateVertexShader);
            g::hooks::get_vertex_shader.install(devvtbl->GetVertexShader, GetVertexShader);
            g::hooks::set_vertex_shader.install(devvtbl->SetVertexShader, SetVertexShader);
            g::hooks::draw_indexed_primitive.install(devvtbl->DrawIndexedPrimitive, DrawIndexedPrimitive);
            g::hooks::draw_primitive.install(devvtbl->DrawPrimitive, DrawPrimitive);
            g::hooks::set_render_state.install(devvtbl->SetRenderState, SetRenderState);
            g::hooks::clear.install(devvtbl->Clear, Clear);
            g::hooks::end_state_block.install(devvtbl->EndStateBlock, EndStateBlock);
        } catch (const std::runtime_error& e) {
            dbg(e.what());
            MessageBoxA(hFocusWindow, e.what(), "Hooking failed", MB_OK);

            // Disable the complete device hook set before releasing anything
            // that its detours may access. A failed disable leaves the device
            // and VR objects quarantined for the remaining active detours.
            const bool hooksDisabled = disable_hooks(
                g::hooks::end_state_block,
                g::hooks::reset,
                g::hooks::clear,
                g::hooks::set_render_state,
                g::hooks::draw_primitive,
                g::hooks::draw_indexed_primitive,
                g::hooks::set_vertex_shader,
                g::hooks::get_vertex_shader,
                g::hooks::create_vertex_shader,
                g::hooks::present,
                g::hooks::set_transform,
                g::hooks::set_vertex_shader_constant_f);

            if (hooksDisabled) {
                // Removing a disabled hook may still fail. Its Hook object
                // retains ownership and a later install() can re-enable it.
                g::hooks::end_state_block.reset();
                g::hooks::reset.reset();
                g::hooks::clear.reset();
                g::hooks::set_render_state.reset();
                g::hooks::draw_primitive.reset();
                g::hooks::draw_indexed_primitive.reset();
                g::hooks::set_vertex_shader.reset();
                g::hooks::get_vertex_shader.reset();
                g::hooks::create_vertex_shader.reset();
                g::hooks::present.reset();
                g::hooks::set_transform.reset();
                g::hooks::set_vertex_shader_constant_f.reset();

                if (g::vr) {
                    delete g::vr;
                    g::vr = nullptr;
                } else if (g::d3d_vr) {
                    g::d3d_vr->Release();
                    g::d3d_vr = nullptr;
                }

                dev->Release();
            } else {
                // At least one detour may still be executing. Keep the device
                // reference and its global pointer alive rather than allowing
                // an active callback to dereference a released D3D9 device.
                g::d3d_dev = dev;
                dbg("Could not disable every D3D9 hook; retaining device and VR resources");
            }

            *ppReturnedDeviceInterface = nullptr;
            return E_FAIL;
        }

        g::d3d_dev = dev;

        try {
            if (g::vr) {
                const auto companion_window_width = pPresentationParameters->BackBufferWidth;
                const auto companion_window_height = pPresentationParameters->BackBufferHeight;

                if (g::vr->get_runtime_type() == OPENXR) {
                    reinterpret_cast<OpenXR*>(g::vr)->init(dev, &g::d3d_vr, companion_window_width, companion_window_height);
                } else {
                    reinterpret_cast<OpenVR*>(g::vr)->init(dev, &g::d3d_vr, companion_window_width, companion_window_height);
                }
            }
        } catch (const std::exception& e) {
            MessageBoxA(hFocusWindow, e.what(), "VR init failed", MB_OK);

            // Either runtime may have allocated render-context or interop
            // resources before initialization failed. Tear down the interface
            // and its Bridge proxy so the device can continue in flat mode.
            if (g::vr) {
                delete g::vr;
                g::vr = nullptr;
            }
            if (g::d3d_vr) {
                g::d3d_vr->Release();
                g::d3d_vr = nullptr;
            }
        }
        // Initialize this pointer here, as it's too early to do this in openRBRVR constructor
        auto handle = GetModuleHandle("Plugins\\rbr_rx.dll");
        if (handle) {
            auto rx_addr = reinterpret_cast<uintptr_t>(handle);
            g::btb_track_status_ptr = reinterpret_cast<uint8_t*>(rx_addr + rbr_rx::TRACK_STATUS_OFFSET);

            IDirect3DDevice9Vtbl* rbrrxdev = reinterpret_cast<IDirect3DDevice9Vtbl*>(rx_addr + rbr_rx::DEVICE_VTABLE_OFFSET);
            try {
                g::hooks::btb_set_render_target.install(rbrrxdev->SetRenderTarget, BTB_SetRenderTarget);
            } catch (const std::runtime_error& e) {
                dbg(e.what());
                MessageBoxA(hFocusWindow, e.what(), "Hooking failed", MB_OK);
            }
        }

        return ret;
    }

    IDirect3D9* __stdcall Direct3DCreate9(UINT SDKVersion)
    {
        if (!g::game)
            return g::hooks::create.call(SDKVersion);

        // The plugin keeps one global VR runtime and one global D3D9 device
        // hook set. Do not replace an existing runtime on repeated factory calls.
        bool createdRuntime = false;
        if (!g::vr) {
            if (g::cfg.runtime == OPENXR) {
                // OpenXR must be initialized before calling Direct3DCreate9
                // because it will query extensions when initializing DXVK.
                try {
                    g::vr = new OpenXR();
                    createdRuntime = true;
                } catch (const std::exception& e) {
                    MessageBoxA(nullptr, e.what(), "OpenXR init failed", MB_OK);
                }
            } else {
                // Initialize the x86 OpenVR Background session before the
                // original D3D9 factory call. The x64 Bridge Server owns the
                // independent Scene session, compositor and Vulkan extension discovery.
                try {
                    g::vr = new OpenVR();
                    createdRuntime = true;
                } catch (const std::exception& e) {
                    MessageBoxA(nullptr, e.what(), "OpenVR init failed", MB_OK);
                }
            }
        }

        auto d3d = g::hooks::create.call(SDKVersion);
        if (!d3d) {
            dbg("Could not initialize Vulkan");
            if (createdRuntime && g::vr) {
                delete g::vr;
                g::vr = nullptr;
            }
            return nullptr;
        }

        auto d3d_vtbl = get_vtable<IDirect3D9Vtbl>(d3d);
        // The CreateDevice implementation is normally shared by D3D9 factory
        // instances. Keep its existing MinHook trampoline instead of removing
        // and recreating the global hook for every Direct3DCreate9 call.
        if (g::hooks::create_device.src) {
            if (g::hooks::create_device.src != d3d_vtbl->CreateDevice) {
                dbg("A different D3D9 CreateDevice implementation is already hooked");
                d3d->Release();
                if (createdRuntime && g::vr) {
                    delete g::vr;
                    g::vr = nullptr;
                }
                return nullptr;
            }
        } else {
            try {
                g::hooks::create_device.install(d3d_vtbl->CreateDevice, CreateDevice);
            } catch (const std::runtime_error& e) {
                dbg(e.what());
                MessageBoxA(nullptr, e.what(), "Hooking failed", MB_OK);
                // Returning an unhooked factory would let the game create a
                // device that never enters the plugin's VR/D3D initialization.
                d3d->Release();
                if (createdRuntime && g::vr) {
                    delete g::vr;
                    g::vr = nullptr;
                }
                return nullptr;
            }
        }
        return d3d;
    }
}
