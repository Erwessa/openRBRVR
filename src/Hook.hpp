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

    void install(T target, T detour)
    {
        if (src)
            throw std::runtime_error("Hook is already installed");

        call = nullptr;
        src = target;
        const auto createStatus = MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&call));
        if (createStatus != MH_OK) {
            src = nullptr;
            throw std::runtime_error("Could not create hook");
        }

        const auto enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
        if (enableStatus != MH_OK) {
            // Keep src/call if removal fails: this object must retain ownership
            // of the MinHook entry and trampoline for a later cleanup attempt.
            if (MH_RemoveHook(reinterpret_cast<void*>(target)) == MH_OK) {
                call = nullptr;
                src = nullptr;
            }
            throw std::runtime_error("Could not enable hook");
        }
    }

    void enable()
    {
        if (src && MH_EnableHook(reinterpret_cast<void*>(src)) != MH_OK) {
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
    Hook(Hook&&) = delete;
    Hook& operator=(const Hook&) = delete;
    Hook& operator=(Hook&&) = delete;

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
