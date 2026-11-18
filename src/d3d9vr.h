#pragma once

#include <d3d9.h>
#include <stdint.h>

// Shared x86/x64 D3D9VR ABI. Vulkan handles and native descriptors are
// intentionally opaque to the client-side OpenRBRVR plugin.
struct D3D9_TEXTURE_VR_DESC;
struct OXR_VK_DEVICE_DESC;

struct D3D9_OPENVR_FRAME_STATE {
  uint32_t Version;
  uint32_t Flags;
  uint64_t FrameIndex;
  uint32_t RecommendedWidth;
  uint32_t RecommendedHeight;
  float DeviceToAbsoluteTracking[12];
  float EyeToHead[2][12];
  float ProjectionRaw[2][4];
  float ProjectionMatrix[2][16];
};
constexpr uint32_t D3D9_OPENVR_FRAME_STATE_VERSION = 2;

struct D3D9_GAME_CAMERA {
  uint32_t Version;
  uint32_t Flags;
  D3DMATRIX WorldToView;
  D3DMATRIX ViewToProjection;
};
constexpr uint32_t D3D9_GAME_CAMERA_VERSION = 1;
constexpr uint32_t D3D9_GAME_CAMERA_VALID = 1u;
static_assert(sizeof(D3D9_GAME_CAMERA) == 136,
  "D3D9_GAME_CAMERA must have an identical x86/x64 wire layout");

enum class D3D9VRCommand : uint32_t {
  InitializeOpenVRScene = 1,
  AcquireOpenVRFrame,
  SubmitOpenVRFrame,
  ResetOpenVRView,
  GetOpenVRFrameTiming,
  WaitDeviceIdle,
  CopySurfaceLayers,
  GetSPIRVShaderCode,
  PatchSPIRVToVertexShader,
  GetShaderConstantCount,
  SetShaderConstantCount,
  GetShaderHash,
  EnableMultiView,
  CreateMultiViewRenderTarget,
  CreateMultiViewDepthStencilSurface,
  Flush,
  ImportFence,
  SignalFence,
  SetGameCamera,
};

static_assert(sizeof(D3D9_OPENVR_FRAME_STATE) == 328,
  "D3D9_OPENVR_FRAME_STATE must have an identical x86/x64 wire layout");

struct D3D9_OPENVR_FRAME_TIMING {
  float GpuPreSubmit;
  float GpuPostSubmit;
  float CompositorGpu;
  float CompositorCpu;
  float CompositorSubmitFrame;
  float GpuTotal;
  float FrameInterval;
  float WaitForPresent;
  float CpuPresentCall;
  float CpuWaitForPresent;
  uint32_t ReprojectionFlags;
  uint32_t MispresentedFrames;
  uint32_t DroppedFrames;
};
static_assert(sizeof(D3D9_OPENVR_FRAME_TIMING) == 52,
  "D3D9_OPENVR_FRAME_TIMING must have an identical x86/x64 wire layout");

MIDL_INTERFACE("7e272b32-a49c-46c7-b1a4-ef52936bec87")
IDirect3DVR9 : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetVRDesc(IDirect3DSurface9* pSurface, D3D9_TEXTURE_VR_DESC* pDesc) = 0;
  virtual HRESULT STDMETHODCALLTYPE TransferSurfaceForVR(IDirect3DSurface9* pSurface) = 0;
  virtual HRESULT STDMETHODCALLTYPE BeginVRSubmit() = 0;
  virtual HRESULT STDMETHODCALLTYPE EndVRSubmit() = 0;
  virtual HRESULT STDMETHODCALLTYPE LockDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE WaitDeviceIdle(BOOL flush) = 0;
  virtual HRESULT STDMETHODCALLTYPE WaitGraphicsQueueIdle(BOOL flush) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetOXRVkDeviceDesc(OXR_VK_DEVICE_DESC* vkDeviceDescOut) = 0;
  virtual HRESULT STDMETHODCALLTYPE Flush() = 0;
  virtual HRESULT STDMETHODCALLTYPE LockSubmissionQueue() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() = 0;
  virtual HRESULT STDMETHODCALLTYPE ImportFence(HANDLE handle, uint64_t value) = 0;
  virtual HRESULT STDMETHODCALLTYPE SignalFence(uint64_t value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShaderHash(IDirect3DVertexShader9* d3dShader, char** out) = 0;
  virtual HRESULT STDMETHODCALLTYPE PatchSPIRVToVertexShader(IDirect3DVertexShader9* d3dShader, const uint32_t* data, uint32_t size) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateMultiViewRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, UINT Views) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateMultiViewDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, UINT Views) = 0;
  virtual HRESULT STDMETHODCALLTYPE CopySurfaceLayers(IDirect3DSurface9* srcSurface, IDirect3DSurface9** dsts, UINT layerCount) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetSPIRVShaderCode(IDirect3DVertexShader9* d3dShader, uint32_t* out, uint32_t* size) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShaderConstantCount(IDirect3DVertexShader9* d3dShader, uint32_t* out) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetShaderConstantCount(IDirect3DVertexShader9* d3dShader, uint32_t constantCount) = 0;
  virtual HRESULT STDMETHODCALLTYPE EnableMultiView(bool enable) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetMultiviewSurfaceLayer(IDirect3DSurface9* pSurface, uint32_t layer) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateTextureFromNTHandle(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) = 0;
  virtual HRESULT STDMETHODCALLTYPE InitializeOpenVRScene() = 0;
  virtual HRESULT STDMETHODCALLTYPE AcquireOpenVRFrame(D3D9_OPENVR_FRAME_STATE* frame) = 0;
  virtual HRESULT STDMETHODCALLTYPE SubmitOpenVRFrame(IDirect3DSurface9* left, IDirect3DSurface9* right) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetOpenVRView() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetOpenVRFrameTiming(D3D9_OPENVR_FRAME_TIMING* timing) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetGameCamera(const D3D9_GAME_CAMERA* camera) = 0;
};
