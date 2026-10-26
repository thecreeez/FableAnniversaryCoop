#pragma once

#include <Windows.h>

#include <cstdint>

namespace fable::game::native
{
    // The retail tutorial message box (HUD message manager, style 0x2000A):
    // a centred panel that stays until the player clicks it away. Shown with
    // mod text, retitled in place and closed by the mod. Game thread only.
    class NativePromptBox final
    {
    public:
        enum class State : std::uint8_t
        {
            Hidden,
            Showing,
            // The player clicked the box away.
            Dismissed,
        };

        void Bind(HMODULE gameModule) noexcept { base_ = reinterpret_cast<std::uintptr_t>(gameModule); }

        // Shows text, or retitles the box already showing. The text up to the
        // first '(' identifies our box among retail messages.
        bool Show(const wchar_t* text) noexcept;
        void Close() noexcept;
        // Moves Showing to Dismissed once the box went away without Close.
        State Poll() noexcept;

    private:
        void* Manager() const noexcept;
        bool IsOurs(void* manager) const noexcept;
        bool Queue(void* manager, const wchar_t* text) noexcept;

        std::uintptr_t base_ = 0;
        State state_ = State::Hidden;
        wchar_t prefix_[48] = {};
        wchar_t shown_[256] = {};
        // Nonzero while our box waits behind another retail message.
        std::uint64_t queuedAt_ = 0;
    };
}
