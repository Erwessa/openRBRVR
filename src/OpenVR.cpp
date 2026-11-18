#include "OpenVR.hpp"

#include <stdexcept>
#include "Config.hpp"
#include "Dx.hpp"
#include "Globals.hpp"

#include <openvr.h>

static HRESULT get_texture_surface(IDirect3DTexture9* texture, IDirect3DSurface9** surface)
{
    if (!surface)
        return E_POINTER;

    *surface = nullptr;
    if (!texture)
        return D3DERR_INVALIDCALL;

    const HRESULT result = texture->GetSurfaceLevel(0, surface);
    if (FAILED(result) || !*surface) {
        if (*surface) {
            (*surface)->Release();
            *surface = nullptr;
        }
        return FAILED(result) ? result : E_FAIL;
    }
    return result;
}

static constexpr M4 m4_from_steamvr_matrix(const float* m)
{
    return M4(
        { m[0], m[4], m[8], 0.0f },
        { m[1], m[5], m[9], 0.0f },
        { m[2], m[6], m[10], 0.0f },
        { m[3], m[7], m[11], 1.0f });
}

OpenVR::OpenVR()
{
    // Initialize the x86 OpenVR client before Direct3DCreate9 starts Vulkan.
    // The x64 Bridge Server owns the independent Scene session and compositor.
    if (!vr::VR_IsHmdPresent())
        throw std::runtime_error("OpenVR HMD is not present");

    vr::EVRInitError error = vr::VRInitError_None;
    auto* system = vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None || system == nullptr) {
        const auto* description = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        throw std::runtime_error(std::format("OpenVR background initialization failed: {}",
            description ? description : "unknown error"));
    }

    m_backgroundSessionInitialized = true;
}

M4 OpenVR::get_projection_matrix(RenderTarget eye, float z_near, float z_far, bool reverse_z) const
{
    const auto& mat = m_frameState.ProjectionMatrix[eye];
    auto result = M4(
        { mat[0], mat[4], mat[8], mat[12] },
        { mat[1], mat[5], mat[9], mat[13] },
        { mat[2], mat[6], mat[10], mat[14] },
        { mat[3], mat[7], mat[11], mat[15] });

    if (reverse_z) [[likely]] {
        result[2][2] = z_near / (z_near - z_far);
        result[2][3] = -1.0f;
        result[3][2] = z_near;
    }

    return result;
}

void OpenVR::init(IDirect3DDevice9* dev, IDirect3DVR9** vrdev,
                  uint32_t companionWindowWidth, uint32_t companionWindowHeight)
{
    if (!dev || !vrdev || !*vrdev || !g::d3d_vr)
        throw std::runtime_error("OpenVR initialization failed: D3D9VR proxy is unavailable");
    if (!render_contexts.empty() || current_render_context)
        throw std::runtime_error("OpenVR initialization refused while old device resources remain");

    // WaitGetPoses may succeed before the HMD has a valid tracked pose.
    // Seed the retained-pose fallback so the first such frame never uses
    // uninitialized GLM matrices.
    hmd_pose[LeftEye] = glm::identity<M4>();
    hmd_pose[RightEye] = glm::identity<M4>();
    eye_pos[LeftEye] = glm::identity<M4>();
    eye_pos[RightEye] = glm::identity<M4>();

    const HRESULT sceneResult = g::d3d_vr->InitializeOpenVRScene();
    if (FAILED(sceneResult))
        throw std::runtime_error(std::format("OpenVR scene initialization failed (HRESULT 0x{:08X})",
            static_cast<uint32_t>(sceneResult)));

    const HRESULT frameResult = g::d3d_vr->AcquireOpenVRFrame(&m_frameState);
    if (FAILED(frameResult) || m_frameState.Version != D3D9_OPENVR_FRAME_STATE_VERSION
        || m_frameState.RecommendedWidth == 0 || m_frameState.RecommendedHeight == 0)
        throw std::runtime_error(std::format("OpenVR initial frame query failed (HRESULT 0x{:08X})",
            static_cast<uint32_t>(frameResult)));

    const auto width = m_frameState.RecommendedWidth;
    const auto height = m_frameState.RecommendedHeight;

    // Validate every configured multiview context before allocating any
    // layered render targets. A failed capability check must not leave a
    // partially initialized set of contexts that later fails on a stage switch.
    for (const auto& gfx : g::cfg.gfx) {
        if (!gfx.second.multiview_rendering)
            continue;

        const HRESULT multiviewResult = g::d3d_vr->EnableMultiView(true);
        if (FAILED(multiviewResult))
            throw std::runtime_error(std::format(
                "OpenVR render context '{}' requires multiview, but the D3D9 runtime does not support it (HRESULT 0x{:08X})",
                gfx.first, static_cast<uint32_t>(multiviewResult)));
    }

    const HRESULT disableMultiviewResult = g::d3d_vr->EnableMultiView(false);
    if (FAILED(disableMultiviewResult))
        throw std::runtime_error(std::format(
            "OpenVR could not reset multiview state before creating render contexts (HRESULT 0x{:08X})",
            static_cast<uint32_t>(disableMultiviewResult)));

    const auto defaultMsaa = g::cfg.gfx.at("default").msaa.value_or(D3DMULTISAMPLE_NONE);

    try {
        for (const auto& gfx : g::cfg.gfx) {
            const auto supersampling = gfx.second.supersampling;
            const auto wss = static_cast<uint32_t>(width * supersampling);
            const auto hss = static_cast<uint32_t>(height * supersampling);
            RenderContext ctx = {
                .width = { wss, wss },
                .height = { hss, hss },
                .msaa = gfx.second.msaa.value_or(defaultMsaa),
                .quad_view_rendering = false,
                .multiview_rendering = gfx.second.multiview_rendering,
            };
            auto& storedContext = render_contexts[gfx.first];
            storedContext = ctx;
            init_surfaces(dev, storedContext, companionWindowWidth, companionWindowHeight);
        }
        set_render_context("default");
        if (!update_vr_poses())
            throw std::runtime_error("OpenVR could not acquire its initial tracking snapshot");
    } catch (const std::runtime_error&) {
        throw;
    }

    dbg("VR init successful\n");
}

void OpenVR::shutdown_device_resources()
{
    discard_prepared_frames();

    if (g::d3d_vr) {
        const HRESULT idleResult = g::d3d_vr->WaitDeviceIdle(true);
        if (FAILED(idleResult)) {
            dbg(std::format("OpenVR device-idle wait failed before resource release (HRESULT 0x{:08X})",
                static_cast<uint32_t>(idleResult)));
        }
    }

    for (auto& contextEntry : render_contexts) {
        auto& ctx = contextEntry.second;
        for (size_t i = 0; i < 6; i++) {
            if (ctx.dx_texture[i]) {
                ctx.dx_texture[i]->Release();
                ctx.dx_texture[i] = nullptr;
            }
            if (ctx.dx_surface[i]) {
                ctx.dx_surface[i]->Release();
                ctx.dx_surface[i] = nullptr;
            }
            if (ctx.dx_depth_stencil_surface[i]) {
                ctx.dx_depth_stencil_surface[i]->Release();
                ctx.dx_depth_stencil_surface[i] = nullptr;
            }
            if (i < 4 && ctx.dx_shared_handle[i]
             && ctx.dx_shared_handle[i] != INVALID_HANDLE_VALUE) {
                CloseHandle(ctx.dx_shared_handle[i]);
                ctx.dx_shared_handle[i] = nullptr;
            }
        }

        if (ctx.overlay_border) {
            ctx.overlay_border->Release();
            ctx.overlay_border = nullptr;
        }
    }

    current_render_context = nullptr;
    current_render_context_name.clear();
    render_contexts.clear();
    release_shared_vertex_buffers();

    if (g::d3d_vr) {
        g::d3d_vr->Release();
        g::d3d_vr = nullptr;
    }

    m_frameState = {};
    m_framesPrepared = false;
}

void OpenVR::shutdown_vr()
{
    shutdown_device_resources();

    // The x86 Background session belongs to the plugin process and survives
    // device Reset, but is released during final runtime shutdown.
    if (m_backgroundSessionInitialized) {
        vr::VR_Shutdown();
        m_backgroundSessionInitialized = false;
    }
}

void OpenVR::set_render_context(const std::string& name)
{
    VRInterface::set_render_context(name);

    // The Bridge device is created before its D3D9VR proxy, so the initial
    // CreateDevice call cannot reliably seed the fixed-function multiview bit.
    // Reapply the selected context's effective mode whenever it becomes active.
    if (g::d3d_vr) {
        const HRESULT result = g::d3d_vr->EnableMultiView(dx::multiview_rendering_enabled());
        if (FAILED(result))
            throw std::runtime_error(std::format(
                "Could not apply OpenVR multiview mode for render context '{}': HRESULT 0x{:08X}",
                name, static_cast<uint32_t>(result)));
    }

    m_framesPrepared = false;
}

void OpenVR::prepare_frames_for_hmd(IDirect3DDevice9* dev)
{
    if (m_framesPrepared)
        discard_prepared_frames();

    if (!current_render_context || !dev || !g::d3d_vr) {
        dbg("Skipping OpenVR frame preparation: render context or D3D9VR proxy unavailable");
        return;
    }

    IDirect3DSurface9 *leftEye = nullptr, *rightEye = nullptr;
    if (FAILED(get_texture_surface(current_render_context->dx_texture[LeftEye], &leftEye))) {
        dbg("Failed to obtain left-eye render surface");
        return;
    }
    if (FAILED(get_texture_surface(current_render_context->dx_texture[RightEye], &rightEye))) {
        dbg("Failed to obtain right-eye render surface");
        leftEye->Release();
        return;
    }

    HRESULT result = D3D_OK;
    if (dx::multiview_rendering_enabled()) {
        if (!current_render_context->dx_surface[LeftEye]) {
            result = D3DERR_INVALIDCALL;
        } else {
            IDirect3DSurface9* eyes[2] = { leftEye, rightEye };
            result = g::d3d_vr->CopySurfaceLayers(
                current_render_context->dx_surface[LeftEye], eyes, 2);
        }
    } else if (current_render_context->msaa != D3DMULTISAMPLE_NONE) {
        if (!current_render_context->dx_surface[LeftEye]
         || !current_render_context->dx_surface[RightEye]) {
            result = D3DERR_INVALIDCALL;
        } else {
            result = dev->StretchRect(current_render_context->dx_surface[LeftEye],
                nullptr, leftEye, nullptr, D3DTEXF_NONE);
            if (SUCCEEDED(result))
                result = dev->StretchRect(current_render_context->dx_surface[RightEye],
                    nullptr, rightEye, nullptr, D3DTEXF_NONE);
        }
    }

    leftEye->Release();
    rightEye->Release();

    if (FAILED(result)) {
        dbg(std::format("Failed to prepare OpenVR eye images (HRESULT 0x{:08X})",
            static_cast<uint32_t>(result)));
        return;
    }

    // No Vulkan handles or OpenVR objects cross IPC. The x64 D3D9VR server
    // transitions both images and submits them to SteamVR in one transaction.
    m_framesPrepared = true;
}

void OpenVR::discard_prepared_frames()
{
    // Image layout transitions are now performed atomically by the x64
    // SubmitOpenVRFrame operation, so an unsubmitted x86 frame needs no cleanup.
    m_framesPrepared = false;
}

void OpenVR::submit_frames_to_hmd(IDirect3DDevice9*)
{
    if (!m_framesPrepared || !g::d3d_vr || !current_render_context) {
        m_framesPrepared = false;
        return;
    }

    IDirect3DSurface9 *leftEye = nullptr, *rightEye = nullptr;
    HRESULT result = get_texture_surface(current_render_context->dx_texture[LeftEye], &leftEye);
    if (SUCCEEDED(result))
        result = get_texture_surface(current_render_context->dx_texture[RightEye], &rightEye);

    if (SUCCEEDED(result))
        result = g::d3d_vr->SubmitOpenVRFrame(leftEye, rightEye);

    if (leftEye)
        leftEye->Release();
    if (rightEye)
        rightEye->Release();

    m_framesPrepared = false;
    if (FAILED(result))
        dbg(std::format("OpenVR frame submission failed (HRESULT 0x{:08X})",
            static_cast<uint32_t>(result)));
}

bool OpenVR::update_vr_poses()
{
    if (!g::d3d_vr)
        return false;

    D3D9_OPENVR_FRAME_STATE next = {};
    const HRESULT result = g::d3d_vr->AcquireOpenVRFrame(&next);
    if (FAILED(result) || next.Version != D3D9_OPENVR_FRAME_STATE_VERSION)
        return false;

    m_frameState = next;
    if (m_frameState.Flags & 1u) {
        hmd_pose[LeftEye] = glm::inverse(
            m4_from_steamvr_matrix(m_frameState.DeviceToAbsoluteTracking));
        if (g::cfg.threedof)
            m4_to_3dof(hmd_pose[LeftEye]);
        hmd_pose[RightEye] = hmd_pose[LeftEye];
    }

    eye_pos[LeftEye] = glm::inverse(m4_from_steamvr_matrix(m_frameState.EyeToHead[LeftEye]));
    eye_pos[RightEye] = glm::inverse(m4_from_steamvr_matrix(m_frameState.EyeToHead[RightEye]));

    projection[LeftEye] = get_projection_matrix(
        LeftEye, z_near, z_far, rbr::should_use_reverse_z_buffer());
    projection[RightEye] = get_projection_matrix(
        RightEye, z_near, z_far, rbr::should_use_reverse_z_buffer());

    return true;
}

FrameTimingInfo OpenVR::get_frame_timing()
{
    FrameTimingInfo result = {};
    if (!g::d3d_vr)
        return result;

    D3D9_OPENVR_FRAME_TIMING timing = {};
    if (SUCCEEDED(g::d3d_vr->GetOpenVRFrameTiming(&timing))) {
        result.gpu_pre_submit = timing.GpuPreSubmit;
        result.gpu_post_submit = timing.GpuPostSubmit;
        result.compositor_gpu = timing.CompositorGpu;
        result.compositor_cpu = timing.CompositorCpu;
        result.compositor_submit_frame = timing.CompositorSubmitFrame;
        result.gpu_total = timing.GpuTotal;
        result.frame_interval = timing.FrameInterval;
        result.wait_for_present = timing.WaitForPresent;
        result.cpu_present_call = timing.CpuPresentCall;
        result.cpu_wait_for_present = timing.CpuWaitForPresent;
        result.reprojection_flags = timing.ReprojectionFlags;
        result.mispresented_frames = timing.MispresentedFrames;
        result.dropped_frames = timing.DroppedFrames;
    }
    return result;
}

void OpenVR::reset_view()
{
    if (!g::d3d_vr || FAILED(g::d3d_vr->ResetOpenVRView()))
        dbg("Could not reset OpenVR view through the x64 Bridge Server");
}
