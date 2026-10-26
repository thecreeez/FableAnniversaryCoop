#include "QuestLifecycleHook.h"

#include "Core/Target/ExecutableValidator.h"
#include "Game/Native/Addresses.h"

#include <cstdio>
#include <cstring>

namespace
{
    using fable::multiplayer::protocol::QuestEventOperation;

    // Activate, Complete and Fail open with the same 14-byte SEH frame
    // prefix; only the scope-table immediate at +3 differs. That operand is
    // absolute, so it is rebased before the exact comparison.
    constexpr std::array<std::uint8_t, 14> SehPrefix = {
        0x6A, 0xFF, 0x68, 0x00, 0x00, 0x00, 0x00, 0x64,
        0xA1, 0x00, 0x00, 0x00, 0x00, 0x50};
    constexpr std::uintptr_t ActivateScopeTable = 0x0252B238;
    constexpr std::uintptr_t CompleteScopeTable = 0x0255D80B;
    constexpr std::uintptr_t FailScopeTable = 0x0255D838;
    // push ebx; push ebp; mov ebp, ecx; mov ebx, [ebp+44h]
    constexpr std::array<std::uint8_t, 7> DeactivatePrefix = {
        0x53, 0x55, 0x8B, 0xE9, 0x8B, 0x5D, 0x44};

    using CharStringConstructor = void(__thiscall*)(
        fable::game::native::CharString*, const char*, int);
    using CharStringDestructor = void(__thiscall*)(
        fable::game::native::CharString*);

    std::array<std::uint8_t, 14> RebasedSehPrefix(
        const std::uintptr_t base,
        const std::uintptr_t scopeTableRva) noexcept
    {
        std::array<std::uint8_t, 14> expected = SehPrefix;
        const auto scopeTable = static_cast<std::uint32_t>(base + scopeTableRva);
        std::memcpy(expected.data() + 3, &scopeTable, sizeof(scopeTable));
        return expected;
    }

    // Reads the counted string behind a retail CCharString without touching
    // its reference count.
    bool ReadName(
        const fable::game::native::CharString* name,
        char* destination,
        const std::size_t capacity) noexcept
    {
        destination[0] = '\0';
        __try
        {
            if (name == nullptr || name->stringData == nullptr) return false;
            const auto* rep =
                static_cast<const fable::game::native::StringRep*>(name->stringData);
            if (rep->data == nullptr || rep->len <= 0 ||
                static_cast<std::size_t>(rep->len) >= capacity)
            {
                return false;
            }
            std::memcpy(destination, rep->data, static_cast<std::size_t>(rep->len));
            destination[rep->len] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            destination[0] = '\0';
            return false;
        }
    }
}

namespace fable::multiplayer::persistence
{
    QuestLifecycleHook* QuestLifecycleHook::active_ = nullptr;
    thread_local bool QuestLifecycleHook::replaying_ = false;

    bool QuestLifecycleHook::Install(
        const HMODULE gameModule,
        const core::Diagnostics& diagnostics) noexcept
    {
#if !defined(_M_IX86)
        (void)gameModule;
        diagnostics.Log("Hook: quest lifecycle sync requires the x86 client.");
        return false;
#else
        if (gameModule == nullptr || IsInstalled() ||
            (active_ != nullptr && active_ != this))
        {
            return false;
        }
        if (!core::target::ValidateFableExecutable(gameModule, nullptr))
        {
            diagnostics.Log(
                "Hook: quest lifecycle sync refused because the current PE build was not validated.");
            return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        const auto at = [base](const std::uintptr_t rva)
        {
            return reinterpret_cast<void*>(base + rva);
        };
        gameModule_ = gameModule;
        diagnostics_ = diagnostics;
        active_ = this;

        const auto activatePrefix = RebasedSehPrefix(base, ActivateScopeTable);
        const auto completePrefix = RebasedSehPrefix(base, CompleteScopeTable);
        const auto failPrefix = RebasedSehPrefix(base, FailScopeTable);
        const bool installed =
            activate_.Install(
                at(game::native::rva::QuestManagerActivate),
                activatePrefix.data(), activatePrefix.size(),
                reinterpret_cast<void*>(&ActivateObserved), activatePrefix.size()) &&
            deactivate_.Install(
                at(game::native::rva::QuestManagerDeactivate),
                DeactivatePrefix.data(), DeactivatePrefix.size(),
                reinterpret_cast<void*>(&DeactivateObserved), DeactivatePrefix.size()) &&
            complete_.Install(
                at(game::native::rva::QuestManagerComplete),
                completePrefix.data(), completePrefix.size(),
                reinterpret_cast<void*>(&CompleteObserved), completePrefix.size()) &&
            fail_.Install(
                at(game::native::rva::QuestManagerFail),
                failPrefix.data(), failPrefix.size(),
                reinterpret_cast<void*>(&FailObserved), failPrefix.size());
        if (!installed)
        {
            diagnostics_.Event(
                "MultiplayerQuestLifecycleHookBlocked",
                "one or more CQuestManager lifecycle prologues did not match the validated build");
            Shutdown();
            return false;
        }
        originalActivate_ = reinterpret_cast<ActivateFn>(activate_.Original());
        originalDeactivate_ = reinterpret_cast<DeactivateFn>(deactivate_.Original());
        originalComplete_ = reinterpret_cast<ResolveFn>(complete_.Original());
        originalFail_ = reinterpret_cast<ResolveFn>(fail_.Original());
        diagnostics_.Event(
            "MultiplayerQuestLifecycleHookReady",
            "CQuestManager activate/deactivate/complete/fail transitions are observed");
        return true;
#endif
    }

    void QuestLifecycleHook::Shutdown() noexcept
    {
        const bool stopped = fail_.Shutdown() & complete_.Shutdown() &
            deactivate_.Shutdown() & activate_.Shutdown();
        if (!stopped)
        {
            diagnostics_.Log(
                "Hook: quest lifecycle shutdown deferred because a target is owned by another hook.");
            return;
        }
        originalActivate_ = nullptr;
        originalDeactivate_ = nullptr;
        originalComplete_ = nullptr;
        originalFail_ = nullptr;
        AcquireSRWLockExclusive(&queueLock_);
        queueHead_ = 0;
        queueCount_ = 0;
        dropped_ = 0;
        ReleaseSRWLockExclusive(&queueLock_);
        gameModule_ = nullptr;
        diagnostics_ = {};
        if (active_ == this) active_ = nullptr;
    }

    bool __fastcall QuestLifecycleHook::ActivateObserved(
        void* const manager, void*, game::native::CharString* const name,
        const std::uint32_t a2, const std::uint32_t a3)
    {
        QuestLifecycleHook* const self = active_;
        if (self == nullptr || self->originalActivate_ == nullptr) return false;
        const bool accepted = self->originalActivate_(manager, name, a2, a3);
        self->generation_.fetch_add(1, std::memory_order_acq_rel);
        if (accepted && !replaying_)
        {
            self->Record(QuestEventOperation::Activate, name, a2, a3, 0);
        }
        return accepted;
    }

    void __fastcall QuestLifecycleHook::DeactivateObserved(
        void* const manager, void*, game::native::CharString* const name,
        const std::uint32_t a2)
    {
        QuestLifecycleHook* const self = active_;
        if (self == nullptr || self->originalDeactivate_ == nullptr) return;
        // Capture the name first: retail may release the quest card that
        // owns the string during deactivation.
        if (!replaying_) self->Record(QuestEventOperation::Deactivate, name, a2, 0, 0);
        self->originalDeactivate_(manager, name, a2);
        self->generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    void __fastcall QuestLifecycleHook::CompleteObserved(
        void* const manager, void*, game::native::CharString* const name,
        const std::uint32_t a2, const std::uint32_t a3, const std::uint32_t a4)
    {
        QuestLifecycleHook* const self = active_;
        if (self == nullptr || self->originalComplete_ == nullptr) return;
        if (!replaying_) self->Record(QuestEventOperation::Complete, name, a2, a3, a4);
        self->originalComplete_(manager, name, a2, a3, a4);
        self->generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    void __fastcall QuestLifecycleHook::FailObserved(
        void* const manager, void*, game::native::CharString* const name,
        const std::uint32_t a2, const std::uint32_t a3, const std::uint32_t a4)
    {
        QuestLifecycleHook* const self = active_;
        if (self == nullptr || self->originalFail_ == nullptr) return;
        if (!replaying_) self->Record(QuestEventOperation::Fail, name, a2, a3, a4);
        self->originalFail_(manager, name, a2, a3, a4);
        self->generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    void QuestLifecycleHook::Record(
        const QuestEventOperation operation,
        const game::native::CharString* const name,
        const std::uint32_t a2, const std::uint32_t a3, const std::uint32_t a4) noexcept
    {
        char buffer[protocol::MaximumQuestEventNameBytes] = {};
        if (!ReadName(name, buffer, sizeof(buffer))) return;
        AcquireSRWLockExclusive(&queueLock_);
        if (queueCount_ == QueueCapacity)
        {
            ++dropped_;
        }
        else
        {
            try
            {
                Observation& slot = queue_[(queueHead_ + queueCount_) % QueueCapacity];
                slot.operation = operation;
                slot.arguments[0] = a2;
                slot.arguments[1] = a3;
                slot.arguments[2] = a4;
                slot.name.assign(buffer);
                ++queueCount_;
            }
            catch (...)
            {
                ++dropped_;
            }
        }
        ReleaseSRWLockExclusive(&queueLock_);
    }

    bool QuestLifecycleHook::PopObservation(Observation& observation) noexcept
    {
        std::size_t dropped = 0;
        bool popped = false;
        AcquireSRWLockExclusive(&queueLock_);
        if (queueCount_ != 0)
        {
            std::swap(observation, queue_[queueHead_]);
            queueHead_ = (queueHead_ + 1) % QueueCapacity;
            --queueCount_;
            popped = true;
        }
        std::swap(dropped, dropped_);
        ReleaseSRWLockExclusive(&queueLock_);
        if (dropped != 0)
        {
            char detail[96] = {};
            std::snprintf(detail, sizeof(detail), "dropped=%zu capacity=%zu", dropped, QueueCapacity);
            diagnostics_.Event("MultiplayerQuestEventQueueOverflow", detail);
        }
        return popped;
    }

    void* QuestLifecycleHook::Manager() const noexcept
    {
        if (gameModule_ == nullptr) return nullptr;
        __try
        {
            return *reinterpret_cast<void* const*>(
                reinterpret_cast<std::uintptr_t>(gameModule_) +
                game::native::rva::QuestManagerGlobal);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool QuestLifecycleHook::IsManagerReady() const noexcept
    {
        return IsInstalled() && Manager() != nullptr;
    }

    bool QuestLifecycleHook::Query(const PredicateFn predicate, const char* const name) const noexcept
    {
        void* const manager = Manager();
        if (manager == nullptr || name == nullptr || name[0] == '\0') return false;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        const auto construct = reinterpret_cast<CharStringConstructor>(
            base + game::native::rva::CharStringConstructor);
        const auto destroy = reinterpret_cast<CharStringDestructor>(
            base + game::native::rva::CharStringDestructor);
        game::native::CharString nativeName;
        bool constructed = false;
        bool result = false;
        __try
        {
            construct(&nativeName, name, -1);
            constructed = true;
            result = predicate(manager, &nativeName);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            result = false;
        }
        if (constructed)
        {
            __try { destroy(&nativeName); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return result;
    }

    bool QuestLifecycleHook::IsActive(const char* const name) const noexcept
    {
        if (gameModule_ == nullptr) return false;
        return Query(reinterpret_cast<PredicateFn>(
            reinterpret_cast<std::uintptr_t>(gameModule_) +
            game::native::rva::QuestManagerIsActive), name);
    }

    bool QuestLifecycleHook::IsCompleted(const char* const name) const noexcept
    {
        if (gameModule_ == nullptr) return false;
        return Query(reinterpret_cast<PredicateFn>(
            reinterpret_cast<std::uintptr_t>(gameModule_) +
            game::native::rva::QuestManagerIsCompleted), name);
    }

    bool QuestLifecycleHook::Replay(
        const QuestEventOperation operation,
        const char* const name,
        const std::uint32_t (&arguments)[3]) noexcept
    {
        return Call(operation, name, arguments, true);
    }

    bool QuestLifecycleHook::InvokeAsGameplay(
        const QuestEventOperation operation,
        const char* const name,
        const std::uint32_t (&arguments)[3]) noexcept
    {
        return Call(operation, name, arguments, false);
    }

    bool QuestLifecycleHook::Call(
        const QuestEventOperation operation,
        const char* const name,
        const std::uint32_t (&arguments)[3],
        const bool replay) noexcept
    {
        void* const manager = Manager();
        if (manager == nullptr || !IsInstalled() || name == nullptr || name[0] == '\0')
        {
            return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        const auto construct = reinterpret_cast<CharStringConstructor>(
            base + game::native::rva::CharStringConstructor);
        const auto destroy = reinterpret_cast<CharStringDestructor>(
            base + game::native::rva::CharStringDestructor);
        // A replay bypasses the detours through the trampolines; gameplay
        // invocation enters the patched retail entry points.
        const auto activate = replay ? originalActivate_
            : reinterpret_cast<ActivateFn>(base + game::native::rva::QuestManagerActivate);
        const auto deactivate = replay ? originalDeactivate_
            : reinterpret_cast<DeactivateFn>(base + game::native::rva::QuestManagerDeactivate);
        const auto complete = replay ? originalComplete_
            : reinterpret_cast<ResolveFn>(base + game::native::rva::QuestManagerComplete);
        const auto fail = replay ? originalFail_
            : reinterpret_cast<ResolveFn>(base + game::native::rva::QuestManagerFail);
        game::native::CharString nativeName;
        bool constructed = false;
        bool accepted = false;
        replaying_ = replay;
        __try
        {
            construct(&nativeName, name, -1);
            constructed = true;
            switch (operation)
            {
            case QuestEventOperation::Activate:
                accepted = activate(manager, &nativeName, arguments[0], arguments[1]);
                break;
            case QuestEventOperation::Deactivate:
                deactivate(manager, &nativeName, arguments[0]);
                accepted = true;
                break;
            case QuestEventOperation::Complete:
                complete(manager, &nativeName, arguments[0], arguments[1], arguments[2]);
                accepted = true;
                break;
            case QuestEventOperation::Fail:
                fail(manager, &nativeName, arguments[0], arguments[1], arguments[2]);
                accepted = true;
                break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            accepted = false;
        }
        replaying_ = false;
        // Trampoline replays bypass the detours that normally bump this.
        if (replay) generation_.fetch_add(1, std::memory_order_acq_rel);
        if (constructed)
        {
            __try { destroy(&nativeName); }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // The transition already crossed the native boundary; never
                // retry it because only the local string cleanup faulted.
                diagnostics_.Event("MultiplayerQuestReplayCleanupFault", name);
            }
        }
        return accepted;
    }
}
