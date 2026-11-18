#pragma once

#include <MinHook.h>
#include <stdexcept>

// RAII wrapper for MinHook
template <typename T>
struct Hook {
    T call;
    T src;
    T detour;

    explicit Hook()
        : call(nullptr)
        , src(nullptr)
        , detour(nullptr)
    {
    }

    void install(T target, T hook)
    {
        if (src) {
            if (src != target || detour != hook)
                throw std::runtime_error("Hook is already installed for a different target");

            const auto status = MH_EnableHook(reinterpret_cast<void*>(src));
            if (status != MH_OK && status != MH_ERROR_ENABLED)
                throw std::runtime_error("Could not re-enable existing hook");
            return;
        }

        call = nullptr;
        src = target;
        detour = hook;
        const auto createStatus = MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(hook), reinterpret_cast<void**>(&call));
        if (createStatus != MH_OK) {
            call = nullptr;
            src = nullptr;
            detour = nullptr;
            throw std::runtime_error("Could not create hook");
        }

        const auto enableStatus = MH_EnableHook(reinterpret_cast<void*>(target));
        if (enableStatus != MH_OK) {
            // Try to establish a disabled state before attempting removal.
            // If either operation fails, retain ownership for a later retry.
            const auto disableStatus = MH_DisableHook(reinterpret_cast<void*>(target));
            if (disableStatus == MH_OK || disableStatus == MH_ERROR_DISABLED) {
                if (MH_RemoveHook(reinterpret_cast<void*>(target)) == MH_OK) {
                    call = nullptr;
                    src = nullptr;
                    detour = nullptr;
                }
            }
            throw std::runtime_error("Could not enable hook");
        }
    }

    MH_STATUS enable() noexcept
    {
        if (!src)
            return MH_OK;

        const auto status = MH_EnableHook(reinterpret_cast<void*>(src));
        return status == MH_ERROR_ENABLED ? MH_OK : status;
    }

    MH_STATUS disable() noexcept
    {
        if (!src)
            return MH_OK;

        const auto status = MH_DisableHook(reinterpret_cast<void*>(src));
        return status == MH_ERROR_DISABLED ? MH_OK : status;
    }

    MH_STATUS reset() noexcept
    {
        if (!src) {
            call = nullptr;
            detour = nullptr;
            return MH_OK;
        }

        const auto status = MH_RemoveHook(reinterpret_cast<void*>(src));
        if (status != MH_OK)
            return status;

        call = nullptr;
        src = nullptr;
        detour = nullptr;
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

template <typename... Hooks>
bool disable_hooks(Hooks&... hooks) noexcept
{
    bool success = true;
    const auto disable_one = [&success](auto& hook) {
        if (hook.disable() != MH_OK)
            success = false;
    };
    (disable_one(hooks), ...);
    return success;
}

template <typename T, typename F>
T* get_vtable(F* obj)
{
    return (T*)(*(uintptr_t*)obj);
}
