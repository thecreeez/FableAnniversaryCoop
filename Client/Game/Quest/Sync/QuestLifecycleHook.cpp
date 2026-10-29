#include "QuestLifecycleHook.h"

#include "Core/Target/ExecutableValidator.h"
#include "Game/Native/Addresses.h"
#include "Game/Entity/Native/ThingComponentAccess.h"

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

    std::uint32_t CardDefinition(void* card) noexcept
    {
        __try { return card != nullptr ? *reinterpret_cast<const std::uint16_t*>(static_cast<unsigned char*>(card) + 0x98) : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    bool AssignCardQuest(void* component, const char* quest, std::uintptr_t base) noexcept
    {
        fable::game::native::CharString name;
        bool built = false, assigned = false;
        __try
        {
            reinterpret_cast<CharStringConstructor>(base + fable::game::native::rva::CharStringConstructor)(&name, quest, -1);
            built = true;
            using Assign = void(__thiscall*)(void*, const fable::game::native::CharString*);
            reinterpret_cast<Assign>(base + 0x012B7B30)(static_cast<unsigned char*>(component) + 0x28, &name);
            assigned = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { assigned = false; }
        if (built)
        {
            __try { reinterpret_cast<CharStringDestructor>(base + fable::game::native::rva::CharStringDestructor)(&name); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return assigned;
    }

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
    thread_local bool QuestLifecycleHook::acceptingRemoteCard_ = false;

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
        const auto acceptPrefix = RebasedSehPrefix(base, 0x0255DE48);
        const std::uint8_t rebuildPrefix[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
        const std::uint8_t eligibilityPrefix[] = {0x83, 0xEC, 0x08, 0x53, 0x8B, 0xD9};
        const bool installed =
            cardEligible_.Install(at(0x01A7D3E0), eligibilityPrefix, sizeof(eligibilityPrefix),
                reinterpret_cast<void*>(&CardEligible), sizeof(eligibilityPrefix)) &&
            acceptCard_.Install(at(0x01BC2820), acceptPrefix.data(), 7,
                reinterpret_cast<void*>(&CardAccepted), 7) &&
            rebuildMenu_.Install(at(0x01A28E10), rebuildPrefix, sizeof(rebuildPrefix),
                reinterpret_cast<void*>(&MenuRebuilt), sizeof(rebuildPrefix)) &&
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
        originalAcceptCard_ = reinterpret_cast<AcceptCardFn>(acceptCard_.Original());
        originalCardEligible_ = reinterpret_cast<CardEligibleFn>(cardEligible_.Original());
        originalRebuildMenu_ = reinterpret_cast<RebuildMenuFn>(rebuildMenu_.Original());
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
        const bool stopped = cardEligible_.Shutdown() & acceptCard_.Shutdown() & rebuildMenu_.Shutdown() & fail_.Shutdown() & complete_.Shutdown() &
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
        originalAcceptCard_ = nullptr;
        originalRebuildMenu_ = nullptr;
        originalCardEligible_ = nullptr;
        questInventory_ = nullptr;
        localHero_ = nullptr;
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
        if (acceptingRemoteCard_)
        {
            char quest[128] = {};
            if (ReadName(name, quest, sizeof(quest)) && self->IsActive(quest)) return true;
        }
        if (!replaying_ && self->IsDuplicate(QuestEventOperation::Activate, name)) return true;
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
        if (!replaying_ && self->IsDuplicate(QuestEventOperation::Deactivate, name)) return;
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
        if (!replaying_ && self->IsDuplicate(QuestEventOperation::Complete, name)) return;
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
        if (!replaying_ && self->IsDuplicate(QuestEventOperation::Fail, name)) return;
        if (!replaying_) self->Record(QuestEventOperation::Fail, name, a2, a3, a4);
        self->originalFail_(manager, name, a2, a3, a4);
        self->generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    bool QuestLifecycleHook::IsDuplicate(
        const QuestEventOperation operation,
        const game::native::CharString* const name) noexcept
    {
        const DuplicateFilter filter = duplicateFilter_;
        if (filter == nullptr) return false;
        char buffer[protocol::MaximumQuestEventNameBytes] = {};
        if (!ReadName(name, buffer, sizeof(buffer))) return false;
        if (!filter(duplicateContext_, operation, buffer)) return false;
        char detail[160] = {};
        std::snprintf(detail, sizeof(detail), "op=%u quest=%s",
            static_cast<unsigned>(operation), buffer);
        diagnostics_.Event("MultiplayerQuestDuplicateSuppressed", detail);
        return true;
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

    bool __fastcall QuestLifecycleHook::CardEligible(void* card, void*)
    {
        if (acceptingRemoteCard_) return true; // Initiator already passed the retail requirements.
        auto* self = active_;
        return self != nullptr && self->originalCardEligible_ != nullptr && self->originalCardEligible_(card);
    }

    bool __fastcall QuestLifecycleHook::CardAccepted(void* manager, void*, void* card, int flags)
    {
        auto* self = active_;
        if (self == nullptr || self->originalAcceptCard_ == nullptr) return false;
        auto* component = static_cast<unsigned char*>(game::entity::native::ThingComponentAccess::Find(
            card, static_cast<game::entity::native::ThingComponentType>(0x6C)));
        char quest[128] = {};
        const auto definition = CardDefinition(card);
        if (component != nullptr) ReadName(reinterpret_cast<game::native::CharString*>(component + 0x28), quest, sizeof(quest));
        const bool accepted = self->originalAcceptCard_(manager, card, flags);
        if (accepted && !replaying_ && quest[0] != '\0')
        {
            const auto base = reinterpret_cast<std::uintptr_t>(self->gameModule_);
            game::native::CharString name;
            reinterpret_cast<CharStringConstructor>(base + game::native::rva::CharStringConstructor)(&name, quest, -1);
            self->Record(QuestEventOperation::AcceptQuestCard, &name, definition, 0, 0);
            reinterpret_cast<CharStringDestructor>(base + game::native::rva::CharStringDestructor)(&name);
        }
        return accepted;
    }

    void __fastcall QuestLifecycleHook::MenuRebuilt(void* inventory, void*)
    {
        auto* self = active_;
        if (self == nullptr || self->originalRebuildMenu_ == nullptr) return;
        self->questInventory_ = inventory;
        self->originalRebuildMenu_(inventory);
    }

    void QuestLifecycleHook::RefreshOpenMenu() noexcept
    {
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        __try
        {
            auto* inventory = static_cast<unsigned char*>(game::entity::native::ThingComponentAccess::Find(
                localHero_, static_cast<game::entity::native::ThingComponentType>(0x19)));
            if (inventory == nullptr || originalRebuildMenu_ == nullptr ||
                *reinterpret_cast<std::uintptr_t*>(inventory) != base + 0x02B00394 ||
                inventory[0x60] == 0) return;
            originalRebuildMenu_(inventory);
            reinterpret_cast<void(__thiscall*)(void*, bool)>(base + 0x019EF818)(inventory + 0x184, true);
            diagnostics_.Event("MultiplayerOpenQuestMenuRefreshed", "native quest list rebuilt without closing inventory");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { questInventory_ = nullptr; }
    }

    bool QuestLifecycleHook::OpenMenuForTest() noexcept
    {
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        void* inventory = game::entity::native::ThingComponentAccess::Find(localHero_,
            static_cast<game::entity::native::ThingComponentType>(0x19));
        __try
        {
            if (inventory == nullptr || *reinterpret_cast<std::uintptr_t*>(inventory) != base + 0x02B00394) return false;
            auto** table = *reinterpret_cast<void***>(inventory);
            if (reinterpret_cast<std::uintptr_t>(table[32]) != base + 0x01A2B810) return false;
            reinterpret_cast<void(__thiscall*)(void*)>(table[32])(inventory);
            return static_cast<unsigned char*>(inventory)[0x60] != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void* QuestLifecycleHook::CreateCard(std::uint32_t definition, const char* quest) noexcept
    {
        if (definition == 0 || definition > 0xFFFF || localHero_ == nullptr) return nullptr;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        game::native::CharString empty;
        bool built = false;
        void* result = nullptr;
        __try
        {
            const auto* entry = reinterpret_cast<const unsigned char*>(base + 0x01B58490);
            if (entry[0] != 0x6A || entry[1] != 0xFF || entry[2] != 0x68 ||
                *reinterpret_cast<const std::uintptr_t*>(entry + 3) != base + 0x02555780) return nullptr;
            reinterpret_cast<CharStringConstructor>(base + game::native::rva::CharStringConstructor)(&empty, "", -1);
            built = true;
            void* position = reinterpret_cast<void*(__thiscall*)(void*)>(base + 0x01B2B440)(localHero_);
            const int map = *reinterpret_cast<signed char*>(static_cast<unsigned char*>(localHero_) + 0x9C);
            using Factory = void*(__cdecl*)(int, void*, int, int, int, int, game::native::CharString*);
            result = reinterpret_cast<Factory>(base + 0x01B58490)(definition, position, map, 1, 0, 0, &empty);
            void* component = game::entity::native::ThingComponentAccess::Find(result,
                static_cast<game::entity::native::ThingComponentType>(0x6C));
            // Stock GiveQuestCard assigns its separate script-name argument
            // after constructing the definition (0189AEE2..0189AEEA).
            if (component == nullptr || !AssignCardQuest(component, quest, base))
            {
                if (result != nullptr) reinterpret_cast<void(__thiscall*)(void*, bool)>(base + 0x01B2E530)(result, false);
                result = nullptr;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { result = nullptr; }
        if (built)
        {
            __try { reinterpret_cast<CharStringDestructor>(base + game::native::rva::CharStringDestructor)(&empty); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return result;
    }

    bool QuestLifecycleHook::AcceptAvailableCardForTest(char (&quest)[128]) noexcept
    {
        quest[0] = 0;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        __try
        {
            auto* manager = static_cast<unsigned char*>(Manager());
            if (manager == nullptr) return false;
            auto* head = *reinterpret_cast<unsigned char**>(manager + 0x88);
            auto* node = *reinterpret_cast<unsigned char**>(head);
            for (unsigned count = 0; node != head && count < 1024; ++count)
            {
                auto* next = *reinterpret_cast<unsigned char**>(node);
                void* card = reinterpret_cast<void*(__thiscall*)(void*)>(base + 0x012E6EA0)(node + 8);
                auto* component = static_cast<unsigned char*>(game::entity::native::ThingComponentAccess::Find(
                    card, static_cast<game::entity::native::ThingComponentType>(0x6C)));
                char candidate[128] = {};
                if (component != nullptr && *reinterpret_cast<int*>(component + 0x14) < 0 &&
                    ReadName(reinterpret_cast<game::native::CharString*>(component + 0x28), candidate, sizeof(candidate)) &&
                    !IsActive(candidate) && !IsCompleted(candidate) &&
                    reinterpret_cast<AcceptCardFn>(base + 0x01BC2820)(manager, card, 0))
                {
                    std::memcpy(quest, candidate, sizeof(quest));
                    return true;
                }
                node = next;
            }
            struct Offer { const char* definition; const char* quest; };
            const Offer offers[] = {
                {"OBJECT_QUEST_CARD_BREAK_SIEGE", "Q_BreakSiege"},
                {"OBJECT_QUEST_CARD_LOST_TRADER", "V_LostTrader"},
                {"OBJECT_QUEST_CARD_AMBUSH_TRADERS", "Q_AmbushTraders"},
                {"OBJECT_QUEST_CARD_ASK_ORACLE", "Q_AwakeningTheOracle"}};
            for (const auto& offer : offers)
            {
                const char* definitionName = offer.definition;
                game::native::CharString name;
                reinterpret_cast<CharStringConstructor>(base + game::native::rva::CharStringConstructor)(&name, definitionName, -1);
                void* thingManager = *reinterpret_cast<void**>(base + game::native::rva::ThingManagerSlot);
                void* definitions = thingManager != nullptr ? *reinterpret_cast<void**>(static_cast<unsigned char*>(thingManager) + 0x2C) : nullptr;
                const int definition = definitions != nullptr ? reinterpret_cast<int(__thiscall*)(void*, game::native::CharString*)>(base + game::native::rva::DefinitionIndexByName)(definitions, &name) : 0;
                reinterpret_cast<CharStringDestructor>(base + game::native::rva::CharStringDestructor)(&name);
                void* card = CreateCard(definition, offer.quest);
                auto* component = static_cast<unsigned char*>(game::entity::native::ThingComponentAccess::Find(card,
                    static_cast<game::entity::native::ThingComponentType>(0x6C)));
                char candidate[128] = {};
                if (component != nullptr) ReadName(reinterpret_cast<game::native::CharString*>(component + 0x28), candidate, sizeof(candidate));
                char probe[256] = {};
                std::snprintf(probe, sizeof(probe), "definition=%s index=%d component=%p quest=%s active=%d completed=%d eligible=%d",
                    definitionName, definition, component, candidate, IsActive(candidate), IsCompleted(candidate),
                    component != nullptr && originalCardEligible_(component));
                diagnostics_.Event("QuestCardAcceptanceCandidate", probe);
                if (component != nullptr && ReadName(reinterpret_cast<game::native::CharString*>(component + 0x28), candidate, sizeof(candidate)) &&
                    !IsActive(candidate) && !IsCompleted(candidate) &&
                    reinterpret_cast<AcceptCardFn>(base + 0x01BC2820)(manager, card, 0))
                {
                    std::memcpy(quest, candidate, sizeof(quest));
                    return true;
                }
                if (card != nullptr) reinterpret_cast<void(__thiscall*)(void*, bool)>(base + 0x01B2E530)(card, false);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return false;
    }

    bool QuestLifecycleHook::AcceptCard(const char* quest, std::uint32_t definition) noexcept
    {
        if (quest == nullptr || originalAcceptCard_ == nullptr || Manager() == nullptr) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        game::native::CharString name;
        bool built = false, accepted = false;
        const bool previousReplay = replaying_;
        __try
        {
            reinterpret_cast<CharStringConstructor>(base + game::native::rva::CharStringConstructor)(&name, quest, -1);
            built = true;
            using Find = void*(__thiscall*)(void*, game::native::CharString*);
            void* card = reinterpret_cast<Find>(base + 0x01BBAE60)(Manager(), &name);
            bool created = false;
            if (card == nullptr) { card = CreateCard(definition, quest); created = card != nullptr; }
            if (card != nullptr)
            {
                auto* component = static_cast<unsigned char*>(game::entity::native::ThingComponentAccess::Find(card,
                    static_cast<game::entity::native::ThingComponentType>(0x6C)));
                char cardQuest[128] = {};
                const bool matches = component != nullptr && ReadName(reinterpret_cast<game::native::CharString*>(component + 0x28), cardQuest, sizeof(cardQuest)) &&
                    std::strcmp(cardQuest, quest) == 0;
                if (!matches)
                {
                    if (created) reinterpret_cast<void(__thiscall*)(void*, bool)>(base + 0x01B2E530)(card, false);
                }
                else
                {
                acceptingRemoteCard_ = true;
                replaying_ = true;
                accepted = originalAcceptCard_(Manager(), card, 0);
                if (!accepted && created) reinterpret_cast<void(__thiscall*)(void*, bool)>(base + 0x01B2E530)(card, false);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { accepted = false; }
        replaying_ = previousReplay;
        acceptingRemoteCard_ = false;
        if (built)
        {
            __try { reinterpret_cast<CharStringDestructor>(base + game::native::rva::CharStringDestructor)(&name); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (accepted) { generation_.fetch_add(1); RefreshOpenMenu(); }
        return accepted;
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
