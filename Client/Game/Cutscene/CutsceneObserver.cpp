#include "CutsceneObserver.h"

#include "Game/Native/Addresses.h"
#include "Game/Native/ScriptTypes.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <new>

namespace
{
    // Scripted cutscene start, cdecl (CCharString* name, ...): 353 callers.
    constexpr std::uintptr_t CutsceneStartRva = 0x0135F0AA;
    // push ebp; lea ebp, [esp-60h]; sub esp, 60h
    constexpr std::array<std::uint8_t, 8> CutsceneStartPrefix = {
        0x55, 0x8D, 0x6C, 0x24, 0xA0, 0x83, 0xEC, 0x60};
    // UHUD_Subtitle::IsInCutscene implementation: reads the per-player byte
    // through the HUD singleton; returns 0/1 in eax.
    constexpr std::uintptr_t IsInCutsceneRva = 0x01BE9BB0;
    constexpr std::uintptr_t HudSingletonRva = 0x0322B3CC;
    // Retail script sequence around a cutscene (Q_WaspBoss, 0x0152CAA1):
    // interface slot 388 fills a CScriptGameResourceObjectMovieBase from a
    // resource name ("" = default), slot 397 toggles cutscene mode.
    constexpr std::size_t CreateMovieResourceSlot = 388;
    constexpr std::size_t SetCutsceneModeSlot = 397;
    // Script "wait for the next frame" (CS_PlayCutscene loop, +0x2C).
    constexpr std::size_t NewScriptFrameSlot = 11;
    constexpr std::uintptr_t MovieResourceBaseConstructorRva = 0x012B5060;
    constexpr std::uintptr_t MovieResourceVtableRva = 0x02A5CD38;
    constexpr std::size_t MovieResourceBytes = 0x20;
    constexpr std::uintptr_t EndScriptedSequenceRva = 0x0189E940;
    // The sequence guard's destructor (0x018899C0) then drops the player
    // mode the sequence pushed: RemoveMode(0x1B) on the player controller
    // resolved from interface +0x14 / +0x1C.
    constexpr std::uintptr_t ResolvePlayerControllerRva = 0x018FD6F0;
    constexpr std::uintptr_t RemovePlayerModeRva = 0x018FF7B0;
    constexpr std::int32_t ScriptedSequencePlayerMode = 0x1B;

    void ReadName(void* const name, char* const destination, const std::size_t capacity) noexcept
    {
        destination[0] = '\0';
        __try
        {
            const auto* string = static_cast<const fable::game::native::CharString*>(name);
            const auto* rep = string == nullptr ? nullptr
                : static_cast<const fable::game::native::StringRep*>(string->stringData);
            if (rep == nullptr || rep->data == nullptr || rep->len <= 0 ||
                static_cast<std::size_t>(rep->len) >= capacity)
            {
                return;
            }
            std::memcpy(destination, rep->data, static_cast<std::size_t>(rep->len));
            destination[rep->len] = '\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            destination[0] = '\0';
        }
    }
}

namespace fable::game::cutscene
{
    CutsceneObserver* CutsceneObserver::active_ = nullptr;
    thread_local bool CutsceneObserver::replaying_ = false;

    void CutsceneObserver::SetActiveName(const char* name) noexcept
    {
        AcquireSRWLockExclusive(&skipLock_);
        strncpy_s(activeName_, name, _TRUNCATE);
        skipRequested_ = false;
        broadcastSkip_ = false;
        ReleaseSRWLockExclusive(&skipLock_);
    }

    bool CutsceneObserver::PopSkip(std::string& name)
    {
        char copy[96] = {};
        AcquireSRWLockExclusive(&skipLock_);
        std::memcpy(copy, localSkip_, sizeof(copy));
        localSkip_[0] = 0;
        ReleaseSRWLockExclusive(&skipLock_);
        if (copy[0] == 0) return false;
        name = copy;
        return true;
    }

    bool CutsceneObserver::RequestSkip(const char* name, bool broadcast) noexcept
    {
        AcquireSRWLockExclusive(&skipLock_);
        const bool matches = name != nullptr && activeName_[0] != 0 && std::strcmp(name, activeName_) == 0;
        if (matches) { skipRequested_ = true; broadcastSkip_ = broadcast; }
        ReleaseSRWLockExclusive(&skipLock_);
        return matches;
    }

    void __fastcall CutsceneObserver::InputObserved(void* self, void*, void* input, void* messages)
    {
        auto* observer = active_;
        if (observer == nullptr || observer->originalInput_ == nullptr) return;
        observer->originalInput_(self, input, messages);
        if (messages == nullptr) return;
        bool requested = false, broadcast = false;
        char name[96] = {};
        AcquireSRWLockExclusive(&observer->skipLock_);
        requested = observer->skipRequested_;
        broadcast = observer->broadcastSkip_;
        observer->skipRequested_ = false;
        observer->broadcastSkip_ = false;
        std::memcpy(name, observer->activeName_, sizeof(name));
        ReleaseSRWLockExclusive(&observer->skipLock_);
        if (name[0] == 0) return;
        const auto base = reinterpret_cast<std::uintptr_t>(observer->gameModule_);
        bool skipped = false;
        __try
        {
            auto* queue = static_cast<unsigned char*>(messages);
            for (unsigned i = 0; i < queue[0xA8] && i < 4; ++i)
                if (*reinterpret_cast<unsigned*>(queue + 8 + i * 0x28) == 0x39) skipped = true;
            if (requested && !skipped)
            {
                unsigned char event[0x28] = {};
                *reinterpret_cast<unsigned*>(event) = 0x39;
                event[4] = 0xFF;
                event[0x26] = 1;
                reinterpret_cast<void(__thiscall*)(void*)>(base + 0x012E69C0)(messages);
                reinterpret_cast<void(__thiscall*)(void*, void*)>(base + 0x012E6950)(messages, event);
                skipped = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (skipped && (!requested || broadcast))
        {
            AcquireSRWLockExclusive(&observer->skipLock_);
            std::memcpy(observer->localSkip_, name, sizeof(name));
            ReleaseSRWLockExclusive(&observer->skipLock_);
        }
    }

    void CutsceneObserver::SetGate(const Gate gate, void* const context) noexcept
    {
        gateContext_.store(context, std::memory_order_release);
        gate_.store(gate, std::memory_order_release);
    }

    bool CutsceneObserver::Install(const HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept
    {
#if !defined(_M_IX86)
        (void)gameModule;
        diagnostics.Log("Hook: cutscene observation requires the x86 client.");
        return false;
#else
        if (gameModule == nullptr || start_.IsInstalled() ||
            (active_ != nullptr && active_ != this))
        {
            return false;
        }
        gameModule_ = gameModule;
        diagnostics_ = diagnostics;
        active_ = this;
        if (!start_.Install(
                reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(gameModule) + CutsceneStartRva),
                CutsceneStartPrefix.data(), CutsceneStartPrefix.size(),
                reinterpret_cast<void*>(&StartObserved), CutsceneStartPrefix.size()))
        {
            diagnostics_.Event("CutsceneObserverBlocked", "cutscene start prologue did not match the validated build");
            active_ = nullptr;
            gameModule_ = nullptr;
            return false;
        }
        originalStart_ = reinterpret_cast<StartFn>(start_.Original());
        unsigned char inputPrefix[8] = {0x83, 0xEC, 0x38, 0xA1};
        const auto cookie = reinterpret_cast<std::uintptr_t>(gameModule) + 0x0303E010;
        std::memcpy(inputPrefix + 4, &cookie, 4);
        if (!skipInput_.Install(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(gameModule) + 0x018C1580),
            inputPrefix, sizeof(inputPrefix), reinterpret_cast<void*>(&InputObserved), sizeof(inputPrefix)))
        {
            diagnostics_.Event("CutsceneSkipHookBlocked", "native cutscene input prologue mismatch");
            Shutdown();
            return false;
        }
        originalInput_ = reinterpret_cast<InputFn>(skipInput_.Original());
        diagnostics_.Event("CutsceneSkipHookReady", "retail cutscene input event39 observed and replayable");
        diagnostics_.Event("CutsceneObserverReady", "scripted cutscene starts and the HUD cutscene flag are observed");
        return true;
#endif
    }

    void CutsceneObserver::Shutdown() noexcept
    {
        if (!skipInput_.Shutdown() || !start_.Shutdown())
        {
            diagnostics_.Log("Hook: cutscene observer shutdown deferred because the target is owned by another hook.");
            return;
        }
        originalStart_ = nullptr;
        originalInput_ = nullptr;
        gameModule_ = nullptr;
        diagnostics_ = {};
        inCutscene_ = false;
        sampled_ = false;
        if (active_ == this) active_ = nullptr;
    }

    std::uintptr_t __cdecl CutsceneObserver::StartObserved(
        void* const name, void* const a2, const std::uint32_t a3, const std::uint32_t a4,
        const std::uint32_t a5, const std::uint32_t a6)
    {
        CutsceneObserver* const self = active_;
        if (self == nullptr || self->originalStart_ == nullptr) return 0;
        if (replaying_)
        {
            return self->originalStart_(name, a2, a3, a4, a5, a6);
        }
        char cutscene[96] = {};
        ReadName(name, cutscene, sizeof(cutscene));
        const Gate gate = self->gate_.load(std::memory_order_acquire);
        if (gate != nullptr && cutscene[0] != '\0')
        {
            gate(self->gateContext_.load(std::memory_order_acquire), cutscene);
        }
        const std::uint32_t ordinal = self->startCount_.fetch_add(1) + 1;
        char detail[256] = {};
        std::snprintf(detail, sizeof(detail),
            "ordinal=%u name=%s thread=%lu args=%p,%u,%u,%u,%u",
            ordinal, cutscene, GetCurrentThreadId(), a2, a3, a4, a5, a6);
        self->diagnostics_.Event("CutsceneStartRequested", detail);
        const std::uint64_t startedAt = GetTickCount64();
        self->SetActiveName(cutscene);
        const std::uintptr_t result = self->originalStart_(name, a2, a3, a4, a5, a6);
        self->SetActiveName("");
        std::snprintf(detail, sizeof(detail),
            "ordinal=%u name=%s result=%p blocked_ms=%llu",
            ordinal, cutscene, reinterpret_cast<void*>(result),
            static_cast<unsigned long long>(GetTickCount64() - startedAt));
        self->diagnostics_.Event("CutsceneStartReturned", detail);
        return result;
    }

    bool CutsceneObserver::ReadInCutscene(bool& value) const noexcept
    {
        value = false;
        if (gameModule_ == nullptr) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        __try
        {
            if (*reinterpret_cast<void* const*>(base + HudSingletonRva) == nullptr) return false;
            using IsInCutsceneFn = int(__cdecl*)();
            value = reinterpret_cast<IsInCutsceneFn>(base + IsInCutsceneRva)() != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void CutsceneObserver::Process(
        const char* const mapName, const float x, const float y, const float z) noexcept
    {
        bool now = false;
        if (!ReadInCutscene(now)) return;
        if (sampled_ && now == inCutscene_) return;
        const bool first = !sampled_;
        sampled_ = true;
        inCutscene_ = now;
        if (first && !now) return;
        char detail[256] = {};
        if (now)
        {
            enteredAt_ = GetTickCount64();
            std::snprintf(detail, sizeof(detail),
                "starts_so_far=%u hero_map=%s hero_position=(%.3f,%.3f,%.3f)",
                startCount_.load(), mapName != nullptr ? mapName : "", x, y, z);
            diagnostics_.Event("CutsceneModeEntered", detail);
        }
        else
        {
            std::snprintf(detail, sizeof(detail), "duration_ms=%llu",
                static_cast<unsigned long long>(GetTickCount64() - enteredAt_));
            diagnostics_.Event("CutsceneModeExited", detail);
        }
    }

    bool CutsceneObserver::Play(const char* const name) noexcept
    {
        if (gameModule_ == nullptr || originalStart_ == nullptr || name == nullptr ||
            name[0] == '\0' || scriptInterface_ == nullptr)
        {
            return false;
        }
        using CharStringConstructor = void(__thiscall*)(native::CharString*, const char*, int);
        using CharStringDestructor = void(__thiscall*)(native::CharString*);
        using BaseConstructor = void(__thiscall*)(void*);
        using CreateMovieResource = void(__thiscall*)(void*, const native::CharString*, void*);
        using SetCutsceneMode = void(__thiscall*)(void*, int);
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        const auto construct = reinterpret_cast<CharStringConstructor>(base + native::rva::CharStringConstructor);
        const auto destroy = reinterpret_cast<CharStringDestructor>(base + native::rva::CharStringDestructor);
        void* const iface = scriptInterface_;
        alignas(8) std::uint8_t movie[MovieResourceBytes] = {};
        native::CharString resource = {};
        native::CharString cutscene = {};
        bool resourceBuilt = false;
        bool nameBuilt = false;
        bool modeEntered = false;
        bool played = false;
        DWORD faultCode = 0;
        replaying_ = true;
        __try
        {
            void* const* const vtable = *reinterpret_cast<void* const* const*>(iface);
            reinterpret_cast<BaseConstructor>(base + MovieResourceBaseConstructorRva)(movie);
            *reinterpret_cast<std::uintptr_t*>(movie) = base + MovieResourceVtableRva;
            construct(&resource, "", -1);
            resourceBuilt = true;
            reinterpret_cast<CreateMovieResource>(vtable[CreateMovieResourceSlot])(iface, &resource, movie);
            reinterpret_cast<SetCutsceneMode>(vtable[SetCutsceneModeSlot])(iface, 1);
            modeEntered = true;
            construct(&cutscene, name, -1);
            nameBuilt = true;
            SetActiveName(name);
            originalStart_(&cutscene, movie, 0, 0, 0, 1);
            SetActiveName("");
            played = true;
        }
        __except (faultCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            played = false;
        }
        __try
        {
            if (modeEntered)
            {
                void* const* const vtable = *reinterpret_cast<void* const* const*>(iface);
                reinterpret_cast<SetCutsceneMode>(vtable[SetCutsceneModeSlot])(iface, 0);
            }
            if (nameBuilt) destroy(&cutscene);
            if (resourceBuilt) destroy(&resource);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        replaying_ = false;
        SetActiveName("");
        if (!played)
        {
            char detail[128] = {};
            std::snprintf(detail, sizeof(detail), "name=%s fault=0x%08lX mode_entered=%d",
                name, static_cast<unsigned long>(faultCode), modeEntered ? 1 : 0);
            diagnostics_.Event("CutsceneReplayFaulted", detail);
        }
        return played;
    }

    bool CutsceneObserver::SetCutsceneMode(const bool enabled) noexcept
    {
        void* const iface = scriptInterface_;
        if (iface == nullptr) return false;
        using SetCutsceneModeFn = void(__thiscall*)(void*, int);
        __try
        {
            void* const* const vtable = *reinterpret_cast<void* const* const*>(iface);
            reinterpret_cast<SetCutsceneModeFn>(vtable[SetCutsceneModeSlot])(iface, enabled ? 1 : 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CutsceneObserver::EndScriptedSequence() noexcept
    {
        void* const iface = scriptInterface_;
        if (iface == nullptr || gameModule_ == nullptr) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        using EndFn = void(__thiscall*)(void*);
        using ResolveFn = void*(__thiscall*)(void*, std::uint32_t);
        using RemoveModeFn = void(__thiscall*)(void*, std::int32_t);
        __try
        {
            reinterpret_cast<EndFn>(base + EndScriptedSequenceRva)(iface);
            auto* const fields = static_cast<std::uint8_t*>(iface);
            void* const controller = reinterpret_cast<ResolveFn>(base + ResolvePlayerControllerRva)(
                *reinterpret_cast<void**>(fields + 0x14), *reinterpret_cast<std::uint32_t*>(fields + 0x1C));
            if (controller != nullptr)
            {
                reinterpret_cast<RemoveModeFn>(base + RemovePlayerModeRva)(controller, ScriptedSequencePlayerMode);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CutsceneObserver::BeginScriptedSequence() noexcept
    {
        void* const iface = scriptInterface_;
        if (iface == nullptr || gameModule_ == nullptr) return false;
        using CharStringConstructor = void(__thiscall*)(native::CharString*, const char*, int);
        using CharStringDestructor = void(__thiscall*)(native::CharString*);
        using BaseConstructor = void(__thiscall*)(void*);
        using CreateMovieResource = void(__thiscall*)(void*, const native::CharString*, void*);
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        // Never freed: destroying it would end the sequence a second time.
        auto* const movie = static_cast<std::uint8_t*>(::operator new(MovieResourceBytes, std::nothrow));
        if (movie == nullptr) return false;
        std::memset(movie, 0, MovieResourceBytes);
        native::CharString resource = {};
        bool built = false;
        bool begun = false;
        __try
        {
            void* const* const vtable = *reinterpret_cast<void* const* const*>(iface);
            reinterpret_cast<BaseConstructor>(base + MovieResourceBaseConstructorRva)(movie);
            *reinterpret_cast<std::uintptr_t*>(movie) = base + MovieResourceVtableRva;
            reinterpret_cast<CharStringConstructor>(base + native::rva::CharStringConstructor)(&resource, "", -1);
            built = true;
            reinterpret_cast<CreateMovieResource>(vtable[CreateMovieResourceSlot])(iface, &resource, movie);
            begun = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            begun = false;
        }
        if (built)
        {
            __try
            {
                reinterpret_cast<CharStringDestructor>(base + native::rva::CharStringDestructor)(&resource);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return begun;
    }

    bool CutsceneObserver::YieldScriptFrame() noexcept
    {
        void* const iface = scriptInterface_;
        if (iface == nullptr) return false;
        using NewScriptFrame = void(__thiscall*)(void*);
        __try
        {
            void* const* const vtable = *reinterpret_cast<void* const* const*>(iface);
            reinterpret_cast<NewScriptFrame>(vtable[NewScriptFrameSlot])(iface);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

}
