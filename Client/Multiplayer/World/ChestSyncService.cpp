#include "ChestSyncService.h"

#include "Game/Entity/EntityService.h"
#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Game/Native/Addresses.h"
#include "Game/Native/ScriptTypes.h"
#include "Multiplayer/Entities/EntityNetworkIdentityRegistry.h"
#include "Multiplayer/Entities/EntityPresenceReplication.h"
#include "Multiplayer/Entities/LiveEntityRegistry.h"
#include "Multiplayer/Replication/LocalHeroReplication.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include "Multiplayer/Transport/UdpPeer.h"

#include <cstdio>
#include <cstring>

namespace
{
    fable::multiplayer::ReliableMessageSink* ResolveChestSink(
        fable::multiplayer::MultiplayerSessionContexts& contexts) noexcept
    {
        return &contexts.world.chestSync;
    }

    fable::multiplayer::ReliableMessageSink* ResolveDemonDoorSink(
        fable::multiplayer::MultiplayerSessionContexts& contexts) noexcept
    {
        return &contexts.world.chestSync.Doors();
    }

    // CTCContainerRewardHero::Reward (this = container component): collects
    // the contents and gives them to the local Hero. push -1; push <SEH>.
    constexpr std::uintptr_t RewardRva = 0x01960350;
    constexpr std::uintptr_t RewardHandlerRva = 0x0252B3E8;
    constexpr std::size_t RewardPrologueBytes = 7;
    // (this, vector<CCharString>* names, int* gold), ret 8.
    constexpr std::uintptr_t CollectContentsRva = 0x0195F9A0;
    constexpr std::uintptr_t DestroyNameVectorRva = 0x0139CDB0;
    // Hero inventory component 0xCB: (this, vector<CCharString>*, source, notify), ret 0xC.
    constexpr std::uintptr_t GiveItemsRva = 0x019C9020;
    constexpr std::int32_t HeroInventoryComponent = 0xCB;
    constexpr std::uintptr_t CharStringCopyRva = 0x012B7840;
    // Local Hero, as retail resolves it: players manager -> controlled Thing.
    constexpr std::uintptr_t WorldSingletonRva = 0x0322AE48;
    constexpr std::uintptr_t ResolvePlayerRva = 0x018FD7D0;
    constexpr std::uintptr_t ResolveHeroRva = 0x018E3AD0;
    // CTCChest (derives the reward container): +0x20 opened, +0x22 opening.
    constexpr std::uintptr_t ChestVtableRva = 0x02AF0824;
    constexpr std::size_t ChestOpenedOffset = 0x20;
    constexpr std::size_t ChestOpeningOffset = 0x22;
    constexpr std::size_t ChestClosingOffset = 0x23;
    constexpr std::size_t MaximumItems = 64;
    // CTCDoor::SetOpen(bool), this = door component (type 0x26).
    constexpr std::uintptr_t DoorSetOpenRva = 0x01981B60;
    constexpr std::uint8_t DoorSetOpenPrologue[] = {0x53, 0x8A, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1};
    constexpr std::int32_t DoorComponent = 0x26;
    constexpr std::size_t DoorOpenOffset = 0x10;

    struct NameVector final
    {
        fable::game::native::CharString* first = nullptr;
        fable::game::native::CharString* last = nullptr;
        fable::game::native::CharString* end = nullptr;
    };

    const char* NameOf(const fable::game::native::CharString& name) noexcept
    {
        const auto* rep = static_cast<const fable::game::native::StringRep*>(name.stringData);
        return rep != nullptr && rep->data != nullptr && rep->len > 0 ? rep->data : nullptr;
    }
}

FABLE_RELIABLE_SINK_DESCRIPTOR(
    g_fableReliableSinkChest,
    0x1013u,
    "chest",
    818u,
    "multiplayer-chest-dispatch",
    ResolveChestSink);

FABLE_RELIABLE_SINK_DESCRIPTOR(
    g_fableReliableSinkDemonDoor,
    0x1015u,
    "demon-door",
    820u,
    "multiplayer-demon-door-dispatch",
    ResolveDemonDoorSink);

namespace fable::multiplayer::world
{
    ChestSyncService* ChestSyncService::active_ = nullptr;
    thread_local bool ChestSyncService::replayingDoor_ = false;

    void ChestSyncService::Initialize(
        const PeerRole role,
        UdpPeer& transport,
        entities::EntityPresenceReplication& presence,
        entities::EntityNetworkIdentityRegistry& identities,
        replication::LocalHeroReplication& localHero,
        game::EntityService& entities,
        const core::Diagnostics& diagnostics) noexcept
    {
        Shutdown();
        entities_ = &entities;
        role_ = role;
        transport_ = &transport;
        presence_ = &presence;
        identities_ = &identities;
        localHero_ = &localHero;
        diagnostics_ = diagnostics;
        base_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::uint8_t prologue[RewardPrologueBytes] = {0x6A, 0xFF, 0x68};
        const auto handler = static_cast<std::uint32_t>(base_ + RewardHandlerRva);
        std::memcpy(prologue + 3, &handler, sizeof(handler));
        active_ = this;
        if (!reward_.Install(reinterpret_cast<void*>(base_ + RewardRva), prologue, sizeof(prologue),
                reinterpret_cast<void*>(&RewardObserved), sizeof(prologue)))
        {
            diagnostics_.Event("ChestSyncBlocked", "container reward prologue did not match");
            active_ = nullptr;
            return;
        }
        originalReward_ = reinterpret_cast<RewardFn>(reward_.Original());
        diagnostics_.Event("ChestSyncReady", "opened chests are shared; consumables go to every player");
        if (doorSetOpen_.Install(reinterpret_cast<void*>(base_ + DoorSetOpenRva), DoorSetOpenPrologue,
                sizeof(DoorSetOpenPrologue), reinterpret_cast<void*>(&DoorSetOpenObserved), sizeof(DoorSetOpenPrologue)))
        {
            originalDoorSetOpen_ = reinterpret_cast<SetDoorOpenFn>(doorSetOpen_.Original());
            diagnostics_.Event("DemonDoorSyncReady", "opened demon doors are shared");
        }
        else
        {
            diagnostics_.Event("DemonDoorSyncBlocked", "door SetOpen prologue did not match");
        }
    }

    void ChestSyncService::Shutdown() noexcept
    {
        if (doorSetOpen_.Shutdown()) originalDoorSetOpen_ = nullptr;
        if (!reward_.Shutdown()) return;
        originalReward_ = nullptr;
        if (active_ == this) active_ = nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        localOpened_.clear();
        localDoors_.clear();
        remoteOpening_.clear();
        inbound_.clear();
        outbound_.clear();
        transport_ = nullptr;
        presence_ = nullptr;
        identities_ = nullptr;
        localHero_ = nullptr;
    }

    void ChestSyncService::ResetForReload() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        localOpened_.clear(); localDoors_.clear(); remoteOpening_.clear(); inbound_.clear(); outbound_.clear();
    }

    bool __fastcall ChestSyncService::RewardObserved(void* const container, void* const edx)
    {
        (void)edx;
        ChestSyncService* const self = active_;
        if (self == nullptr || self->originalReward_ == nullptr) return false;
        bool remote = false;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            remote = self->remoteOpening_.erase(container) != 0;
            if (!remote && self->localOpened_.size() < QueueLimit) self->localOpened_.push_back(container);
        }
        if (remote) return self->RewardConsumables(container);
        return self->originalReward_(container);
    }

    void __fastcall ChestSyncService::DoorSetOpenObserved(void* const door, void* const edx, const std::uint8_t open)
    {
        (void)edx;
        ChestSyncService* const self = active_;
        if (self == nullptr || self->originalDoorSetOpen_ == nullptr) return;
        if (!replayingDoor_ && open != 0)
        {
            const bool wasClosed = static_cast<const std::uint8_t*>(door)[DoorOpenOffset] == 0;
            void* const owner = *reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(door) + 4);
            if (wasClosed && owner != nullptr)
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (self->localDoors_.size() < QueueLimit) self->localDoors_.push_back(owner);
            }
        }
        self->originalDoorSetOpen_(door, open);
    }

    bool ChestSyncService::IsDemonDoor(const void* const thing) const
    {
        const entities::LiveEntityRecord* const live = presence_->LiveEntities().FindByThing(thing);
        if (live == nullptr) return false;
        if (live->scriptName.find("DemonDoor") != std::string::npos) return true;
        std::string definition;
        return entities_ != nullptr && entities_->ResolveDefinitionName(live->definitionIndex, definition) &&
            definition.find("DEMON_DOOR") != std::string::npos;
    }

    bool ChestSyncService::OpenDoorLocally(const ChestOpen& message)
    {
        const PlayerState* const local = localHero_ != nullptr ? localHero_->CurrentState() : nullptr;
        if (local == nullptr || local->mapId != message.mapId || originalDoorSetOpen_ == nullptr) return true;
        std::uint64_t uid = identities_->FindLocal(message.thingUid);
        if (uid == 0) uid = message.thingUid;
        const entities::LiveEntityRecord* const live = presence_->LiveEntities().Find(uid);
        void* const door = live != nullptr && live->thing != nullptr
            ? game::entity::native::ThingComponentAccess::Find(
                live->thing, static_cast<game::entity::native::ThingComponentType>(DoorComponent))
            : nullptr;
        char detail[128] = {};
        std::snprintf(detail, sizeof(detail), "thing_uid=%016llX door=%d",
            static_cast<unsigned long long>(message.thingUid), door != nullptr ? 1 : 0);
        if (door != nullptr)
        {
            replayingDoor_ = true;
            originalDoorSetOpen_(door, 1);
            replayingDoor_ = false;
        }
        diagnostics_.Event("DemonDoorRemoteOpened", detail);
        return true;
    }

    bool ChestSyncService::IsSharedConsumable(const char* const name) noexcept
    {
        if (name == nullptr) return false;
        // Consumables only; anything else (weapons, clothing, augments,
        // keys, books, trophies) belongs to whoever opened the chest.
        static constexpr const char* Tokens[] = {
            "POTION", "PHIAL", "ELIXIR", "FOOD", "APPLE", "PIE", "MEAT", "CHICKEN", "FISH",
            "CARROT", "CHEESE", "BEER", "WINE", "ALE", "MOONSHINE", "BERR", "FRUIT", "CABBAGE",
            "CAKE", "CRUNCHY", "BREAD", "SANDWICH", "TOFU", "MUTTON", "DRUMSTICK", "PUMPKIN"};
        if (std::strstr(name, "DUMMY") != nullptr) return false;
        for (const char* token : Tokens)
        {
            if (std::strstr(name, token) != nullptr) return true;
        }
        return false;
    }

    bool ChestSyncService::RewardConsumables(void* const container) noexcept
    {
        using CollectFn = void(__thiscall*)(void*, NameVector*, std::int32_t*);
        using DestroyVectorFn = void(__thiscall*)(NameVector*);
        using GiveFn = void(__thiscall*)(void*, NameVector*, void*, std::uint8_t);
        using CopyFn = void(__thiscall*)(fable::game::native::CharString*, const fable::game::native::CharString*);
        using DestroyFn = void(__thiscall*)(fable::game::native::CharString*);
        using GetFn = void*(__thiscall*)(void*);
        NameVector contents;
        std::int32_t gold = 0;
        fable::game::native::CharString shared[MaximumItems] = {};
        std::size_t sharedCount = 0;
        std::size_t totalCount = 0;
        bool given = false;
        __try
        {
            reinterpret_cast<CollectFn>(base_ + CollectContentsRva)(container, &contents, &gold);
            for (auto* item = contents.first; item != nullptr && item != contents.last; ++item)
            {
                ++totalCount;
                if (sharedCount < MaximumItems && IsSharedConsumable(NameOf(*item)))
                {
                    reinterpret_cast<CopyFn>(base_ + CharStringCopyRva)(&shared[sharedCount], item);
                    ++sharedCount;
                }
            }
            reinterpret_cast<DestroyVectorFn>(base_ + DestroyNameVectorRva)(&contents);
            if (sharedCount != 0)
            {
                auto* const world = *reinterpret_cast<std::uint8_t* const*>(base_ + WorldSingletonRva);
                void* const player = world == nullptr ? nullptr
                    : reinterpret_cast<GetFn>(base_ + ResolvePlayerRva)(*reinterpret_cast<void* const*>(world + 0x24));
                void* const hero = player == nullptr ? nullptr : reinterpret_cast<GetFn>(base_ + ResolveHeroRva)(player);
                void* const inventory = hero == nullptr ? nullptr
                    : game::entity::native::ThingComponentAccess::Find(
                        hero, static_cast<game::entity::native::ThingComponentType>(HeroInventoryComponent));
                if (inventory != nullptr)
                {
                    NameVector filtered{shared, shared + sharedCount, shared + sharedCount};
                    void* const source = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(container) + 4);
                    reinterpret_cast<GiveFn>(base_ + GiveItemsRva)(inventory, &filtered, source, 1);
                    given = true;
                }
            }
            for (std::size_t index = 0; index < sharedCount; ++index)
            {
                reinterpret_cast<DestroyFn>(base_ + fable::game::native::rva::CharStringDestructor)(&shared[index]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            given = false;
        }
        char detail[160] = {};
        std::snprintf(detail, sizeof(detail), "items=%zu consumables_given=%zu gold_kept_by_opener=%d given=%d",
            totalCount, sharedCount, gold, given ? 1 : 0);
        diagnostics_.Event("ChestRemoteRewarded", detail);
        return given;
    }

    // Starts the lid animation; retail calls Reward when it completes.
    static bool StartOpening(std::uint8_t* const chest) noexcept
    {
        __try
        {
            if (chest[ChestOpenedOffset] != 0 || chest[ChestOpeningOffset] != 0) return false;
            chest[ChestClosingOffset] = 0;
            chest[ChestOpeningOffset] = 1;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ChestSyncService::OpenLocally(const ChestOpen& message)
    {
        if (presence_ == nullptr || identities_ == nullptr || localHero_ == nullptr) return true;
        const PlayerState* const local = localHero_->CurrentState();
        // A player elsewhere opens it when the map's saved state reaches them.
        if (local == nullptr || local->mapId != message.mapId) return true;
        std::uint64_t uid = identities_->FindLocal(message.thingUid);
        if (uid == 0) uid = message.thingUid;
        const entities::LiveEntityRecord* const live = presence_->LiveEntities().Find(uid);
        if (live == nullptr || live->thing == nullptr) return true;
        auto* const chest = static_cast<std::uint8_t*>(game::entity::native::ThingComponentAccess::FindByVtableRva(
            live->thing, reinterpret_cast<HMODULE>(base_), ChestVtableRva));
        char detail[160] = {};
        if (chest == nullptr)
        {
            std::snprintf(detail, sizeof(detail), "thing_uid=%016llX reason=not-a-chest",
                static_cast<unsigned long long>(message.thingUid));
            diagnostics_.Event("ChestRemoteOpenSkipped", detail);
            return true;
        }
        const bool started = StartOpening(chest);
        if (started)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remoteOpening_.insert(chest);
        }
        std::snprintf(detail, sizeof(detail), "thing_uid=%016llX started=%d",
            static_cast<unsigned long long>(message.thingUid), started ? 1 : 0);
        diagnostics_.Event("ChestRemoteOpened", detail);
        return true;
    }

    bool ChestSyncService::Send(const ChestOpen& message)
    {
        std::uint8_t bytes[MessageBytes] = {};
        std::memcpy(bytes, &message.revision, 8);
        std::memcpy(bytes + 8, &message.thingUid, 8);
        std::memcpy(bytes + 16, &message.mapId, 2);
        return transport_ != nullptr && transport_->SubmitReliable(
            reliable_stream::Control,
            message.door ? protocol::PacketType::DemonDoorOpen : protocol::PacketType::ChestOpen,
            bytes, sizeof(bytes));
    }

    static void* ContainerOwner(void* const container) noexcept
    {
        __try
        {
            return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(container) + 4);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ChestSyncService::Process()
    {
        if (transport_ == nullptr || originalReward_ == nullptr) return true;
        if (transport_->HasFailed()) return false;
        std::deque<void*> opened;
        std::deque<void*> doors;
        std::deque<ChestOpen> inbound;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            opened.swap(localOpened_);
            doors.swap(localDoors_);
            inbound.swap(inbound_);
        }
        const PlayerState* const local = localHero_ != nullptr ? localHero_->CurrentState() : nullptr;
        for (void* const container : opened)
        {
            void* const thing = ContainerOwner(container);
            const entities::LiveEntityRecord* const live = thing != nullptr
                ? presence_->LiveEntities().FindByThing(thing) : nullptr;
            if (live == nullptr || local == nullptr) continue;
            ChestOpen message;
            message.revision = ++nextRevision_;
            message.thingUid = identities_->CanonicalizeLocalObservation(live->thingUid);
            message.mapId = local->mapId;
            outbound_.push_back(message);
            char detail[128] = {};
            std::snprintf(detail, sizeof(detail), "thing_uid=%016llX map_id=%u",
                static_cast<unsigned long long>(message.thingUid), static_cast<unsigned>(message.mapId));
            diagnostics_.Event("ChestLocalOpened", detail);
        }
        for (void* const thing : doors)
        {
            const entities::LiveEntityRecord* const live = presence_->LiveEntities().FindByThing(thing);
            if (live == nullptr || local == nullptr || !IsDemonDoor(thing)) continue;
            ChestOpen message;
            message.revision = ++nextRevision_;
            message.thingUid = identities_->CanonicalizeLocalObservation(live->thingUid);
            message.mapId = local->mapId;
            message.door = true;
            outbound_.push_back(message);
            char detail[128] = {};
            std::snprintf(detail, sizeof(detail), "thing_uid=%016llX map_id=%u",
                static_cast<unsigned long long>(message.thingUid), static_cast<unsigned>(message.mapId));
            diagnostics_.Event("DemonDoorLocalOpened", detail);
        }
        for (const ChestOpen& message : inbound)
        {
            if (message.door) OpenDoorLocally(message);
            else OpenLocally(message);
            // The host fans a guest's chest out to the other guests.
            if (role_ == PeerRole::Host && outbound_.size() < QueueLimit) outbound_.push_back(message);
        }
        while (!outbound_.empty())
        {
            if (!Send(outbound_.front())) return !transport_->HasFailed();
            outbound_.pop_front();
        }
        return true;
    }

    bool ChestSyncService::HandleReliableMessage(const TransportMessage& transportMessage)
    {
        if (transportMessage.type != protocol::PacketType::ChestOpen &&
            transportMessage.type != protocol::PacketType::DemonDoorOpen)
        {
            return false;
        }
        if (transportMessage.payloadSize != MessageBytes) return true;
        ChestOpen message;
        std::memcpy(&message.revision, transportMessage.payload.data(), 8);
        std::memcpy(&message.thingUid, transportMessage.payload.data() + 8, 8);
        std::memcpy(&message.mapId, transportMessage.payload.data() + 16, 2);
        message.door = transportMessage.type == protocol::PacketType::DemonDoorOpen;
        if (message.thingUid == 0 || message.mapId == 0) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (inbound_.size() < QueueLimit) inbound_.push_back(message);
        return true;
    }
}
