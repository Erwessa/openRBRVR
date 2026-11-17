#pragma once

#include "Config.hpp"
#include "VR.hpp"

class OpenVR : public VRInterface {
private:
    D3D9_OPENVR_FRAME_STATE m_frameState = {};
    bool m_framesPrepared = false;
    bool m_backgroundSessionInitialized = false;

    M4 get_projection_matrix(RenderTarget eye, float z_near, float z_far, bool reverse_z) const;

public:
    OpenVR();
    ~OpenVR() override {
        shutdown_vr();
    }

    void init(IDirect3DDevice9* dev, IDirect3DVR9** vrdev, uint32_t companionWindowWidth, uint32_t companionWindowHeight);

    void shutdown_device_resources();
    void shutdown_vr() override;
    void discard_prepared_frames() override;
    void prepare_frames_for_hmd(IDirect3DDevice9* dev) override;
    bool update_vr_poses() override;
    void submit_frames_to_hmd(IDirect3DDevice9* dev) override;
    FrameTimingInfo get_frame_timing() override;
    void reset_view() override;

    VRRuntime get_runtime_type() const override {
        return OPENVR;
    }
    void set_render_context(const std::string& name) override;
};
