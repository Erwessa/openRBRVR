#include "Vertex.hpp"

bool create_vertex_buffer(IDirect3DDevice9* dev, Vertex* v, size_t vertexCount, IDirect3DVertexBuffer9** buf)
{
    const auto vsz = vertexCount * sizeof(Vertex);
    *buf = nullptr;
    if (auto ret = dev->CreateVertexBuffer(vsz, 0, D3DFVF_XYZ | D3DFVF_TEX1, D3DPOOL_MANAGED, buf, nullptr); FAILED(ret)) {
        dbg("D3D initialization failed: CreateVertexBuffer");
        return false;
    }

    void* bufmem = nullptr;
    if (FAILED((*buf)->Lock(0, vsz, &bufmem, 0))) {
        dbg("D3D initialization failed: Lock vertex buffer");
        (*buf)->Release();
        *buf = nullptr;
        return false;
    }

    memcpy(bufmem, v, vsz);
    if (FAILED((*buf)->Unlock())) {
        dbg("D3D initialization failed: Unlock vertex buffer");
        (*buf)->Release();
        *buf = nullptr;
        return false;
    }

    return true;
}
