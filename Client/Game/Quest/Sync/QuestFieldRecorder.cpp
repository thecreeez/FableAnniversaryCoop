#include "QuestFieldRecorder.h"

#include "Game/Native/Addresses.h"
#include "Game/Native/ScriptTypes.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace
{
    // CScriptBase::PersistState(this = script, CCharString* text, bool save):
    // with save=1 it builds a writing CPersistContext over `text` and runs
    // the script's Persist slot (vtable +0x10); save=0 is the load path.
    constexpr std::uintptr_t ScriptPersistRva = 0x013761B0;
    // CPersistContext::Transfer primitives: (name, field*, default*).
    constexpr std::uintptr_t TransferBoolRva = 0x0137B510;
    constexpr std::uintptr_t TransferIntRva = 0x0137B440;
    constexpr std::uintptr_t TransferFloatRva = 0x0137B360;

    // sub esp, 8; push esi; push edi; mov edi, [esp+14h]
    constexpr std::array<std::uint8_t, 9> TransferPrefix = {
        0x83, 0xEC, 0x08, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14};

    // CQuestManager: +0x44 active-quest list head; node [0] next, [8] quest.
    // Quest object +0x34: name string rep ([+4] chars, [+8] length).
    constexpr std::size_t ActiveQuestListOffset = 0x44;
    constexpr std::size_t QuestNodeValueOffset = 8;
    constexpr std::size_t QuestNameOffset = 0x34;
    // Quest-object words searched for the owning NScript pointer.
    constexpr std::size_t QuestObjectScanBytes = 0x100;

    // Quest-script progress fields live inside the script object; anything
    // further away belongs to an entity script persisted from inside it.
    constexpr std::uintptr_t ScriptObjectSpan = 0x2000;

    using CharStringConstructor = void(__thiscall*)(fable::game::native::CharString*, const char*, int);
    using CharStringDestructor = void(__thiscall*)(fable::game::native::CharString*);
}

namespace fable::multiplayer::persistence
{
    QuestFieldRecorder* QuestFieldRecorder::active_ = nullptr;
    std::atomic<void*> QuestFieldRecorder::recordingScript_{nullptr};
    SRWLOCK QuestFieldRecorder::observedLock_ = SRWLOCK_INIT;
    std::size_t QuestFieldRecorder::observedCount_ = 0;
    std::atomic_uint32_t QuestFieldRecorder::windowTransfers_{0};
    std::atomic_long QuestFieldRecorder::firstDelta_{0};
    std::size_t QuestFieldRecorder::scratchBytes_ = 0;
    QuestFieldRecorder::ObservedField
        QuestFieldRecorder::observed_[QuestFieldRecorder::MaximumObservedPerScript] = {};

    bool QuestFieldRecorder::Install(
        const HMODULE gameModule,
        const core::Diagnostics& diagnostics) noexcept
    {
#if !defined(_M_IX86)
        (void)gameModule;
        diagnostics.Log("Hook: quest field recording requires the x86 client.");
        return false;
#else
        if (gameModule == nullptr || IsInstalled() ||
            (active_ != nullptr && active_ != this))
        {
            return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        gameModule_ = gameModule;
        diagnostics_ = diagnostics;
        active_ = this;
        const auto at = [base](const std::uintptr_t rva)
        {
            return reinterpret_cast<void*>(base + rva);
        };
        const bool installed =
            transferBool_.Install(at(TransferBoolRva), TransferPrefix.data(), TransferPrefix.size(),
                reinterpret_cast<void*>(&TransferBoolObserved), TransferPrefix.size()) &&
            transferInt_.Install(at(TransferIntRva), TransferPrefix.data(), TransferPrefix.size(),
                reinterpret_cast<void*>(&TransferIntObserved), TransferPrefix.size()) &&
            transferFloat_.Install(at(TransferFloatRva), TransferPrefix.data(), TransferPrefix.size(),
                reinterpret_cast<void*>(&TransferFloatObserved), TransferPrefix.size());
        if (!installed)
        {
            diagnostics_.Event(
                "MultiplayerQuestFieldRecorderBlocked",
                "Transfer prologues did not match the validated build");
            Shutdown();
            return false;
        }
        originalBool_ = reinterpret_cast<TransferFn>(transferBool_.Original());
        originalInt_ = reinterpret_cast<TransferFn>(transferInt_.Original());
        originalFloat_ = reinterpret_cast<TransferFn>(transferFloat_.Original());
        diagnostics_.Event(
            "MultiplayerQuestFieldRecorderReady",
            "running quest-script Transfer(bool/int/float) fields can be mapped to live memory");
        return true;
#endif
    }

    void QuestFieldRecorder::Shutdown() noexcept
    {
        const bool stopped = transferFloat_.Shutdown() & transferInt_.Shutdown() &
            transferBool_.Shutdown();
        if (!stopped)
        {
            diagnostics_.Log(
                "Hook: quest field recorder shutdown deferred because a target is owned by another hook.");
            return;
        }
        originalBool_ = nullptr;
        originalInt_ = nullptr;
        originalFloat_ = nullptr;
        gameModule_ = nullptr;
        diagnostics_ = {};
        reported_ = false;
        if (active_ == this) active_ = nullptr;
    }

    bool QuestFieldRecorder::IsInstalled() const noexcept
    {
        return transferBool_.IsInstalled() && transferInt_.IsInstalled() &&
            transferFloat_.IsInstalled();
    }

#define FABLE_QUEST_FIELD_TRANSFER(NAME, ORIGINAL, KIND)                                   \
    void __fastcall QuestFieldRecorder::NAME(                                              \
        void* const context, void*, const char* const name, void* const field,             \
        const void* const fallback)                                                        \
    {                                                                                      \
        QuestFieldRecorder* const self = active_;                                          \
        if (self == nullptr || self->ORIGINAL == nullptr) return;                          \
        self->ORIGINAL(context, name, field, fallback);                                    \
        if (recordingScript_.load(std::memory_order_acquire)) Observe(KIND, name, field);                       \
    }

    FABLE_QUEST_FIELD_TRANSFER(TransferBoolObserved, originalBool_, Kind::Bool)
    FABLE_QUEST_FIELD_TRANSFER(TransferIntObserved, originalInt_, Kind::Int32)
    FABLE_QUEST_FIELD_TRANSFER(TransferFloatObserved, originalFloat_, Kind::Float32)
#undef FABLE_QUEST_FIELD_TRANSFER

    void QuestFieldRecorder::Observe(const Kind kind, const char* const name, void* const field) noexcept
    {
        const auto scriptAddress = reinterpret_cast<std::uintptr_t>(
            recordingScript_.load(std::memory_order_acquire));
        const auto fieldAddress = reinterpret_cast<std::uintptr_t>(field);
        windowTransfers_.fetch_add(1, std::memory_order_relaxed);
        if (name == nullptr || scriptAddress == 0 || fieldAddress < scriptAddress ||
            fieldAddress >= scriptAddress + ScriptObjectSpan)
        {
            if (scriptAddress != 0 && firstDelta_.load(std::memory_order_relaxed) == 0)
            {
                firstDelta_.store(static_cast<long>(fieldAddress - scriptAddress), std::memory_order_relaxed);
            }
            return;
        }
        AcquireSRWLockExclusive(&observedLock_);
        if (observedCount_ < MaximumObservedPerScript)
        {
            observed_[observedCount_++] = {name, kind, field};
        }
        ReleaseSRWLockExclusive(&observedLock_);
    }

    std::size_t QuestFieldRecorder::CollectRunningQuests(
        RunningQuest* const quests, const std::size_t capacity) const noexcept
    {
        std::size_t count = 0;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        __try
        {
            const auto manager = *reinterpret_cast<const std::uintptr_t*>(
                base + game::native::rva::QuestManagerGlobal);
            if (manager == 0) return 0;
            const auto head = *reinterpret_cast<const std::uintptr_t*>(manager + ActiveQuestListOffset);
            std::uintptr_t node = *reinterpret_cast<const std::uintptr_t*>(head);
            while (node != 0 && node != head && count < capacity)
            {
                const auto quest = *reinterpret_cast<const std::uintptr_t*>(node + QuestNodeValueOffset);
                node = *reinterpret_cast<const std::uintptr_t*>(node);
                if (quest == 0) continue;
                const auto* rep = *reinterpret_cast<const game::native::StringRep* const*>(quest + QuestNameOffset);
                if (rep == nullptr || rep->data == nullptr || rep->len <= 0 ||
                    static_cast<std::size_t>(rep->len) >= sizeof(quests[count].name))
                {
                    continue;
                }
                void* script = nullptr;
                for (std::size_t offset = 0; offset < QuestObjectScanBytes; offset += sizeof(void*))
                {
                    void* const candidate = *reinterpret_cast<void* const*>(quest + offset);
                    if (IsQuestScriptObject(candidate))
                    {
                        script = candidate;
                        break;
                    }
                }
                if (script == nullptr) continue;
                std::memcpy(quests[count].name, rep->data, static_cast<std::size_t>(rep->len));
                quests[count].name[rep->len] = '\0';
                quests[count].script = script;
                ++count;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return count;
    }

    bool QuestFieldRecorder::IsQuestScriptObject(const void* const candidate) const noexcept
    {
        // The object's vtable must belong to the game image, and its RTTI type
        // must be a top-level quest script: ".?AV<Class>@NScript@@" with no
        // enclosing class (nested names are entity scripts).
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        __try
        {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            const std::uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
            const auto object = reinterpret_cast<std::uintptr_t>(candidate);
            if (object == 0 || (object >= base && object < end)) return false;
            const auto vtable = *reinterpret_cast<const std::uintptr_t*>(object);
            if (vtable < base || vtable >= end) return false;
            const auto locator = *reinterpret_cast<const std::uintptr_t*>(vtable - sizeof(void*));
            if (locator < base || locator >= end) return false;
            const auto typeDescriptor = *reinterpret_cast<const std::uintptr_t*>(locator + 12);
            if (typeDescriptor < base || typeDescriptor >= end) return false;
            const char* const name = reinterpret_cast<const char*>(typeDescriptor + 8);
            static constexpr char Prefix[] = ".?AVC";
            static constexpr char Suffix[] = "@NScript@@";
            constexpr std::size_t prefixLength = sizeof(Prefix) - 1;
            constexpr std::size_t suffixLength = sizeof(Suffix) - 1;
            if (std::strncmp(name, Prefix, prefixLength) != 0) return false;
            const std::size_t length = strnlen(name, 160);
            if (length <= prefixLength + suffixLength ||
                std::strcmp(name + length - suffixLength, Suffix) != 0)
            {
                return false;
            }
            for (std::size_t index = prefixLength; index < length - suffixLength; ++index)
            {
                if (name[index] == '@') return false;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool QuestFieldRecorder::SaveScript(void* const script) const noexcept
    {
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        const auto construct = reinterpret_cast<CharStringConstructor>(base + game::native::rva::CharStringConstructor);
        const auto destroy = reinterpret_cast<CharStringDestructor>(base + game::native::rva::CharStringDestructor);
        const auto persist = reinterpret_cast<ScriptPersistFn>(base + ScriptPersistRva);
        game::native::CharString scratch = {};
        bool constructed = false;
        bool saved = false;
        AcquireSRWLockExclusive(&observedLock_);
        observedCount_ = 0;
        ReleaseSRWLockExclusive(&observedLock_);
        recordingScript_.store(script, std::memory_order_release);
        __try
        {
            construct(&scratch, "", -1);
            constructed = true;
            persist(script, &scratch, 1);
            saved = true;
            const auto* const rep = static_cast<const game::native::StringRep*>(scratch.stringData);
            if (rep != nullptr) scratchBytes_ += static_cast<std::size_t>(rep->len > 0 ? rep->len : 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            saved = false;
        }
        recordingScript_.store(nullptr, std::memory_order_release);
        if (constructed)
        {
            __try { destroy(&scratch); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return saved;
    }

    bool QuestFieldRecorder::Record(std::vector<Field>& fields)
    {
        fields.clear();
        if (!IsInstalled() || gameModule_ == nullptr) return false;
        std::array<RunningQuest, MaximumRunningQuests> quests = {};
        const std::size_t questCount = CollectRunningQuests(quests.data(), quests.size());
        std::size_t scripted = 0;
        for (std::size_t index = 0; index < questCount; ++index)
        {
            if (!SaveScript(quests[index].script)) continue;
            ++scripted;
            for (std::size_t at = 0; at < observedCount_; ++at)
            {
                fields.push_back({quests[index].name, observed_[at].name,
                    observed_[at].kind, observed_[at].address});
            }
        }
        if (!reported_)
        {
            reported_ = true;
            char detail[160] = {};
            std::snprintf(detail, sizeof(detail),
                "running_quests=%zu scripts_saved=%zu fields=%zu window_transfers=%u first_delta=%ld scratch_bytes=%zu",
                questCount, scripted, fields.size(), windowTransfers_.load(),
                firstDelta_.load(), scratchBytes_);
            diagnostics_.Event("MultiplayerQuestFieldRecorderStats", detail);
        }
        return true;
    }

    bool QuestFieldRecorder::Read(const Field& field, std::uint32_t& bits) noexcept
    {
        bits = 0;
        __try
        {
            bits = field.kind == Kind::Bool
                ? (*static_cast<const std::uint8_t*>(field.address) != 0 ? 1u : 0u)
                : *static_cast<const std::uint32_t*>(field.address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool QuestFieldRecorder::Write(const Field& field, const std::uint32_t bits) noexcept
    {
        __try
        {
            if (field.kind == Kind::Bool)
            {
                *static_cast<std::uint8_t*>(field.address) = bits != 0 ? 1 : 0;
            }
            else
            {
                *static_cast<std::uint32_t*>(field.address) = bits;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}
