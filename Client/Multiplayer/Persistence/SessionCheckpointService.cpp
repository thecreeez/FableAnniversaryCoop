#include "SessionCheckpointService.h"
#include "Game/Entity/EntityService.h"
#include "Game/Entity/Entity.h"
#include "Game/Entity/Native/ThingComponentAccess.h"
#include "Game/Native/Addresses.h"
#include "Game/Persistence/CheckpointLoadScope.h"
#include "Game/Persistence/Native/AutoSaveFunction.h"
#include "Multiplayer/Runtime/MultiplayerSessionContexts.h"
#include "Multiplayer/Transport/ReliableSinkDescriptorRegistry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <intrin.h>

namespace
{
    using Wide = fable::game::native::WideString;
    using ConstructWide = Wide*(__thiscall*)(Wide*, const wchar_t*);
    using DestroyWide = void(__thiscall*)(Wide*);
    bool ReadWide(const Wide* value, const wchar_t*& data, std::size_t& length) noexcept
    {
        __try
        {
            const auto* rep = value ? static_cast<const unsigned char*>(value->stringData) : nullptr;
            if (!rep) return false;
            length = *reinterpret_cast<const std::uint32_t*>(rep + 0x10);
            if (length > 32767) return false;
            data = *reinterpret_cast<const std::uint32_t*>(rep + 0x14) >= 8 ?
                *reinterpret_cast<const wchar_t* const*>(rep) : reinterpret_cast<const wchar_t*>(rep);
            return data && data[length] == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void* World(const std::uintptr_t base) noexcept
    {
        __try
        {
            const auto manager = *reinterpret_cast<std::uintptr_t*>(base + 0x0322F1B0);
            return manager ? *reinterpret_cast<void**>(manager + 0x30) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool FileInfo(const wchar_t* path, std::uint64_t& size, std::uint64_t& stamp) noexcept
    {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info) || info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
        size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        stamp = (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
        return size > 0;
    }
    fable::multiplayer::ReliableMessageSink* ResolveCheckpoint(fable::multiplayer::MultiplayerSessionContexts& c) noexcept
    { return &c.world.checkpoints; }
}
FABLE_RELIABLE_SINK_DESCRIPTOR(g_fableReliableSinkCheckpoint, 0x1022u, "session-checkpoint", 826u,
    "multiplayer-checkpoint-dispatch", ResolveCheckpoint);

namespace fable::multiplayer::persistence
{
    SessionCheckpointService* SessionCheckpointService::active_ = nullptr;
    thread_local bool SessionCheckpointService::projecting_ = false;

    bool SessionCheckpointService::Initialize(const PeerRole role, MultiplayerSessionContexts& contexts,
        game::EntityService& entities, const std::uint64_t epoch, const core::Diagnostics& diagnostics) noexcept
    {
        if (active_) return false;
        base_ = reinterpret_cast<std::uintptr_t>(entities.GameModule());
        role_ = role; contexts_ = &contexts; entities_ = &entities; epoch_ = epoch; diagnostics_ = diagnostics;
        deathPrompt_.Bind(entities.GameModule()); waitPrompt_.Bind(entities.GameModule());
        active_ = this;
        // All retail save paths converge on CWorld's complete serializer, RVA01BA32D0.
        unsigned char savePrefix[7] = {0x6A, 0xFF, 0x68}; const auto handler = base_ + 0x0255B240;
        std::memcpy(savePrefix + 3, &handler, 4);
        const unsigned char selectPrefix[] = {0x8B, 0x49, 0x04, 0x85, 0xC9};
        if (!save_.Install(reinterpret_cast<void*>(base_ + 0x01BA32D0), savePrefix, 7,
                reinterpret_cast<void*>(&SaveObserved), 7) ||
            !select_.Install(reinterpret_cast<void*>(base_ + 0x016D7470), selectPrefix, sizeof(selectPrefix),
                reinterpret_cast<void*>(&LoadSelected), sizeof(selectPrefix)))
        { diagnostics_.Event("MultiplayerCheckpointHookBlocked", "save/load selector bytes mismatch"); Shutdown(); return false; }
        originalSave_ = reinterpret_cast<SaveFn>(save_.Original());
        originalSelect_ = reinterpret_cast<SelectFn>(select_.Original());
        unsigned char loadPrefix[7] = {0x6A, 0xFF, 0x68};
        const auto loadHandler = base_ + 0x0255BC32;
        std::memcpy(loadPrefix + 3, &loadHandler, 4);
        if (!load_.Install(reinterpret_cast<void*>(base_ + 0x01BAA3F0), loadPrefix, 7,
            reinterpret_cast<void*>(&WorldLoadRequested), 7))
        { diagnostics_.Event("MultiplayerCheckpointHookBlocked", "CWorld load-request bytes mismatch"); Shutdown(); return false; }
        originalLoad_ = reinterpret_cast<LoadFn>(load_.Original());
        unsigned char buttonPrefix[7] = {0x6A, 0xFF, 0x68};
        const auto buttonHandler = base_ + 0x023FAE18;
        std::memcpy(buttonPrefix + 3, &buttonHandler, 4);
        if (!loadButton_.Install(reinterpret_cast<void*>(base_ + 0x00ACEC50), buttonPrefix, 7,
            reinterpret_cast<void*>(&SetLoadButtonEnabled), 7))
        { diagnostics_.Event("MultiplayerCheckpointHookBlocked", "load-menu enabled-state setter bytes mismatch"); Shutdown(); return false; }
        originalButton_ = reinterpret_cast<ButtonFn>(loadButton_.Original());
        // Scoped override of the SAVE header's CurrentRegionName only. PLAYER stays local.
        unsigned char expected[5] = {0xE8}, replacement[5] = {0xE8};
        const auto oldDisplacement = static_cast<std::int32_t>(0x0137B6B0 - (0x01BA1CB2 + 5));
        const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&TransferRegion) - (base_ + 0x01BA1CB2 + 5));
        std::memcpy(expected + 1, &oldDisplacement, 4); std::memcpy(replacement + 1, &displacement, 4);
        if (!regionCall_.Install(reinterpret_cast<void*>(base_ + 0x01BA1CB2), expected, 5, replacement, 5))
        { diagnostics_.Event("MultiplayerCheckpointHookBlocked", "world region-save call mismatch"); Shutdown(); return false; }
        diagnostics_.Event("MultiplayerCheckpointReady", "paired native Hero saves; committed checkpoint and reload barriers");
        if (!bundleHook_.Install(entities.GameModule(), &BundleLoaded, this, diagnostics_))
        { diagnostics_.Event("MultiplayerCheckpointHookBlocked", "native load completion observer unavailable"); Shutdown(); return false; }
        return true;
    }

    void SessionCheckpointService::BundleLoaded(void* context) noexcept
    {
        auto* self = static_cast<SessionCheckpointService*>(context);
        if (self) self->worldEntered_.store(true, std::memory_order_release);
        if (self && self->loading_) self->bundleLoaded_.store(true, std::memory_order_release);
    }

    void SessionCheckpointService::QueueSourceSave(const wchar_t* name) noexcept
    {
        if (role_ != PeerRole::Host || projecting_ || loading_) return;
        if (std::wcsstr(name, L"QuestCheckPoint")) sourceSave_.store(2, std::memory_order_release);
        else if (std::wcsstr(name, L"AutoSave") || std::wcsstr(name, L"Manual"))
        { unsigned int empty = 0; sourceSave_.compare_exchange_strong(empty, 1, std::memory_order_release); }
    }
    void __fastcall SessionCheckpointService::SaveObserved(void* world, void*, const Wide* name,
        unsigned char a, std::uint32_t b, std::uint32_t c)
    {
        auto* self = active_;
        if (!self || !self->originalSave_) return;
        // Immutable checkpoint slots cannot be overwritten by autosaves during load.
        if (self->loading_ && !projecting_) return;
        wchar_t label[128] = {};
        const wchar_t* data = nullptr; std::size_t length = 0;
        if (ReadWide(name, data, length) && length < 128) std::wmemcpy(label, data, length);
        self->originalSave_(world, name, a, b, c);
        self->QueueSourceSave(label);
    }
    void __fastcall SessionCheckpointService::LoadSelected(void* selector, void*, const int slot)
    {
        auto* self = active_;
        if (!self || !self->originalSelect_) return;
        const bool dead = self->contexts_->actions.playerDeath.IsTerminalDead();
        // Initial front-end save selection still runs normally. Once a co-op
        // Hero is bound, every host checkpoint selection must use the barrier,
        // including a manual selection made before terminal death.
        if (!self->loading_ && self->role_ == PeerRole::Guest && self->worldEntered_.load(std::memory_order_acquire))
        {
            self->diagnostics_.Event("MultiplayerGuestLoadBlocked", "only the host may select a session save; initial profile load already completed");
            return;
        }
        if (!self->loading_ && self->contexts_->transport.transport.HasPeer() &&
            (dead || self->contexts_->players.localHero.IsWorldReady()))
        {
            if (self->role_ == PeerRole::Guest) return;
            if (slot == 16 || slot == 17)
            {
                self->requestedLoad_.store(slot, std::memory_order_release);
                self->diagnostics_.Event("MultiplayerHostCheckpointReloadRequested", "native menu selected checkpoint reload; await matching guest saves");
                return;
            }
        }
        self->originalSelect_(selector, slot);
    }
    bool __fastcall SessionCheckpointService::WorldLoadRequested(void* world, void*, const Wide* name,
        const game::native::CharString* region, unsigned char immediate)
    {
        auto* self = active_;
        if (!self || !self->originalLoad_) return false;
        if (!self->loading_ && self->role_ == PeerRole::Guest && self->worldEntered_.load(std::memory_order_acquire))
        {
            self->diagnostics_.Event("MultiplayerGuestLoadBlocked", "native world-load request rejected; waiting for host-authorized checkpoint barrier");
            return false;
        }
        // A live-world load first enters CWorld+104's native loading state machine.
        // Front-end immediate loads (CMainGameComponent::vf8) must still run normally.
        if (!immediate && !self->loading_ && self->contexts_->transport.transport.HasPeer() &&
            (self->contexts_->players.localHero.IsWorldReady() || self->contexts_->actions.playerDeath.IsTerminalDead()))
        {
            if (self->role_ == PeerRole::Guest) return true;
            const wchar_t* data = nullptr; std::size_t length = 0;
            wchar_t label[128] = {};
            if (!ReadWide(name, data, length) || length >= 128) return false;
            std::wmemcpy(label, data, length);
            self->requestedLoad_.store(std::wcsstr(label, L"Quest") ? 17 : 16, std::memory_order_release);
            self->diagnostics_.Event("MultiplayerHostCheckpointReloadRequested", "native CWorld live-world load deferred until paired checkpoint barrier");
            return true;
        }
        return self->originalLoad_(world, name, region, immediate);
    }

    void __fastcall SessionCheckpointService::SetLoadButtonEnabled(void* array, void*, unsigned int index, int enabled)
    {
        auto* self = active_;
        if (!self || !self->originalButton_) return;
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - self->base_;
        // Retail load-page selection/deletion refreshes set enabled[0]. Restrict
        // this shared GFx array helper to those callers; save/settings menus keep
        // their own enabled states. Server-driven loads bypass UI selection.
        const bool loadPage = (caller >= 0x01BF4D70 && caller < 0x01BF4F60) ||
            (caller >= 0x01BF6210 && caller < 0x01BF6444);
        if (loadPage && index == 0 && self->role_ == PeerRole::Guest && self->worldEntered_.load(std::memory_order_acquire)) enabled = 0;
        self->originalButton_(array, index, enabled);
    }

    bool SessionCheckpointService::QueueNativeLoad(const Saved& saved, bool observe) noexcept
    {
        void* world = World(base_);
        if (!world || !originalLoad_) return false;
        Wide name;
        game::native::CharString region;
        bool result = false;
        __try
        {
            reinterpret_cast<ConstructWide>(base_ + 0x0134F580)(&name, saved.name.c_str());
            reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(base_ + 0x012B7800)(&region, saved.ticket.region, -1);
            const auto load = observe ? reinterpret_cast<LoadFn>(base_ + 0x01BAA3F0) : originalLoad_;
            result = load(world, &name, &region, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { result = false; }
        if (region.stringData) reinterpret_cast<void(__thiscall*)(void*)>(base_ + 0x012B75D0)(&region);
        if (name.stringData) reinterpret_cast<DestroyWide>(base_ + 0x0134EFD0)(&name);
        return result;
    }
    void __fastcall SessionCheckpointService::TransferRegion(void* writer, void*, const char* name,
        game::native::CharString* value, game::native::CharString* fallback)
    {
        auto* self = active_;
        const auto base = self ? self->base_ : reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (self && projecting_)
        {
            game::native::CharString region;
            reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(base + 0x012B7800)(&region, self->pending_.ticket.region, -1);
            reinterpret_cast<void(__thiscall*)(void*, const void*)>(base + 0x012B7B30)(value, &region);
            reinterpret_cast<void(__thiscall*)(void*)>(base + 0x012B75D0)(&region);
        }
        reinterpret_cast<void(__thiscall*)(void*, const char*, void*, void*)>(base + 0x0137B6B0)(writer, name, value, fallback);
    }

    bool SessionCheckpointService::ResolvePath(Saved& saved) noexcept
    {
        const wchar_t* name = saved.ticket.kind ? (saved.ticket.slot ? L"CoopCheckpoint_Quest_B" : L"CoopCheckpoint_Quest_A") :
            (saved.ticket.slot ? L"CoopCheckpoint_Auto_B" : L"CoopCheckpoint_Auto_A");
        saved.name = name;
        Wide slot, path;
        bool okay = false;
        __try
        {
            reinterpret_cast<ConstructWide>(base_ + 0x0134F580)(&slot, name);
            void* profile = reinterpret_cast<void*(__cdecl*)()>(base_ + 0x01B9B3C0)();
            using GetFullPath = Wide*(__thiscall*)(void*, Wide*, const Wide*, unsigned char);
            reinterpret_cast<GetFullPath>(base_ + 0x01B940F0)(profile, &path, &slot, 0);
            const wchar_t* data = nullptr; std::size_t length = 0;
            if (ReadWide(&path, data, length) && length > 0) { saved.path.assign(data, length); okay = true; }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { okay = false; }
        if (slot.stringData) reinterpret_cast<DestroyWide>(base_ + 0x0134EFD0)(&slot);
        if (path.stringData) reinterpret_cast<DestroyWide>(base_ + 0x0134EFD0)(&path);
        return okay;
    }
    bool SessionCheckpointService::SaveNative(Saved& saved) noexcept
    {
        if (!ResolvePath(saved)) return false;
        auto& hero = contexts_->players.localHero;
        auto* world = static_cast<unsigned char*>(World(base_));
        auto* creature = static_cast<unsigned char*>(hero.NativeHero());
        if (!world || !creature || !hero.Hero()) return false;
        Wide name;
        unsigned char oldMarker[16] = {};
        float oldHealth = 0;
        bool swapped = false, okay = false;
        game::persistence::CheckpointHeroProjection projection;
        projection.uid = hero.Hero()->GetUid(); projection.map = saved.ticket.map; projection.position = saved.ticket.position;
        __try
        {
            reinterpret_cast<ConstructWide>(base_ + 0x0134F580)(&name, saved.name.c_str());
            std::memcpy(oldMarker, world + 0x151, 12); std::memcpy(oldMarker + 12, world + 0x160, 4);
            oldHealth = *reinterpret_cast<float*>(creature + 0xD0);
            swapped = true;
            std::memcpy(world + 0x151, &saved.ticket.position, 12); std::memcpy(world + 0x160, &saved.ticket.facing, 4);
            // Serialize a viable checkpoint Hero for a dead guest without recovery, inventory
            // use, a health mutation event or even one live game frame with increased HP.
            if (oldHealth <= 0.01f) *reinterpret_cast<float*>(creature + 0xD0) = *reinterpret_cast<float*>(creature + 0xCC);
            saved.health = *reinterpret_cast<float*>(creature + 0xD0);
            game::persistence::checkpointHeroProjection = &projection;
            projecting_ = true;
            // The manual-save wrapper can copy QuestCheckPoint instead of serializing
            // the current Hero. Use its full-serializer branch directly for paired slots.
            originalSave_(world, &name, 0, 0, 0);
            okay = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { okay = false; }
        projecting_ = false; game::persistence::checkpointHeroProjection = nullptr;
        if (swapped)
        {
            std::memcpy(world + 0x151, oldMarker, 12); std::memcpy(world + 0x160, oldMarker + 12, 4);
            *reinterpret_cast<float*>(creature + 0xD0) = oldHealth;
        }
        if (name.stringData) reinterpret_cast<DestroyWide>(base_ + 0x0134EFD0)(&name);
        return okay;
    }
    bool SessionCheckpointService::StableFile(Saved& saved) noexcept
    {
        std::uint64_t size = 0, stamp = 0;
        if (!FileInfo(saved.path.c_str(), size, stamp) || stamp == fileOriginalStamp_) return false;
        if (size != saved.size || stamp != saved.stamp)
        { saved.size = size; saved.stamp = stamp; fileStableSince_ = GetTickCount64(); return false; }
        return GetTickCount64() - fileStableSince_ >= 500;
    }
    bool SessionCheckpointService::VerifyFile(const Saved& saved) const noexcept
    {
        std::uint64_t size = 0, stamp = 0;
        return saved.ticket.revision && FileInfo(saved.path.c_str(), size, stamp) && size == saved.size && stamp == saved.stamp;
    }
    bool SessionCheckpointService::RequestReloadForTest() noexcept
    {
        if (role_ != PeerRole::Host || !contexts_->actions.playerDeath.IsTerminalDead()) return false;
        // Match the retail Enter path: close the death card and release its UI
        // pause through vf28 before selecting a save from the load menu.
        auto* card = static_cast<unsigned char*>(game::entity::native::ThingComponentAccess::Find(
            contexts_->players.localHero.NativeHero(), static_cast<game::entity::native::ThingComponentType>(0xFC)));
        if (!card || !card[0xD] || *reinterpret_cast<int*>(card + 0x24) != 3) return false;
        card[0x20] = 1;
        auto** table = *reinterpret_cast<void***>(card);
        reinterpret_cast<void(__thiscall*)(void*)>(table[28])(card);
        return VerifyFile(committed_[0]) && QueueNativeLoad(committed_[0], true);
    }
    bool SessionCheckpointService::ShowPartyDeathCard() noexcept
    {
        // The terminal UI is independent of health/flasks. Enter the same type3
        // card directly, not PlayerCreature::vf16 (which could spend host lives).
        auto* card = static_cast<unsigned char*>(game::entity::native::ThingComponentAccess::Find(
            contexts_->players.localHero.NativeHero(), static_cast<game::entity::native::ThingComponentType>(0xFC)));
        if (!card) return false;
        game::native::CharString empty; Wide text;
        bool shown = false;
        __try
        {
            constexpr unsigned char expected[] = {0x83, 0xEC, 0x08, 0x83, 0x7C, 0x24, 0x0C, 0x03};
            if (std::memcmp(reinterpret_cast<void*>(base_ + 0x01A81D80), expected, sizeof(expected)) != 0) return false;
            if (*reinterpret_cast<std::uintptr_t*>(card) != base_ + 0x02B08434 || card[0xD]) return false;
            reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(base_ + 0x012B7800)(&empty, "", -1);
            reinterpret_cast<ConstructWide>(base_ + 0x0134F580)(&text,
                L"\u041e\u0434\u0438\u043d \u0438\u0437 \u0433\u0435\u0440\u043e\u0435\u0432 \u043f\u043e\u0433\u0438\u0431. \u041d\u0435\u043e\u0431\u0445\u043e\u0434\u0438\u043c\u043e \u0437\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044c \u043f\u043e\u0441\u043b\u0435\u0434\u043d\u044e\u044e \u043a\u043e\u043d\u0442\u0440\u043e\u043b\u044c\u043d\u0443\u044e \u0442\u043e\u0447\u043a\u0443.");
            using Show = void(__thiscall*)(void*, int, const game::native::CharString*, unsigned char, const Wide*, unsigned char);
            reinterpret_cast<Show>(base_ + 0x01A81D80)(card, 3, &empty, 1, &text, 1);
            shown = true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { shown = false; }
        if (text.stringData) reinterpret_cast<DestroyWide>(base_ + 0x0134EFD0)(&text);
        if (empty.stringData) reinterpret_cast<void(__thiscall*)(void*)>(base_ + 0x012B75D0)(&empty);
        return shown;
    }
    bool SessionCheckpointService::Send(Ticket ticket, const Operation op)
    {
        ticket.operation = op;
        return contexts_->transport.transport.SubmitReliable(reliable_stream::Control, protocol::PacketType::Checkpoint,
            reinterpret_cast<const std::uint8_t*>(&ticket), sizeof(ticket));
    }
    bool SessionCheckpointService::AllAcknowledged() const
    {
        for (auto actor : contexts_->transport.transport.ConnectedActorIds())
            if (!acknowledged_.count(actor)) return false;
        return true;
    }
    void SessionCheckpointService::CommitLocal(const Saved& saved)
    {
        committed_[saved.ticket.kind] = saved;
        // The local manifest identifies an immutable native save belonging to THIS Hero.
        const std::wstring manifest = saved.path + L".coop-id";
        HANDLE file = CreateFileW(manifest.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0; WriteFile(file, &saved.ticket, sizeof(saved.ticket), &written, nullptr); FlushFileBuffers(file); CloseHandle(file);
        }
        diagnostics_.Event("MultiplayerCheckpointCommitted", "all connected players saved matching checkpoint generation; individual Hero sections retained");
    }

    bool SessionCheckpointService::HandleReliableMessage(const TransportMessage& message)
    {
        if (message.payloadSize != sizeof(Ticket)) return false;
        Ticket t; std::memcpy(&t, message.payload.data(), sizeof(t));
        if (!t.epoch || !t.revision || t.kind > 1 || t.slot > 1 || t.reserved || !t.map ||
            t.operation < Operation::Prepare || t.operation > Operation::HeroDied ||
            !std::memchr(t.region, 0, sizeof(t.region)) || !t.region[0] ||
            !std::isfinite(t.position.x) || !std::isfinite(t.position.y) || !std::isfinite(t.position.z) || !std::isfinite(t.facing) ||
            std::fabs(t.position.x) > 1000000 || std::fabs(t.position.y) > 1000000 || std::fabs(t.position.z) > 1000000) return false;
        bool authorized = false;
        for (const auto& peer : contexts_->transport.remotePlayerChannels.Snapshots())
            if (peer.state.actorId == message.sourceActorId &&
                (role_ == PeerRole::Guest ? peer.state.role == PeerRole::Host : peer.state.role == PeerRole::Guest)) authorized = true;
        if (!authorized || (role_ == PeerRole::Host && t.epoch != epoch_)) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (role_ == PeerRole::Guest)
        {
            if (remoteHost_ != message.sourceActorId || remoteNonce_ != message.connectionNonce)
            {
                remoteHost_ = message.sourceActorId; remoteNonce_ = message.connectionNonce; inbound_.clear();
                hostSessionChanged_.store(true, std::memory_order_release);
            }
        }
        if (inbound_.size() < 64) inbound_.push_back({t, message.sourceActorId});
        return true;
    }

    void SessionCheckpointService::ResetTransientReplication()
    {
        contexts_->entities.entityMovement.ResetForReload();
        contexts_->world.destructibles.ResetForReload();
        contexts_->world.questEvents.ResetForReload();
        contexts_->world.chestSync.ResetForReload();
        contexts_->world.questFields.ResetForReload();
        contexts_->world.worldClock.ResetForReload();
        contexts_->world.weather.ResetForReload();
        contexts_->world.effects.ResetForReload();
        contexts_->players.remotePlayers.BeginWorldTransition();
        contexts_->players.localHero.BeginCheckpointReload();
    }
    bool SessionCheckpointService::BeginReload(const Saved& saved)
    {
        if (!VerifyFile(saved)) return false;
        if (!World(base_) || !originalLoad_) return false;
        if (!contexts_->world.savedEntityMapBaseline.BeginCheckpointHeroLoad()) return false;
        reloading_ = saved; loading_ = true; bundleLoaded_.store(false, std::memory_order_release);
        sawWorldDeparture_ = false; oldHero_ = contexts_->players.localHero.NativeHero();
        loadStartedAt_ = GetTickCount64(); deathPrompt_.Close(); waitPrompt_.Close();
        // Host restores its own native world first; guests then use newly published host
        // world sections and their matching local PLAYER checkpoint through the normal gate.
        game::persistence::pairedCheckpointLoad.store(role_ == PeerRole::Host, std::memory_order_release);
        // Release script controls, companion links and effect references while
        // their owning world is still valid, before any native load callback.
        ResetTransientReplication();
        if (!QueueNativeLoad(saved))
        {
            loading_ = false;
            game::persistence::pairedCheckpointLoad.store(false, std::memory_order_release);
            contexts_->players.localHero.OnWorldReady();
            contexts_->players.remotePlayers.CompleteWorldTransition();
            reloadRequested_ = false;
            phase_ = Phase::Idle;
            diagnostics_.Event("MultiplayerCheckpointReloadBlocked", "native CWorld rejected checkpoint load request");
            return false;
        }
        diagnostics_.Event("MultiplayerCheckpointLoadStarted", "CWorld load request accepted; native loading state machine scheduled with matching local file");
        return true;
    }
    void SessionCheckpointService::FinishReload()
    {
        game::persistence::pairedCheckpointLoad.store(false, std::memory_order_release);
        loading_ = false;
        contexts_->actions.playerDeath.ResetForCheckpointLoad();
        partyDeathRequired_ = partyDeathCardShown_ = false; nextDeathNoticeAt_ = 0;
        contexts_->players.remotePlayers.CompleteWorldTransition();
        if (role_ == PeerRole::Host)
        {
            contexts_->world.questState.CaptureHostCurrent();
            pending_ = reloading_; phase_ = Phase::AwaitHostBaseline; phaseSince_ = GetTickCount64();
        }
        else
        {
            lastLoaded_ = reloading_.ticket;
            completeAckPending_ = !Send(lastLoaded_, Operation::ReloadComplete);
            reloadRequested_ = false; waitPrompt_.Close(); phase_ = Phase::Idle;
        }
        diagnostics_.Event("MultiplayerCheckpointLoadCompleted", "native Hero/world restored; awaiting common host world baseline");
    }

    bool SessionCheckpointService::Process()
    {
        if (!contexts_ || !originalSave_) return true;
        auto& hero = contexts_->players.localHero;
        auto& transport = contexts_->transport.transport;
        const auto now = GetTickCount64();
        if (loading_)
        {
            if (bundleLoaded_.load(std::memory_order_acquire))
            {
                hero.OnWorldReady();
                if (hero.IsEntryPending()) hero.TryBind();
            }
            const bool ready = hero.IsWorldReady() && hero.NativeHero();
            if (!ready) sawWorldDeparture_ = true;
            if (ready && bundleLoaded_.load(std::memory_order_acquire) &&
                (sawWorldDeparture_ || hero.NativeHero() != oldHero_ || now - loadStartedAt_ > 2000) && now - loadStartedAt_ > 1000) FinishReload();
            return true;
        }
        if (role_ == PeerRole::Guest && contexts_->actions.playerDeath.IsTerminalDead() && !reloadRequested_ && now >= nextDeathNoticeAt_)
        {
            const auto& saved = committed_[1].ticket.revision > committed_[0].ticket.revision ? committed_[1] : committed_[0];
            if (saved.ticket.revision && hero.CurrentState())
            {
                Ticket notice = saved.ticket;
                // HeroDied reuses section-revision fields for the actor lifecycle.
                notice.rollback = WorldFence(); notice.map = hero.MapId();
                notice.quests = hero.CurrentState()->actorGeneration;
                notice.regions = hero.CurrentState()->mapEpoch;
                notice.factions = hero.CurrentState()->authorityEpoch;
                if (Send(notice, Operation::HeroDied)) nextDeathNoticeAt_ = now + 1000;
            }
        }
        if (completeAckPending_) completeAckPending_ = !Send(lastLoaded_, Operation::ReloadComplete);
        if (role_ == PeerRole::Host &&
            (contexts_->actions.playerDeath.IsTerminalDead() || partyDeathRequired_) &&
            !reloadRequested_ && now >= nextDeathNoticeAt_ && hero.CurrentState())
        {
            Ticket notice = {};
            notice.epoch = epoch_; notice.revision = revision_ ? revision_ : 1;
            notice.rollback = WorldFence(); notice.map = hero.MapId();
            notice.quests = hero.CurrentState()->actorGeneration;
            notice.regions = hero.CurrentState()->mapEpoch;
            notice.factions = hero.CurrentState()->authorityEpoch;
            strncpy_s(notice.region, hero.MapName().c_str(), _TRUNCATE);
            if (Send(notice, Operation::HeroDied)) nextDeathNoticeAt_ = now + 1000;
        }
        if (role_ == PeerRole::Guest &&
            (contexts_->actions.playerDeath.IsTerminalDead() || partyDeathRequired_) && !reloadRequested_)
        {
            if (deathPrompt_.Poll() != game::native::NativePromptBox::State::Showing)
                deathPrompt_.Show(L"\u041e\u0434\u0438\u043d \u0438\u0437 \u0433\u0435\u0440\u043e\u0435\u0432 \u043f\u043e\u0433\u0438\u0431. \u041e\u0436\u0438\u0434\u0430\u043d\u0438\u0435 \u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0438 \u043a\u043e\u043d\u0442\u0440\u043e\u043b\u044c\u043d\u043e\u0439 \u0442\u043e\u0447\u043a\u0438 \u0445\u043e\u0441\u0442\u043e\u043c.");
        }
        else deathPrompt_.Close();
        std::deque<std::pair<Ticket, std::uint64_t>> inbound;
        { std::lock_guard<std::mutex> lock(mutex_); inbound.swap(inbound_); }
        if (hostSessionChanged_.exchange(false, std::memory_order_acq_rel))
        {
            pending_ = {}; committed_ = {}; deferredGo_ = {}; lastLoaded_ = {};
            completeAckPending_ = false; reloadRequested_ = false; phase_ = Phase::Idle;
            partyDeathRequired_ = partyDeathCardShown_ = false; nextDeathNoticeAt_ = 0;
            worldFence_.store(0, std::memory_order_release); waitPrompt_.Close();
        }
        for (const auto& entry : inbound)
        {
            const auto& t = entry.first;
            if (role_ == PeerRole::Guest && t.operation == Operation::HeroDied)
            {
                const auto* peer = contexts_->transport.remotePlayerChannels.Find(entry.second);
                if (peer && peer->role == PeerRole::Host && t.rollback == WorldFence() &&
                    peer->actorGeneration == t.quests && peer->mapEpoch == t.regions &&
                    peer->authorityEpoch == t.factions && peer->mapId == t.map && !reloadRequested_)
                {
                    if (!partyDeathRequired_)
                        diagnostics_.Event("MultiplayerPartyDeathWaiting", "host reported party death; waiting for host checkpoint selection");
                    partyDeathRequired_ = true;
                    if (phase_ == Phase::SaveFile || phase_ == Phase::AwaitSaved) { pending_ = {}; phase_ = Phase::Idle; }
                }
                continue;
            }
            if (role_ == PeerRole::Host && t.operation == Operation::HeroDied)
            {
                const auto* peer = contexts_->transport.remotePlayerChannels.Find(entry.second);
                if (peer && t.rollback == WorldFence() && peer->actorGeneration == t.quests &&
                    peer->mapEpoch == t.regions && peer->authorityEpoch == t.factions && peer->mapId == t.map && !reloadRequested_)
                {
                    partyDeathRequired_ = true;
                    if (phase_ == Phase::SaveFile || phase_ == Phase::AwaitSaved) { pending_ = {}; phase_ = Phase::Idle; }
                }
                continue;
            }
            if (role_ == PeerRole::Guest && (t.operation == Operation::ReloadPrepare || t.operation == Operation::ReloadGo) &&
                (!t.rollback || t.rollback < WorldFence())) continue;
            if (role_ == PeerRole::Host)
            {
                if (t.epoch != pending_.ticket.epoch || t.revision != pending_.ticket.revision || t.kind != pending_.ticket.kind ||
                    t.slot != pending_.ticket.slot || t.rollback != pending_.ticket.rollback) continue;
                if ((phase_ == Phase::AwaitSaved && t.operation == Operation::Saved) ||
                    (phase_ == Phase::AwaitReloadReady && t.operation == Operation::ReloadReady) ||
                    (phase_ == Phase::AwaitGuestLoaded && t.operation == Operation::ReloadComplete)) acknowledged_.insert(entry.second);
            }
            else if (t.operation == Operation::Prepare && pending_.ticket.epoch == t.epoch &&
                pending_.ticket.revision == t.revision && phase_ == Phase::AwaitSaved && VerifyFile(pending_))
            { Send(t, Operation::Saved); }
            else if (t.operation == Operation::Prepare && (t.epoch != committed_[t.kind].ticket.epoch || t.revision > committed_[t.kind].ticket.revision))
            {
                if (phase_ != Phase::Idle) continue;
                pending_ = {}; pending_.ticket = t; phase_ = Phase::SaveFile; phaseSince_ = now;
                // Generation/map authority comes from the host; the serialized
                // Hero placement belongs to THIS guest, not the host's marker.
                if (hero.CurrentState() && hero.MapName()==t.region)
                {
                    pending_.ticket.position=hero.CurrentState()->position;
                    pending_.ticket.facing=hero.CurrentState()->facing;
                    diagnostics_.Event("MultiplayerGuestCheckpointPlacementCaptured",
                        "guest save retains its own position/facing; checkpoint generation remains host-owned");
                }
                worldFence_.store(t.rollback, std::memory_order_release);
                fileOriginalStamp_ = 0; fileStableSince_ = 0; nextRetryAt_ = 0;
            }
            else if (t.operation == Operation::Commit && pending_.ticket.epoch == t.epoch && pending_.ticket.revision == t.revision &&
                pending_.ticket.kind == t.kind && pending_.ticket.slot == t.slot && VerifyFile(pending_))
            { CommitLocal(pending_); phase_ = Phase::Idle; }
            else if (t.operation == Operation::ReloadPrepare)
            {
                const auto& saved = committed_[t.kind];
                if (saved.ticket.epoch == t.epoch && saved.ticket.revision == t.revision && VerifyFile(saved))
                {
                    if (!reloadRequested_)
                    {
                        contexts_->players.remotePlayers.BeginWorldTransition();
                        contexts_->world.populationSimulation.SetHighDetailReady(hero.MapName(), false);
                        diagnostics_.Event("MultiplayerCheckpointPresentationSuspended", "guest accepted reload barrier; native replicas retired before host rollback");
                    }
                    reloadRequested_ = true;
                    worldFence_.store(t.rollback, std::memory_order_release);
                    if (!Send(t, Operation::ReloadReady))
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (inbound_.size() < 64) inbound_.push_back(entry);
                    }
                }
                else diagnostics_.Event("MultiplayerCheckpointReloadBlocked", "guest has no verified matching committed checkpoint; no wrong save loaded");
            }
            else if (t.operation == Operation::ReloadGo)
            {
                if (lastLoaded_.epoch == t.epoch && lastLoaded_.revision == t.revision && lastLoaded_.rollback == t.rollback)
                    completeAckPending_ = !Send(lastLoaded_, Operation::ReloadComplete);
                else deferredGo_ = t;
            }
        }
        if (role_ == PeerRole::Guest && deferredGo_.revision && hero.IsWorldReady())
        {
            const auto& t = deferredGo_;
            Saved saved = committed_[t.kind];
            saved.ticket.rollback = t.rollback;
            using Section = protocol::WorldSection;
            if (saved.ticket.epoch == t.epoch && saved.ticket.revision == t.revision &&
                contexts_->world.questState.CurrentSnapshotRevision() >= t.quests &&
                contexts_->world.worldSections.CurrentSnapshotRevision(Section::Regions) >= t.regions &&
                contexts_->world.worldSections.CurrentSnapshotRevision(Section::Factions) >= t.factions && BeginReload(saved)) deferredGo_ = {};
            return true;
        }
        const int requested = requestedLoad_.exchange(-1, std::memory_order_acq_rel);
        if (role_ == PeerRole::Host && requested >= 0)
        {
            const auto preferred = requested == 17 ? 1 : 0;
            const Saved& saved = committed_[preferred].ticket.revision ? committed_[preferred] : committed_[1 - preferred];
            if (VerifyFile(saved))
            {
                pending_ = saved; pending_.ticket.rollback = ++reloadCounter_;
                worldFence_.store(pending_.ticket.rollback, std::memory_order_release);
                phase_ = Phase::AwaitReloadReady; acknowledged_.clear(); phaseSince_ = now;
                reloadRequested_ = true; nextRetryAt_ = 0;
                contexts_->players.remotePlayers.BeginWorldTransition();
                contexts_->world.populationSimulation.SetHighDetailReady(hero.MapName(), false);
                diagnostics_.Event("MultiplayerCheckpointPresentationSuspended", "host entered reload barrier; native replicas retired before world load");
            }
            else diagnostics_.Event("MultiplayerCheckpointReloadBlocked", "no fully committed paired checkpoint; preserving existing saves");
        }
        if (role_ == PeerRole::Host && partyDeathRequired_ && !partyDeathCardShown_ && !reloadRequested_ &&
            hero.IsWorldReady() && !contexts_->actions.playerDeath.IsTerminalDead() && ShowPartyDeathCard())
        {
            partyDeathCardShown_ = true;
            diagnostics_.Event("MultiplayerPartyDeathCardShown", "guest terminal death requires host checkpoint choice; living host health/flasks untouched");
        }
        // The host keeps the retail death/load UI. A second modal prompt can
        // intercept its input or obscure the loading screen. Only guests wait.
        if (role_ == PeerRole::Guest && reloadRequested_ && !loading_)
        {
            if (waitPrompt_.Poll() != game::native::NativePromptBox::State::Showing)
                waitPrompt_.Show(L"\u041e\u0436\u0438\u0434\u0430\u043d\u0438\u0435 \u0438\u0433\u0440\u043e\u043a\u043e\u0432 \u0434\u043b\u044f \u043e\u0431\u0449\u0435\u0439 \u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0438 \u043a\u043e\u043d\u0442\u0440\u043e\u043b\u044c\u043d\u043e\u0439 \u0442\u043e\u0447\u043a\u0438...");
        }
        if (role_ == PeerRole::Host && phase_ == Phase::AwaitReloadReady)
        {
            if (now >= nextRetryAt_) { Send(pending_.ticket, Operation::ReloadPrepare); nextRetryAt_ = now + 1000; }
            if (AllAcknowledged()) BeginReload(pending_);
            return true;
        }
        if (role_ == PeerRole::Host && phase_ == Phase::AwaitHostBaseline && now - phaseSince_ >= 1000)
        {
            using Section = protocol::WorldSection;
            pending_.ticket.quests = contexts_->world.questState.CurrentSnapshotRevision();
            pending_.ticket.regions = contexts_->world.worldSections.CurrentSnapshotRevision(Section::Regions);
            pending_.ticket.factions = contexts_->world.worldSections.CurrentSnapshotRevision(Section::Factions);
            if (Send(pending_.ticket, Operation::ReloadGo))
            { phase_ = Phase::AwaitGuestLoaded; acknowledged_.clear(); nextRetryAt_ = now + 1000; }
        }
        if (role_ == PeerRole::Host && phase_ == Phase::AwaitGuestLoaded)
        {
            if (AllAcknowledged()) { phase_ = Phase::Idle; reloadRequested_ = false; waitPrompt_.Close(); }
            else if (now >= nextRetryAt_) { Send(pending_.ticket, Operation::ReloadGo); nextRetryAt_ = now + 1000; }
        }
        if (!hero.IsWorldReady() || !hero.CurrentState() || !transport.HasPeer()) { stableWorldSince_ = 0; return true; }
        if (!stableWorldSince_) stableWorldSince_ = now;
        if (role_ == PeerRole::Host && phase_ == Phase::Idle && !partyDeathRequired_ && !contexts_->actions.playerDeath.IsTerminalDead() && now - stableWorldSince_ > 2000)
        {
            const auto peerRevision = transport.PeerSetRevision();
            const unsigned int source = sourceSave_.exchange(0, std::memory_order_acq_rel);
            if (source || peerRevision != peerRevision_)
            {
                peerRevision_ = peerRevision; pending_ = {};
                pending_.ticket.epoch = epoch_; pending_.ticket.revision = ++revision_;
                pending_.ticket.rollback = WorldFence();
                pending_.ticket.kind = source == 2 ? 1 : 0;
                pending_.ticket.slot = committed_[pending_.ticket.kind].ticket.revision ? 1 - committed_[pending_.ticket.kind].ticket.slot : 0;
                pending_.ticket.map = hero.MapId(); pending_.ticket.position = hero.CurrentState()->position;
                pending_.ticket.facing = hero.CurrentState()->facing;
                strncpy_s(pending_.ticket.region, hero.MapName().c_str(), _TRUNCATE);
                phase_ = Phase::SaveFile; phaseSince_ = now; nextRetryAt_ = 0; fileStableSince_ = 0; fileOriginalStamp_ = 0;
            }
        }
        if (phase_ == Phase::SaveFile)
        {
            if (!fileStableSince_ && now >= nextRetryAt_)
            {
                ResolvePath(pending_);
                std::uint64_t size = 0; FileInfo(pending_.path.c_str(), size, fileOriginalStamp_);
                if (SaveNative(pending_)) fileStableSince_ = now;
                nextRetryAt_ = now + 1000;
            }
            if (fileStableSince_ && StableFile(pending_))
            {
                if (role_ == PeerRole::Host)
                {
                    if (Send(pending_.ticket, Operation::Prepare)) { phase_ = Phase::AwaitSaved; acknowledged_.clear(); nextRetryAt_ = now + 1000; }
                }
                else if (Send(pending_.ticket, Operation::Saved)) phase_ = Phase::AwaitSaved;
            }
        }
        if (role_ == PeerRole::Host && phase_ == Phase::AwaitSaved)
        {
            if (AllAcknowledged() && Send(pending_.ticket, Operation::Commit)) { CommitLocal(pending_); phase_ = Phase::Idle; }
            else if (now >= nextRetryAt_) { Send(pending_.ticket, Operation::Prepare); nextRetryAt_ = now + 1000; }
        }
        if (!reloadRequested_ && phase_ != Phase::Idle && now - phaseSince_ > 30000)
        {
            diagnostics_.Event("MultiplayerCheckpointSaveDeferred", "matching checkpoint was not committed; previous committed slots retained");
            if (role_ == PeerRole::Host) sourceSave_.store(pending_.ticket.kind ? 2 : 1, std::memory_order_release);
            phase_ = Phase::Idle;
        }
        return true;
    }

    void SessionCheckpointService::Shutdown() noexcept
    {
        bundleHook_.Shutdown();
        if (!(regionCall_.Shutdown() & loadButton_.Shutdown() & load_.Shutdown() & select_.Shutdown() & save_.Shutdown())) return;
        game::persistence::pairedCheckpointLoad.store(false, std::memory_order_release);
        deathPrompt_.Close(); waitPrompt_.Close();
        if (active_ == this) active_ = nullptr;
        originalSave_ = nullptr; originalSelect_ = nullptr; originalLoad_ = nullptr;
        partyDeathRequired_ = partyDeathCardShown_ = false; nextDeathNoticeAt_ = 0;
        originalButton_ = nullptr; worldEntered_.store(false);
        contexts_ = nullptr; entities_ = nullptr; loading_ = reloadRequested_ = false;
        pending_ = {}; reloading_ = {}; committed_ = {}; phase_ = Phase::Idle;
        acknowledged_.clear(); inbound_.clear(); sourceSave_.store(0);
        epoch_ = revision_ = peerRevision_ = stableWorldSince_ = 0;
        lastLoaded_ = {}; deferredGo_ = {}; completeAckPending_ = false; requestedLoad_.store(-1);
        worldFence_.store(0); reloadCounter_ = 0; bundleLoaded_.store(false);
        hostSessionChanged_.store(false); remoteHost_ = remoteNonce_ = 0;
    }
}
