#include "NativePromptBox.h"

#include <cstring>
#include <cwchar>

namespace
{
    constexpr std::uintptr_t HudSingletonRva = 0x0322B3CC;
    constexpr std::uintptr_t ManagerOffset = 0x18;
    // (this, CWideString* text, style, float duration, int, int), ret 0x14.
    constexpr std::uintptr_t ShowRva = 0x0191BBBD;
    // (this, bool immediate): the retail click-away path passes false.
    constexpr std::uintptr_t CloseRva = 0x0191B5FA;
    constexpr std::uintptr_t OperatorNewRva = 0x00001050;
    constexpr std::uintptr_t WideAssignFillRva = 0x0134EE40;
    constexpr std::uintptr_t WideStringDestructorRva = 0x0134EFD0;
    constexpr std::uint32_t TutorialStyle = 0x2000A;
    // Manager fields.
    constexpr std::uintptr_t TextOffset = 0x4C;
    constexpr std::uintptr_t StyleOffset = 0x6C;
    constexpr std::uintptr_t ShowingOffset = 0xED;
    // A box queued behind another message is given up after this.
    constexpr std::uint64_t QueuedTimeoutMilliseconds = 30'000;

    using NewFn = void*(__cdecl*)(std::size_t);
    using AssignFn = void(__thiscall*)(void* rep, std::uint32_t count, wchar_t fill);
    using DestroyFn = void(__thiscall*)(void* string);

    // CWideString is a pointer to a refcounted std::wstring (+0x1C count).
    const wchar_t* WideData(const void* const string, std::uint32_t& size) noexcept
    {
        size = 0;
        const auto* rep = *static_cast<const std::uint8_t* const*>(string);
        if (rep == nullptr) return nullptr;
        size = *reinterpret_cast<const std::uint32_t*>(rep + 0x10);
        return *reinterpret_cast<const std::uint32_t*>(rep + 0x14) >= 8
            ? *reinterpret_cast<const wchar_t* const*>(rep)
            : reinterpret_cast<const wchar_t*>(rep);
    }

    // Builds a game-owned CWideString; the caller destroys it through the game.
    bool MakeWide(const std::uintptr_t base, const wchar_t* const text, void*& string) noexcept
    {
        string = nullptr;
        const auto length = static_cast<std::uint32_t>(std::wcslen(text));
        auto* const rep = static_cast<std::uint8_t*>(reinterpret_cast<NewFn>(base + OperatorNewRva)(0x20));
        if (rep == nullptr) return false;
        *reinterpret_cast<std::uint32_t*>(rep + 0x10) = 0;
        *reinterpret_cast<std::uint32_t*>(rep + 0x14) = 7;
        *reinterpret_cast<wchar_t*>(rep) = L'\0';
        *reinterpret_cast<std::int32_t*>(rep + 0x1C) = 1;
        string = rep;
        reinterpret_cast<AssignFn>(base + WideAssignFillRva)(rep, length, L' ');
        std::uint32_t size = 0;
        auto* const data = const_cast<wchar_t*>(WideData(&string, size));
        if (data == nullptr || size != length) return false;
        std::memcpy(data, text, length * sizeof(wchar_t));
        return true;
    }
}

namespace fable::game::native
{
    void* NativePromptBox::Manager() const noexcept
    {
        if (base_ == 0) return nullptr;
        auto* const hud = *reinterpret_cast<std::uint8_t* const*>(base_ + HudSingletonRva);
        return hud == nullptr ? nullptr : hud + ManagerOffset;
    }

    bool NativePromptBox::IsOurs(void* const manager) const noexcept
    {
        const auto* const m = static_cast<const std::uint8_t*>(manager);
        if (m[ShowingOffset] == 0 || *reinterpret_cast<const std::uint32_t*>(m + StyleOffset) != TutorialStyle)
        {
            return false;
        }
        std::uint32_t size = 0;
        const wchar_t* const data = WideData(m + TextOffset, size);
        const std::size_t prefix = std::wcslen(prefix_);
        if (data == nullptr || prefix == 0 || size < prefix) return false;
        return std::wmemcmp(data, prefix_, prefix) == 0;
    }

    bool NativePromptBox::Queue(void* const manager, const wchar_t* const text) noexcept
    {
        void* string = nullptr;
        bool shown = false;
        __try
        {
            if (MakeWide(base_, text, string))
            {
                using ShowFn = void(__thiscall*)(void*, void*, std::uint32_t, float, std::uint32_t, std::uint32_t);
                reinterpret_cast<ShowFn>(base_ + ShowRva)(manager, &string, TutorialStyle, 0.0f, 1, 1);
                shown = true;
            }
            if (string != nullptr) reinterpret_cast<DestroyFn>(base_ + WideStringDestructorRva)(&string);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            shown = false;
        }
        return shown;
    }

    bool NativePromptBox::Show(const wchar_t* const text) noexcept
    {
        void* const manager = Manager();
        if (manager == nullptr || text == nullptr || text[0] == L'\0') return false;
        const wchar_t* const open = std::wcschr(text, L'(');
        const std::size_t prefix = open == nullptr ? std::wcslen(text) : static_cast<std::size_t>(open - text);
        const std::size_t keep = prefix < 47 ? prefix : 47;
        std::wmemcpy(prefix_, text, keep);
        prefix_[keep] = L'\0';
        if (state_ == State::Showing)
        {
            bool ours = false;
            __try
            {
                ours = IsOurs(manager);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            if (ours)
            {
                // The panel is drawn from the text it opened with, so a new
                // title means closing it and opening it again at once.
                if (std::wcscmp(text, shown_) == 0) return true;
                using CloseFn = void(__thiscall*)(void*, std::uint8_t);
                __try
                {
                    reinterpret_cast<CloseFn>(base_ + CloseRva)(manager, 0);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return false;
                }
            }
            else if (queuedAt_ != 0)
            {
                return true;
            }
        }
        if (!Queue(manager, text)) return false;
        const std::size_t length = std::wcslen(text);
        const std::size_t copied = length < 255 ? length : 255;
        std::wmemcpy(shown_, text, copied);
        shown_[copied] = L'\0';
        state_ = State::Showing;
        queuedAt_ = GetTickCount64();
        return true;
    }

    void NativePromptBox::Close() noexcept
    {
        void* const manager = Manager();
        if (state_ == State::Showing && manager != nullptr)
        {
            __try
            {
                if (IsOurs(manager))
                {
                    using CloseFn = void(__thiscall*)(void*, std::uint8_t);
                    reinterpret_cast<CloseFn>(base_ + CloseRva)(manager, 0);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        state_ = State::Hidden;
        queuedAt_ = 0;
    }

    bool NativePromptBox::DismissAsPlayerForTest() noexcept
    {
        void* const manager = Manager();
        if (state_ != State::Showing || manager == nullptr) return false;
        __try
        {
            if (!IsOurs(manager)) return false;
            using CloseFn = void(__thiscall*)(void*, std::uint8_t);
            reinterpret_cast<CloseFn>(base_ + CloseRva)(manager, 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    NativePromptBox::State NativePromptBox::Poll() noexcept
    {
        if (state_ != State::Showing) return state_;
        void* const manager = Manager();
        bool ours = false;
        __try
        {
            ours = manager != nullptr && IsOurs(manager);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ours = false;
        }
        if (ours)
        {
            queuedAt_ = 0;
            return state_;
        }
        // Still queued behind another message: wait, but not forever.
        if (queuedAt_ != 0 && GetTickCount64() - queuedAt_ < QueuedTimeoutMilliseconds) return state_;
        state_ = State::Dismissed;
        return state_;
    }
}
