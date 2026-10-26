#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fable::multiplayer::persistence
{
    // Maps every persisted progress variable of the running quest scripts to
    // its live memory.
    //
    // Retail quest scripts persist progress as named fields through
    // CPersistContext::Transfer(name, &field, &default), but only when a
    // quest ends; SaveGameState never serializes running scripts. Recording
    // therefore walks CQuestManager's active quests, finds each quest's
    // NScript object by RTTI, and runs the retail script persist entry in save
    // mode into a scratch string while the Transfer primitives report field
    // addresses.
    class QuestFieldRecorder final
    {
    public:
        enum class Kind : std::uint8_t
        {
            Bool = 1,
            Int32 = 2,
            Float32 = 3,
        };

        struct Field final
        {
            std::string quest;
            std::string name;
            Kind kind = Kind::Int32;
            void* address = nullptr;
        };

        bool Install(HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept;
        void Shutdown() noexcept;
        [[nodiscard]] bool IsInstalled() const noexcept;

        // Game thread only. Replaces `fields` with the current mapping.
        bool Record(std::vector<Field>& fields);

        static bool Read(const Field& field, std::uint32_t& bits) noexcept;
        static bool Write(const Field& field, std::uint32_t bits) noexcept;

    private:
        struct RunningQuest final
        {
            char name[96] = {};
            void* script = nullptr;
        };

        struct ObservedField final
        {
            const char* name = nullptr;
            Kind kind = Kind::Int32;
            void* address = nullptr;
        };

        using ScriptPersistFn = void(__thiscall*)(void*, void*, int);
        using TransferFn = void(__thiscall*)(void*, const char*, void*, const void*);

        static void __fastcall TransferBoolObserved(void* context, void*, const char* name, void* field, const void* fallback);
        static void __fastcall TransferIntObserved(void* context, void*, const char* name, void* field, const void* fallback);
        static void __fastcall TransferFloatObserved(void* context, void*, const char* name, void* field, const void* fallback);
        static void Observe(Kind kind, const char* name, void* field) noexcept;

        // SEH-only helpers; they must not own C++ objects.
        std::size_t CollectRunningQuests(RunningQuest* quests, std::size_t capacity) const noexcept;
        bool IsQuestScriptObject(const void* candidate) const noexcept;
        bool SaveScript(void* script) const noexcept;

        static constexpr std::size_t MaximumRunningQuests = 256;
        static constexpr std::size_t MaximumObservedPerScript = 256;

        static QuestFieldRecorder* active_;
        // Set for the duration of one SaveScript call. Process-wide, because
        // retail may run a script's Persist on that script's own thread; the
        // address-span check rejects unrelated concurrent Transfers.
        static std::atomic<void*> recordingScript_;
        static SRWLOCK observedLock_;
        static std::size_t observedCount_;
        static std::atomic_uint32_t windowTransfers_;
        static std::atomic_long firstDelta_;
        static std::size_t scratchBytes_;
        static ObservedField observed_[MaximumObservedPerScript];

        HMODULE gameModule_ = nullptr;
        core::Diagnostics diagnostics_ = {};
        core::hooking::InlineHook transferBool_;
        core::hooking::InlineHook transferInt_;
        core::hooking::InlineHook transferFloat_;
        TransferFn originalBool_ = nullptr;
        TransferFn originalInt_ = nullptr;
        TransferFn originalFloat_ = nullptr;
        bool reported_ = false;
    };
}
