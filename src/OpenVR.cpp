#include "OpenVR.hpp"
#include "Config.hpp"
#include "Dx.hpp"
#include "Globals.hpp"

static constexpr std::string vr_compositor_error_str(vr::VRCompositorError e)
{
    switch (e) {
        case vr::VRCompositorError::VRCompositorError_AlreadySet:
            return "AlreadySet";
        case vr::VRCompositorError::VRCompositorError_AlreadySubmitted:
            return "AlreadySubmitted";
        case vr::VRCompositorError::VRCompositorError_DoNotHaveFocus:
            return "DoNotHaveFocus";
        case vr::VRCompositorError::VRCompositorError_IncompatibleVersion:
            return "IncompatibleVersion";
        case vr::VRCompositorError::VRCompositorError_IndexOutOfRange:
            return "IndexOutOfRange";
        case vr::VRCompositorError::VRCompositorError_InvalidBounds:
            return "InvalidBounds";
        case vr::VRCompositorError::VRCompositorError_InvalidTexture:
            return "InvalidTexture";
        case vr::VRCompositorError::VRCompositorError_IsNotSceneApplication:
            return "IsNotSceneApplication";
        case vr::VRCompositorError::VRCompositorError_None:
            return "None";
        case vr::VRCompositorError::VRCompositorError_RequestFailed:
            return "RequestFailed";
        case vr::VRCompositorError::VRCompositorError_SharedTexturesNotSupported:
            return "SharedTexturesNotSupported";
        case vr::VRCompositorError::VRCompositorError_TextureIsOnWrongDevice:
            return "TextureIsOnWrongDevice";
        case vr::VRCompositorError::VRCompositorError_TextureUsesUnsupportedFormat:
            return "TextureUsesUnsupportedFormat";
        default:
            return "";
    }
}

constexpr M4 OpenVR::get_projection_matrix(RenderTarget eye, float z_near, float z_far, bool reverse_z)
{
    vr::HmdMatrix44_t mat = hmd->GetProjectionMatrix(static_cast<vr::EVREye>(eye), z_near, z_far);

    auto ret = M4(
        { mat.m[0][0], mat.m[1][0], mat.m[2][0], mat.m[3][0] },
        { mat.m[0][1], mat.m[1][1], mat.m[2][1], mat.m[3][1] },
        { mat.m[0][2], mat.m[1][2], mat.m[2][2], mat.m[3][2] },
        { mat.m[0][3], mat.m[1][3], mat.m[2][3], mat.m[3][3] });

    if (reverse_z) [[likely]] {
        ret[2][2] = z_near / (z_near - z_far);
        ret[2][3] = -1.0f;
        ret[3][2] = z_near;
    }

    return ret;
}

constexpr static M4 m4_from_steamvr_matrix(const vr::HmdMatrix34_t& m)
{
    return M4(
        { m.m[0][0], m.m[1][0], m.m[2][0], 0.0 },
        { m.m[0][1], m.m[1][1], m.m[2][1], 0.0 },
        { m.m[0][2], m.m[1][2], m.m[2][2], 0.0 },
        { m.m[0][3], m.m[1][3], m.m[2][3], 1.0f });
}

void OpenVR::init(IDirect3DDevice9* dev, IDirect3DVR9** vrdev, uint32_t companionWindowWidth, uint32_t companionWindowHeight)
{
    // WaitGetPoses might access the Vulkan queue so we need to lock it
    g::d3d_vr->LockSubmissionQueue();
    if (auto e = compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0); e != vr::VRCompositorError_None) {
        dbg("Could not get VR poses");
    }
    g::d3d_vr->UnlockSubmissionQueue();

    uint32_t w, h;
    hmd->GetRecommendedRenderTargetSize(&w, &h);

    // This can also be used to get a (slightly different) VR display size, if need arises
    // if (vr::IVRExtendedDisplay* VRExtDisplay = vr::VRExtendedDisplay()) {
    //	int32_t x, y;
    // 	VRExtDisplay->GetWindowBounds(&x, &y, &w, &h);
    // }

    try {
        for (const auto& gfx : g::cfg.gfx) {
            auto supersampling = gfx.second.supersampling;
            auto wss = static_cast<uint32_t>(w * supersampling);
            auto hss = static_cast<uint32_t>(h * supersampling);
            RenderContext ctx = {
                .width = { wss, wss },
                .height = { hss, hss },
                .msaa = gfx.second.msaa.value_or(g::cfg.gfx["default"].msaa.value()),
                .quad_view_rendering = false,
                .multiview_rendering = gfx.second.multiview_rendering,
            };
            auto& storedContext = render_contexts[gfx.first];
            storedContext = ctx;
            init_surfaces(dev, storedContext, companionWindowWidth, companionWindowHeight);
        }
        set_render_context("default");
    } catch (const std::runtime_error& e) {
        dbg(e.what());
        MessageBoxA(nullptr, e.what(), "VR init error", MB_OK);
        return;
    }

    dbg("VR init successful\n");
}

OpenVR::OpenVR()
    : hmd(nullptr)
    , compositor(nullptr)
{
    if (!vr::VR_IsHmdPresent()) {
        throw std::runtime_error("HMD not present, not initializing OpenVR");
    }

    vr::EVRInitError e = vr::VRInitError_None;
    hmd = vr::VR_Init(&e, vr::VRApplication_Scene);
    if (!hmd || e != vr::VRInitError_None) {
        throw std::runtime_error(std::format("VR init failed: {}", vr::VR_GetVRInitErrorAsEnglishDescription(e)));
    }

    compositor = vr::VRCompositor();
    if (!compositor) {
        vr::VR_Shutdown();
        throw std::runtime_error("Could not initialize VR compositor");
    }
    compositor->SetTrackingSpace(vr::ETrackingUniverseOrigin::TrackingUniverseSeated);

    eye_pos[LeftEye] = glm::inverse(m4_from_steamvr_matrix(hmd->GetEyeToHeadTransform(static_cast<vr::EVREye>(LeftEye))));
    eye_pos[RightEye] = glm::inverse(m4_from_steamvr_matrix(hmd->GetEyeToHeadTransform(static_cast<vr::EVREye>(RightEye))));
}

void OpenVR::shutdown_vr()
{
    if (hmd || compositor) {
        vr::VR_Shutdown();
        hmd = nullptr;
        compositor = nullptr;
    }

    if (g::d3d_vr) {
        g::d3d_vr->WaitDeviceIdle(true);
    }

    for (auto& contextEntry : render_contexts) {
        auto& ctx = contextEntry.second;
        for (size_t i = 0; i < 6; ++i) {
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

    render_contexts.clear();
    m_framesPrepared = false;
    m_textureDescriptorsValid = false;
}

void OpenVR::set_render_context(const std::string& name)
{
    VRInterface::set_render_context(name);
    m_textureDescriptorsValid = false;

    IDirect3DSurface9 *left_eye, *right_eye;
    if (current_render_context->dx_texture[LeftEye]->GetSurfaceLevel(0, &left_eye) != D3D_OK) {
        dbg("Failed to get left eye surface");
        return;
    }
    if (current_render_context->dx_texture[RightEye]->GetSurfaceLevel(0, &right_eye) != D3D_OK) {
        dbg("Failed to get right eye surface");
        left_eye->Release();
        return;
    }

    const HRESULT leftDescResult = g::d3d_vr->GetVRDesc(left_eye, &dxvk_texture[LeftEye]);
    const HRESULT rightDescResult = leftDescResult == D3D_OK
        ? g::d3d_vr->GetVRDesc(right_eye, &dxvk_texture[RightEye])
        : leftDescResult;

    left_eye->Release();
    right_eye->Release();

    if (leftDescResult != D3D_OK || rightDescResult != D3D_OK) {
        dbg("Failed to get eye texture descriptor");
        return;
    }

    const auto& left = dxvk_texture[LeftEye];
    const auto& right = dxvk_texture[RightEye];
    if (left.Image == 0 || right.Image == 0
     || left.Image == right.Image
     || left.SampleCount != VK_SAMPLE_COUNT_1_BIT
     || right.SampleCount != VK_SAMPLE_COUNT_1_BIT
     || left.Width == 0 || left.Height == 0
     || left.Width != right.Width || left.Height != right.Height
     || left.Format != right.Format
     || left.Device != right.Device
     || left.PhysicalDevice != right.PhysicalDevice
     || left.Instance != right.Instance
     || left.Queue != right.Queue
     || left.QueueFamilyIndex != right.QueueFamilyIndex) {
        dbg("OpenVR eye texture descriptors are inconsistent or not single-sampled");
        return;
    }

    openvr_texture[LeftEye].handle = reinterpret_cast<void*>(&dxvk_texture[LeftEye]);
    openvr_texture[LeftEye].eType = vr::TextureType_Vulkan;
    openvr_texture[LeftEye].eColorSpace = vr::ColorSpace_Auto;
    openvr_texture[RightEye].handle = reinterpret_cast<void*>(&dxvk_texture[RightEye]);
    openvr_texture[RightEye].eType = vr::TextureType_Vulkan;
    openvr_texture[RightEye].eColorSpace = vr::ColorSpace_Auto;
    m_textureDescriptorsValid = true;
}

void OpenVR::prepare_frames_for_hmd(IDirect3DDevice9* dev)
{
    m_framesPrepared = false;
    if (!m_textureDescriptorsValid) {
        dbg("Skipping OpenVR frame preparation because eye texture descriptors are invalid");
        return;
    }

    IDirect3DSurface9 *left_eye = nullptr, *right_eye = nullptr;
    if (current_render_context->dx_texture[LeftEye]->GetSurfaceLevel(0, &left_eye) != D3D_OK) {
        dbg("Failed to get left eye surface");
        return;
    }
    if (current_render_context->dx_texture[RightEye]->GetSurfaceLevel(0, &right_eye) != D3D_OK) {
        dbg("Failed to get right eye surface");
        left_eye->Release();
        return;
    }

    HRESULT prepareResult = D3D_OK;
    if (dx::multiview_rendering_enabled()) {
        IDirect3DSurface9* eyes[2] = { left_eye, right_eye };
        prepareResult = g::d3d_vr->CopySurfaceLayers(current_render_context->dx_surface[LeftEye], eyes, 2);
    } else if (current_render_context->msaa != D3DMULTISAMPLE_NONE) {
        prepareResult = dev->StretchRect(current_render_context->dx_surface[LeftEye], nullptr, left_eye, nullptr, D3DTEXF_NONE);
        if (prepareResult == D3D_OK)
            prepareResult = dev->StretchRect(current_render_context->dx_surface[RightEye], nullptr, right_eye, nullptr, D3DTEXF_NONE);
    }

    if (prepareResult != D3D_OK) {
        dbg("Failed to prepare eye textures for OpenVR submission");
        left_eye->Release();
        right_eye->Release();
        return;
    }

    // DXVK must transition each submitted image to TRANSFER_SRC_OPTIMAL
    // before the OpenVR compositor accesses the Vulkan queue.
    const HRESULT leftTransfer = g::d3d_vr->TransferSurfaceForVR(left_eye);
    const HRESULT rightTransfer = leftTransfer == D3D_OK
        ? g::d3d_vr->TransferSurfaceForVR(right_eye)
        : leftTransfer;

    left_eye->Release();
    right_eye->Release();

    if (leftTransfer != D3D_OK || rightTransfer != D3D_OK) {
        dbg("Failed to prepare eye image layouts for OpenVR submission");

        // If the first eye was queued successfully, close an empty VR submit
        // section so DXVK restores its layout before the next game frame.
        if (leftTransfer == D3D_OK) {
            g::d3d_vr->BeginVRSubmit();
            g::d3d_vr->EndVRSubmit();
        }
        return;
    }

    m_framesPrepared = true;
}

void OpenVR::submit_frames_to_hmd(IDirect3DDevice9* dev)
{
    if (!m_framesPrepared) {
        dbg("Skipping OpenVR submission because eye textures were not prepared");
        return;
    }
    m_framesPrepared = false;

    g::d3d_vr->BeginVRSubmit();
    if (auto e = compositor->Submit(static_cast<vr::EVREye>(LeftEye), &openvr_texture[LeftEye]); e != vr::VRCompositorError_None) [[unlikely]] {
        dbg(std::format("Compositor error: {}", vr_compositor_error_str(e)));
    }
    if (auto e = compositor->Submit(static_cast<vr::EVREye>(RightEye), &openvr_texture[RightEye]); e != vr::VRCompositorError_None) [[unlikely]] {
        dbg(std::format("Compositor error: {}", vr_compositor_error_str(e)));
    }
    compositor->PostPresentHandoff();
    g::d3d_vr->EndVRSubmit();
}

bool OpenVR::update_vr_poses()
{
    // WaitGetPoses might access the Vulkan queue so we need to lock it
    g::d3d_vr->LockSubmissionQueue();
    if (auto e = compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0); e != vr::VRCompositorError_None) {
        dbg("Could not get VR poses");
        g::d3d_vr->UnlockSubmissionQueue();
        return false;
    }
    g::d3d_vr->UnlockSubmissionQueue();

    auto pose = &poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (pose->bPoseIsValid) {
        hmd_pose[LeftEye] = glm::inverse(m4_from_steamvr_matrix(pose->mDeviceToAbsoluteTracking));

        if (g::cfg.threedof) {
            m4_to_3dof(hmd_pose[LeftEye]);
        }

        hmd_pose[RightEye] = hmd_pose[LeftEye];
    }

    projection[LeftEye] = get_projection_matrix(LeftEye, z_near, z_far, rbr::should_use_reverse_z_buffer());
    projection[RightEye] = get_projection_matrix(RightEye, z_near, z_far, rbr::should_use_reverse_z_buffer());

    return true;
}

FrameTimingInfo OpenVR::get_frame_timing()
{
    FrameTimingInfo ret = { 0 };

    vr::Compositor_FrameTiming t {
        .m_nSize = sizeof(vr::Compositor_FrameTiming)
    };
    if (compositor->GetFrameTiming(&t)) {
        ret.gpu_pre_submit = t.m_flPreSubmitGpuMs;
        ret.gpu_post_submit = t.m_flPostSubmitGpuMs;
        ret.compositor_gpu = t.m_flCompositorRenderGpuMs;
        ret.compositor_cpu = t.m_flCompositorRenderCpuMs;
        ret.compositor_submit_frame = t.m_flSubmitFrameMs;
        ret.gpu_total = t.m_flTotalRenderGpuMs;
        ret.frame_interval = t.m_flClientFrameIntervalMs;
        ret.cpu_present_call = t.m_flPresentCallCpuMs;
        ret.cpu_wait_for_present = t.m_flWaitForPresentCpuMs;
        ret.reprojection_flags = t.m_nReprojectionFlags;
        ret.mispresented_frames = t.m_nNumMisPresented;
        ret.dropped_frames = t.m_nNumDroppedFrames;
    }
    return ret;
}
