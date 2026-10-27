#include "KillExperienceShare.h"

#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Multiplayer/Replication/LocalHeroReplication.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include "Multiplayer/Transport/UdpPeer.h"

#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace
{
    fable::multiplayer::ReliableMessageSink* ResolveKillExperienceSink(
        fable::multiplayer::MultiplayerSessionContexts& contexts) noexcept
    {
        return &contexts.actions.killExperience;
    }

    // CTCHeroExperience::AddGeneralExperience(amount): sub esp,8; push esi;
    // mov esi,ecx; cmp byte ptr [esi+44h],0
    constexpr std::uintptr_t AddGeneralRva = 0x019BB310;
    constexpr std::uint8_t AddGeneralPrologue[] = {0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x44, 0x00};
    // Return address of the kill handler's call (after "call AddGeneral").
    constexpr std::uintptr_t KillGrantReturnRva = 0x01AC4606;
    constexpr std::int32_t HeroExperienceComponent = 0x68;
    // CTCHeroExperience stat experience: Get(index), Set(index, value);
    // 0 strength, 1 skill, 2 will.
    constexpr std::uintptr_t GetStatRva = 0x019B8CC0;
    constexpr std::uintptr_t SetStatRva = 0x019B8FE0;
    using GetStatFn = std::int32_t(__thiscall*)(void*, std::int32_t);
    using SetStatFn = void(__thiscall*)(void*, std::int32_t, std::int32_t);

    bool ReadStats(std::uintptr_t base, void* experience, std::int32_t (&values)[3]) noexcept
    {
        __try
        {
            for (int index = 0; index < 3; ++index)
            {
                values[index] = reinterpret_cast<GetStatFn>(base + GetStatRva)(experience, index);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool AddStat(std::uintptr_t base, void* experience, std::int32_t index, std::int32_t amount) noexcept
    {
        __try
        {
            const std::int32_t current = reinterpret_cast<GetStatFn>(base + GetStatRva)(experience, index);
            reinterpret_cast<SetStatFn>(base + SetStatRva)(experience, index, current + amount);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

FABLE_RELIABLE_SINK_DESCRIPTOR(
    g_fableReliableSinkKillExperience,
    0x1014u,
    "kill-experience",
    819u,
    "multiplayer-kill-experience-dispatch",
    ResolveKillExperienceSink);

namespace fable::multiplayer::combat
{
    KillExperienceShare* KillExperienceShare::active_ = nullptr;

    void KillExperienceShare::Initialize(
        const PeerRole role, UdpPeer& transport, replication::LocalHeroReplication& localHero,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        role_ = role;
        transport_ = &transport;
        localHero_ = &localHero;
        diagnostics_ = diagnostics;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        killReturn_ = base + KillGrantReturnRva;
        active_ = this;
        if (!add_.Install(reinterpret_cast<void*>(base + AddGeneralRva), AddGeneralPrologue, sizeof(AddGeneralPrologue),
                reinterpret_cast<void*>(&AddObserved), sizeof(AddGeneralPrologue)))
        {
            diagnostics_.Event("KillExperienceShareBlocked", "experience grant prologue did not match");
            active_ = nullptr;
            return;
        }
        original_ = reinterpret_cast<AddFn>(add_.Original());
        diagnostics_.Event("KillExperienceShareReady", "killer 1x, other players 0.3x");
    }

    void KillExperienceShare::Shutdown() noexcept
    {
        if (!add_.Shutdown()) return;
        original_ = nullptr;
        if (active_ == this) active_ = nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        localKills_.clear();
        inbound_.clear();
        outbound_.clear();
        transport_ = nullptr;
        localHero_ = nullptr;
    }

    void __fastcall KillExperienceShare::AddObserved(void* const experience, void* const edx, const std::int32_t amount)
    {
        (void)edx;
        KillExperienceShare* const self = active_;
        if (self == nullptr || self->original_ == nullptr) return;
        // The hook jumps here from the patched entry, so this is the
        // retail caller's return address.
        if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == self->killReturn_ && amount > 0)
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->localKills_.size() < QueueLimit) self->localKills_.push_back(amount);
        }
        self->original_(experience, amount);
    }

    void KillExperienceShare::TrackStatExperience(void* const experience)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::int32_t current[StatCount] = {};
        if (experience == nullptr || !ReadStats(base, experience, current))
        {
            trackedExperience_ = nullptr;
            return;
        }
        if (experience != trackedExperience_)
        {
            // New Hero or first frame: only start tracking.
            trackedExperience_ = experience;
            std::memcpy(statBaseline_, current, sizeof(statBaseline_));
            return;
        }
        Grant grant;
        bool earned = false;
        for (int index = 0; index < StatCount; ++index)
        {
            const std::int32_t delta = current[index] - statBaseline_[index];
            if (delta > 0 && delta < 1'000'000)
            {
                grant.stats[index] = delta;
                earned = true;
            }
        }
        std::memcpy(statBaseline_, current, sizeof(statBaseline_));
        if (earned && outbound_.size() < QueueLimit) outbound_.push_back(grant);
    }

    bool KillExperienceShare::Process()
    {
        if (transport_ == nullptr || original_ == nullptr) return true;
        if (transport_->HasFailed()) return false;
        std::deque<std::int32_t> kills;
        std::deque<Grant> inbound;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            kills.swap(localKills_);
            inbound.swap(inbound_);
        }
        for (const std::int32_t amount : kills)
        {
            Grant grant;
            grant.general = amount;
            if (outbound_.size() < QueueLimit) outbound_.push_back(grant);
        }
        void* const hero = localHero_ != nullptr ? localHero_->NativeHero() : nullptr;
        void* const experience = hero != nullptr
            ? game::entity::native::ThingComponentAccess::Find(
                hero, static_cast<game::entity::native::ThingComponentType>(HeroExperienceComponent))
            : nullptr;
        // Own earnings first, so shares applied below are not sent back.
        TrackStatExperience(experience);
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        for (const Grant& grant : inbound)
        {
            const auto share = [](std::int32_t amount)
            {
                return static_cast<std::int32_t>(std::lround(static_cast<double>(amount) * PeerShare));
            };
            Grant applied;
            if (experience != nullptr)
            {
                applied.general = share(grant.general);
                if (applied.general > 0) original_(experience, applied.general);
                for (int index = 0; index < StatCount; ++index)
                {
                    applied.stats[index] = share(grant.stats[index]);
                    if (applied.stats[index] > 0) AddStat(base, experience, index, applied.stats[index]);
                }
            }
            char detail[160] = {};
            std::snprintf(detail, sizeof(detail),
                "general=%d strength=%d skill=%d will=%d share=(%d,%d,%d,%d)",
                grant.general, grant.stats[0], grant.stats[1], grant.stats[2],
                applied.general, applied.stats[0], applied.stats[1], applied.stats[2]);
            diagnostics_.Event("KillExperienceShared", detail);
            // The host fans a guest's earnings out to the other guests.
            if (role_ == PeerRole::Host && outbound_.size() < QueueLimit) outbound_.push_back(grant);
        }
        // Re-baseline after shares so they are not mistaken for earnings.
        if (experience != nullptr && experience == trackedExperience_)
        {
            std::int32_t current[StatCount] = {};
            if (ReadStats(base, experience, current)) std::memcpy(statBaseline_, current, sizeof(statBaseline_));
        }
        while (!outbound_.empty())
        {
            const Grant& grant = outbound_.front();
            std::int32_t wire[1 + StatCount] = {grant.general, grant.stats[0], grant.stats[1], grant.stats[2]};
            if (!transport_->SubmitReliable(reliable_stream::Control, protocol::PacketType::KillExperience,
                    reinterpret_cast<const std::uint8_t*>(wire), sizeof(wire)))
            {
                return !transport_->HasFailed();
            }
            outbound_.pop_front();
        }
        return true;
    }

    bool KillExperienceShare::HandleReliableMessage(const TransportMessage& message)
    {
        if (message.type != protocol::PacketType::KillExperience) return false;
        std::int32_t wire[1 + StatCount] = {};
        if (message.payloadSize != sizeof(wire)) return true;
        std::memcpy(wire, message.payload.data(), sizeof(wire));
        Grant grant;
        grant.general = wire[0];
        bool any = grant.general > 0;
        for (int index = 0; index < StatCount; ++index)
        {
            grant.stats[index] = wire[1 + index];
            any = any || grant.stats[index] > 0;
        }
        for (const std::int32_t value : wire)
        {
            if (value < 0 || value > 1'000'000) return true;
        }
        if (!any) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (inbound_.size() < QueueLimit) inbound_.push_back(grant);
        return true;
    }
}
