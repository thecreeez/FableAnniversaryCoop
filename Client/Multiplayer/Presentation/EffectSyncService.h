#pragma once
#include "Core/Diagnostics/Diagnostics.h"
#include "Core/Hooking/CodePatch.h"
#include "Game/Native/ScriptTypes.h"
#include "Multiplayer/Protocol/EffectMessage.h"
#include "Multiplayer/Protocol/PlayerState.h"
#include "Multiplayer/Transport/ReliableMessageDispatcher.h"
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace fable::multiplayer
{
    struct MultiplayerSessionContexts;
    namespace presentation
    {
        // Replicates visual emitters, never damage, buffs or inventory effects.
        class EffectSyncService final : public ReliableMessageSink
        {
        public:
            ReliableMessageTypeSet HandledPacketTypes() const noexcept override
            { static constexpr protocol::PacketType t[] = {protocol::PacketType::Effect}; return {t, 1}; }
            bool Initialize(PeerRole, MultiplayerSessionContexts&, const core::Diagnostics&) noexcept;
            void Shutdown() noexcept;
            void ResetForReload() noexcept;
            bool Process();
            bool HandleReliableMessage(const TransportMessage&) override;
        private:
            using Wire = protocol::EffectMessage;
            using SpawnFn = void*(__cdecl*)(unsigned int, const game::Vector3*, unsigned char);
            using MaterialSpawnFn = void*(__cdecl*)(unsigned int, unsigned int, float, const game::Vector3*, unsigned char);
            using AttachFn = void(__thiscall*)(void*, void*, unsigned int, const game::native::CharString*, int, float);
            using StopFn = void(__thiscall*)(void*);
            using HitFn = void(__thiscall*)(void*, void*);
            struct NativeRef final
            {
                void* value[2] = {};
                NativeRef();
                ~NativeRef();
                NativeRef(const NativeRef&) = delete;
                NativeRef& operator=(const NativeRef&) = delete;
                void Set(void*) noexcept;
                void* Get() noexcept;
            };
            struct Binding { Wire identity; bool local = false; };
            struct Record final { Wire state; NativeRef emitter; std::uint64_t receivedAt = 0; bool borrowed = false; };
            struct Key
            {
                std::uint64_t actor = 0, instance = 0;
                std::uint32_t epoch = 0, generation = 0, mapEpoch = 0;
                bool operator==(const Key& b) const noexcept
                { return actor == b.actor && instance == b.instance && epoch == b.epoch && generation == b.generation && mapEpoch == b.mapEpoch; }
            };
            struct Hash { std::size_t operator()(const Key& k) const noexcept { return static_cast<std::size_t>(k.actor ^ (k.instance + (k.actor << 6)) ^ k.epoch ^ (k.generation << 8) ^ k.mapEpoch); } };
            static EffectSyncService* active_;
            static thread_local bool replaying_;
            static thread_local bool impactScope_;
            static void* __cdecl Spawn(unsigned int, const game::Vector3*, unsigned char);
            static void* __cdecl MaterialSpawn(unsigned int, unsigned int, float, const game::Vector3*, unsigned char);
            static void __fastcall Attach(void*, void*, void*, unsigned int, const game::native::CharString*, int, float);
            static void __fastcall Stop(void*, void*);
            static void __fastcall Destroy(void*, void*);
            static void __fastcall ObjectHit(void*, void*, void*);
            void ObserveAttach(void*, void*, unsigned int, const game::native::CharString*, int, float);
            void ObserveStop(void*);
            void ObserveImpact(void*, unsigned int, const game::Vector3&, unsigned int kind = 0, unsigned int secondary = 0, float strength = 0);
            bool MakeEmitter(Record&, void* target) noexcept;
            void DestroyEmitter(Record&) noexcept;
            void RefreshBindings();
            void CaptureExistingEmitters();
            bool IsCurrent(const Wire&) const;
            bool Send(const Wire&);
            bool SameVisual(const Wire&, const Wire&) const noexcept;
            core::hooking::InlineHook spawn_, materialSpawn_, attach_, stop_, destroy_, objectHit_;
            SpawnFn originalSpawn_ = nullptr;
            MaterialSpawnFn originalMaterialSpawn_ = nullptr;
            AttachFn originalAttach_ = nullptr;
            StopFn originalStop_ = nullptr, originalDestroy_ = nullptr;
            HitFn originalHit_ = nullptr;
            MultiplayerSessionContexts* contexts_ = nullptr;
            core::Diagnostics diagnostics_;
            PeerRole role_ = PeerRole::Guest;
            std::mutex mutex_;
            std::unordered_map<void*, Binding> bindings_;
            std::unordered_map<void*, std::unique_ptr<Record>> sources_, observed_;
            std::unordered_map<void*, Wire> impactCandidates_;
            std::unordered_map<Key, std::unique_ptr<Record>, Hash> replicas_;
            std::unordered_map<Key, Wire, Hash> desired_;
            std::unordered_map<Key, std::uint64_t, Hash> seen_;
            std::deque<Wire> local_, inbound_, outbound_;
            std::uint64_t instance_ = 0, revision_ = 0, peerRevision_ = 0, nextBaseline_ = 0;
            std::uint64_t nextCapture_ = 0;
            std::uintptr_t base_ = 0;
        };
    }
}
