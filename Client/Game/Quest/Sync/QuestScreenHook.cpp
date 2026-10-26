#include "QuestScreenHook.h"

#include "Game/Native/Addresses.h"
#include "Game/Native/ScriptTypes.h"

#include <cstdio>
#include <cstring>

namespace
{
    // Retail slot 325 implementation, checked before patching.
    constexpr std::uintptr_t ShowQuestCardRva = 0x0189B2E0;

    bool ReadRaw(void* const name, char (&buffer)[128]) noexcept
    {
        __try
        {
            const auto* string = static_cast<const fable::game::native::CharString*>(name);
            const auto* rep = string == nullptr ? nullptr
                : static_cast<const fable::game::native::StringRep*>(string->stringData);
            if (rep == nullptr || rep->data == nullptr || rep->len <= 0 ||
                static_cast<std::size_t>(rep->len) >= sizeof(buffer))
            {
                return false;
            }
            std::memcpy(buffer, rep->data, static_cast<std::size_t>(rep->len));
            buffer[rep->len] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadName(void* const name, std::string& value) noexcept
    {
        char buffer[128] = {};
        if (!ReadRaw(name, buffer)) return false;
        try
        {
            value = buffer;
        }
        catch (...)
        {
            return false;
        }
        return true;
    }
}

namespace fable::multiplayer::persistence
{
    QuestScreenHook* QuestScreenHook::active_ = nullptr;
    thread_local bool QuestScreenHook::replaying_ = false;

    bool QuestScreenHook::Install(const HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept
    {
        if (gameModule == nullptr || entry_ != nullptr || active_ != nullptr) return false;
        diagnostics_ = diagnostics;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        auto** const vtable = reinterpret_cast<void**>(base + fable::game::native::rva::GameScriptInterfaceVtable);
        void** const entry = vtable + Slot;
        if (*entry != reinterpret_cast<void*>(base + ShowQuestCardRva))
        {
            diagnostics_.Event("QuestScreenHookBlocked", "slot 325 does not hold the validated quest card function");
            return false;
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
        original_ = reinterpret_cast<ShowFn>(*entry);
        gameModule_ = gameModule;
        active_ = this;
        *entry = reinterpret_cast<void*>(&Shown);
        DWORD ignored = 0;
        VirtualProtect(entry, sizeof(void*), oldProtect, &ignored);
        entry_ = entry;
        diagnostics_.Event("QuestScreenHookReady", "quest card screens are observed");
        return true;
    }

    void QuestScreenHook::Shutdown() noexcept
    {
        if (entry_ != nullptr)
        {
            DWORD oldProtect = 0;
            if (VirtualProtect(entry_, sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                if (*entry_ == reinterpret_cast<void*>(&Shown)) *entry_ = reinterpret_cast<void*>(original_);
                DWORD ignored = 0;
                VirtualProtect(entry_, sizeof(void*), oldProtect, &ignored);
            }
        }
        entry_ = nullptr;
        if (active_ == this) active_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            observations_.clear();
        }
    }

    void __fastcall QuestScreenHook::Shown(
        void* const self, void* const edx, void* const name, const std::uint8_t mode, const std::uint8_t startQuest)
    {
        (void)edx;
        QuestScreenHook* const hook = active_;
        if (hook == nullptr || hook->original_ == nullptr) return;
        if (!replaying_)
        {
            Observation observation;
            if (ReadName(name, observation.name))
            {
                observation.mode = mode != 0;
                try
                {
                    std::lock_guard<std::mutex> lock(hook->mutex_);
                    if (hook->observations_.size() < ObservationLimit) hook->observations_.push_back(std::move(observation));
                }
                catch (...)
                {
                }
            }
        }
        hook->original_(self, name, mode, startQuest);
    }

    bool QuestScreenHook::PopObservation(Observation& observation)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (observations_.empty()) return false;
        observation = std::move(observations_.front());
        observations_.pop_front();
        return true;
    }

    bool QuestScreenHook::Replay(void* const scriptInterface, const char* const questName, const bool mode) noexcept
    {
        if (original_ == nullptr || scriptInterface == nullptr || gameModule_ == nullptr ||
            questName == nullptr || questName[0] == '\0')
        {
            return false;
        }
        using CharStringConstructor = void(__thiscall*)(fable::game::native::CharString*, const char*, int);
        using CharStringDestructor = void(__thiscall*)(fable::game::native::CharString*);
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule_);
        fable::game::native::CharString name = {};
        bool built = false;
        bool shown = false;
        replaying_ = true;
        __try
        {
            reinterpret_cast<CharStringConstructor>(base + fable::game::native::rva::CharStringConstructor)(&name, questName, -1);
            built = true;
            original_(scriptInterface, &name, mode ? 1 : 0, 0);
            shown = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            shown = false;
        }
        replaying_ = false;
        if (built)
        {
            __try
            {
                reinterpret_cast<CharStringDestructor>(base + fable::game::native::rva::CharStringDestructor)(&name);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return shown;
    }
}
