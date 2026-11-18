#pragma once

#include <MinHook.h>
#include <stdexcept>

// RAII wrapper for MinHook
template <typename T>
struct Hook {
    T call;
    T src;

    explicit Hook()
        : call(nullptr)
        , src(nullptr)
    {
    }

    explicit Hook(T src, T tgt)
        : src(src)
    {
        if (MH_CreateHook(reinterpret_cast<void*>(src), reinterpret_cast<void*>(tgt), reinterpret_cast<void**>(&call)) != MH_OK) {
            throw std::runtime_error("Could not hook");
        }
        try {
            enable();
        } catch (...) {
            reset();
            throw;
        }
    }
    void enable()
    {
        if (MH_EnableHook(reinterpret_cast<void*>(src)) != MH_OK) {
            throw std::runtime_error("Could not enable hook");
        }
    }
    void disable()
    {
        if (src && MH_DisableHook(reinterpret_cast<void*>(src)) != MH_OK) {
            throw std::runtime_error("Could not disable hook");
        }
    }
    MH_STATUS reset() noexcept
    {
        if (!src) {
            call = nullptr;
            return MH_OK;
        }

        const auto status = MH_RemoveHook(reinterpret_cast<void*>(src));
        if (status != MH_OK)
            return status;

        call = nullptr;
        src = nullptr;
        return MH_OK;
    }
    Hook(const Hook&) = delete;
    Hook(Hook&& rhs)
    {
        call = rhs.call;
        src = rhs.src;
        rhs.call = nullptr;
        rhs.src = nullptr;
    }
    Hook& operator=(const Hook&) = delete;
    Hook& operator=(Hook&& rhs)
    {
        if (this == &rhs)
            return *this;

        // Report removal failure to the installation transaction instead of
        // silently discarding the incoming hook.
        if (reset() != MH_OK)
            throw std::runtime_error("Could not remove existing hook");

        call = rhs.call;
        src = rhs.src;
        rhs.call = nullptr;
        rhs.src = nullptr;
        return *this;
    }
    ~Hook()
    {
        reset();
    }
};

template <typename T, typename F>
T* get_vtable(F* obj)
{
    return (T*)(*(uintptr_t*)obj);
}
