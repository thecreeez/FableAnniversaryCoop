#include "HudMessageProbe.h"

#include "Game/Entity/Native/ThingComponentAccess.h"

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace
{
    // HUD message manager queue (this = HUD + 0x18), ret 0x24:
    // (CWideString* text, CWideString* x4, style, float, int, int).
    constexpr std::uintptr_t QueueRva = 0x0191AF8B;
    // push 28h; mov eax, <SEH handler 0x2925CE0>
    constexpr std::uintptr_t QueueHandlerRva = 0x02525CE0;
    constexpr std::size_t QueuePrologueBytes = 7;
    // (this = HUD + 0x18, CWideString* text, style, float, int, int), ret 0x14.

    // CWideString holds a pointer to a refcounted std::wstring.
    void ReadWide(void* const string, char* const destination, const std::size_t capacity) noexcept
    {
        destination[0] = '\0';
        __try
        {
            const auto* rep = string == nullptr ? nullptr : *static_cast<const std::uint8_t* const*>(string);
            if (rep == nullptr) return;
            const auto size = *reinterpret_cast<const std::uint32_t*>(rep + 0x10);
            const auto reserved = *reinterpret_cast<const std::uint32_t*>(rep + 0x14);
            const wchar_t* data = reserved >= 8
                ? *reinterpret_cast<const wchar_t* const*>(rep)
                : reinterpret_cast<const wchar_t*>(rep);
            if (data == nullptr || size == 0 || size > 512) return;
            const int written = WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(size),
                destination, static_cast<int>(capacity) - 1, nullptr, nullptr);
            destination[written > 0 ? written : 0] = '\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            destination[0] = '\0';
        }
    }
}

namespace fable::game::native
{
    HudMessageProbe* HudMessageProbe::active_ = nullptr;

    bool HudMessageProbe::Install(const HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept
    {
#if !defined(_M_IX86)
        (void)gameModule;
        (void)diagnostics;
        return false;
#else
        char flag[8] = {};
        const DWORD length = GetEnvironmentVariableA("ALBIONTOGETHER_TRACE_SCRIPT_INTERFACE", flag, sizeof(flag));
        if (length == 0 || length >= sizeof(flag) || flag[0] != '1' || gameModule == nullptr ||
            queue_.IsInstalled() || active_ != nullptr)
        {
            return false;
        }
        diagnostics_ = diagnostics;
        base_ = reinterpret_cast<std::uintptr_t>(gameModule);
        std::uint8_t prologue[QueuePrologueBytes] = {0x6A, 0x28, 0xB8};
        const std::uint32_t handler = static_cast<std::uint32_t>(base_ + QueueHandlerRva);
        std::memcpy(prologue + 3, &handler, sizeof(handler));
        active_ = this;
        if (!queue_.Install(reinterpret_cast<void*>(base_ + QueueRva), prologue, sizeof(prologue),
                reinterpret_cast<void*>(&Queued), sizeof(prologue)))
        {
            diagnostics_.Event("HudMessageProbeBlocked", "message queue prologue did not match");
            active_ = nullptr;
            return false;
        }
        original_ = reinterpret_cast<QueueFn>(queue_.Original());
        prompt_.Bind(gameModule);
        diagnostics_.Event("HudMessageProbeReady", "HUD message queue is logged");
        return true;
#endif
    }

    void HudMessageProbe::Process() noexcept
    {
        if (original_ == nullptr) return;
        DWORD process = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &process);
        const bool focused = process == GetCurrentProcessId();
        constexpr int Keys[3] = {VK_INSERT, VK_HOME, VK_END};
        for (int i = 0; i < 3; ++i)
        {
            const bool down = focused && (GetAsyncKeyState(Keys[i]) & 0x8000) != 0;
            if (down && !keyDown_[i]) QueueTest(i);
            keyDown_[i] = down;
        }
        ReportInputStack();
        const NativePromptBox::State state = prompt_.Poll();
        if (state != reportedState_)
        {
            reportedState_ = state;
            diagnostics_.Event("HudPromptState", state == NativePromptBox::State::Showing ? "showing"
                : state == NativePromptBox::State::Dismissed ? "dismissed" : "hidden");
        }
    }

    void HudMessageProbe::ReportInputStack() noexcept
    {
        // Player object control fields: +0x2D4 control taken, +0x2D5 its
        // last-frame copy, +0x2D8 mode (1 = player), +0x2E0 mode lock.
        const std::uint64_t now = GetTickCount64();
        if (now < nextInputReportAt_) return;
        nextInputReportAt_ = now + 250;
        char stack[400] = {};
        __try
        {
            auto* const player = *reinterpret_cast<std::uint8_t* const*>(base_ + 0x0322B3CC);
            if (player == nullptr) return;
            // Retail HUD visibility (0x1CE9510) requires: +0x3A0 set, +0xFE clear,
            // [[+4]+0x38]->vfunc(0x674) false, and the 0x1CFF470 state != 0x10.
            auto* const owner = *reinterpret_cast<std::uint8_t* const*>(player + 4);
            auto* const gate = owner == nullptr ? nullptr : *reinterpret_cast<std::uint8_t* const*>(owner + 0x38);
            const std::uintptr_t gateVtable = gate == nullptr ? 0 : *reinterpret_cast<const std::uintptr_t*>(gate);
            using QueryFn = bool(__thiscall*)(void*);
            const bool gateBusy = gate != nullptr &&
                reinterpret_cast<QueryFn>((*reinterpret_cast<void* const* const*>(gate))[0x674 / 4])(gate);
            using GetFn = void*(__thiscall*)(void*);
            using StateFn = std::int32_t(__thiscall*)(void*);
            void* const controllers = reinterpret_cast<GetFn>(base_ + 0x018FD7D0)(*reinterpret_cast<void* const*>(player + 8));
            const std::int32_t controllerState = controllers == nullptr ? -1
                : reinterpret_cast<StateFn>(base_ + 0x018FF470)(controllers);
            int used = std::snprintf(stack, sizeof(stack),
                "visible=%u hud3a0=%u cut=%u gate_vt=%08X gate_busy=%d ctl_state=%d f105=%u mode=%d",
                player[*reinterpret_cast<const std::int32_t*>(player + 0x3D8) + 0x38D], player[0x3A0], player[0xFE],
                static_cast<unsigned>(gateVtable == 0 ? 0 : gateVtable - base_ + 0x400000), gateBusy ? 1 : 0,
                controllerState, player[0x105], *reinterpret_cast<const std::int32_t*>(player + 0x2D8));
            // The local Hero Thing, as retail HUD code resolves it.
            using GetFn = void*(__thiscall*)(void*);
            auto* const world = *reinterpret_cast<std::uint8_t* const*>(base_ + 0x0322AE48);
            void* const manager = world == nullptr ? nullptr
                : reinterpret_cast<GetFn>(base_ + 0x018FD7D0)(*reinterpret_cast<void* const*>(world + 0x24));
            void* const hero = manager == nullptr ? nullptr : reinterpret_cast<GetFn>(base_ + 0x018E3AD0)(manager);
            entity::native::ThingComponentRange range;
            if (hero != nullptr && entity::native::ThingComponentAccess::ReadRange(hero, range))
            {
                auto* const scripted = static_cast<std::uint8_t*>(entity::native::ThingComponentAccess::Find(
                    hero, entity::native::ThingComponentType::ScriptedControl));
                if (scripted != nullptr && used < 380)
                {
                    std::snprintf(stack + used, sizeof(stack) - used, " sc=%u/%d", scripted[0x20],
                        *reinterpret_cast<const std::int32_t*>(scripted + 0x24));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            std::snprintf(stack, sizeof(stack), "fault");
        }
        if (std::strcmp(stack, lastInputStack_) == 0) return;
        std::memcpy(lastInputStack_, stack, sizeof(stack));
        diagnostics_.Event("PlayerControlState", stack);
    }

    void HudMessageProbe::QueueTest(const int key) noexcept
    {
        bool ok = true;
        if (key == 2)
        {
            prompt_.Close();
        }
        else
        {
            if (key == 0) count_ = 1;
            else count_ = count_ % 3 + 1;
            wchar_t text[160] = {};
            std::swprintf(text, sizeof(text) / sizeof(text[0]),
                L"\u041E\u0436\u0438\u0434\u0430\u043D\u0438\u0435 \u0438\u0433\u0440\u043E\u043A\u043E\u0432 (%u/3), \u0435\u0441\u043B\u0438 \u0445\u043E\u0442\u0438\u0442\u0435 \u043F\u0440\u0435\u043A\u0440\u0430\u0442\u0438\u0442\u044C \u043E\u0436\u0438\u0434\u0430\u043D\u0438\u0435 \u043D\u0430\u0436\u043C\u0438\u0442\u0435 \u041B\u041A\u041C", count_);
            ok = prompt_.Show(text);
        }
        char detail[64] = {};
        std::snprintf(detail, sizeof(detail), "key=%d count=%u ok=%d", key, count_, ok ? 1 : 0);
        diagnostics_.Event("HudPromptTest", detail);
    }

    void HudMessageProbe::Shutdown() noexcept
    {
        if (!queue_.Shutdown()) return;
        original_ = nullptr;
        if (active_ == this) active_ = nullptr;
    }

    std::uintptr_t __fastcall HudMessageProbe::Queued(void* const self, void* const edx, void* const text,
        void* const a2, void* const a3, void* const a4, void* const a5, const std::uint32_t style,
        const float duration, const std::uint32_t a8, const std::uint32_t a9)
    {
        (void)edx;
        HudMessageProbe* const probe = active_;
        if (probe == nullptr || probe->original_ == nullptr) return 0;
        char body[384] = {};
        char extra[4][96] = {};
        ReadWide(text, body, sizeof(body));
        ReadWide(a2, extra[0], sizeof(extra[0]));
        ReadWide(a3, extra[1], sizeof(extra[1]));
        ReadWide(a4, extra[2], sizeof(extra[2]));
        ReadWide(a5, extra[3], sizeof(extra[3]));
        void* frames[6] = {};
        const USHORT captured = CaptureStackBackTrace(1, 6, frames, nullptr);
        char callers[96] = {};
        std::size_t used = 0;
        for (USHORT i = 0; i < captured && used + 12 < sizeof(callers); ++i)
        {
            const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
            used += static_cast<std::size_t>(std::snprintf(callers + used, sizeof(callers) - used, "%08X ",
                static_cast<unsigned>(address - probe->base_ + 0x400000)));
        }
        char detail[900] = {};
        std::snprintf(detail, sizeof(detail),
            "style=0x%X duration=%.2f a8=%u a9=%u text=\"%s\" x=[%s|%s|%s|%s] callers=%s",
            style, duration, a8, a9, body, extra[0], extra[1], extra[2], extra[3], callers);
        probe->diagnostics_.Event("HudMessageQueued", detail);
        return probe->original_(self, text, a2, a3, a4, a5, style, duration, a8, a9);
    }
}
