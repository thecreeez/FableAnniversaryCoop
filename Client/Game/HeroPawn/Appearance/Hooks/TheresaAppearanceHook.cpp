#include "TheresaAppearanceHook.h"

#include "Game/Native/Addresses.h"
#include "Game/Native/ScriptTypes.h"
#include "Game/HeroPawn/Appearance/Native/HeroClothingComponent.h"

#include <cstdio>
#include <cstring>

namespace
{
    // CThingPlayerCreature::vf28, thiscall(type, graphic), RET 8.
    constexpr std::uintptr_t PlayerSetGraphicRva = 0x01B5BAF0;
    constexpr std::uintptr_t PlayerSetGraphicHandlerRva = 0x024AC1F8;
    // CTCHeroAttachableAppearanceModifiers rebuild, including direct vf5/vf1 paths.
    constexpr std::uintptr_t ModifierBuildRva = 0x019B4290;
    constexpr std::uintptr_t ModifierBuildHandlerRva = 0x025313D8;
    // Preceding body-part visibility stage, same component and thiscall(void).
    constexpr std::uintptr_t BodyPartBuildRva = 0x019B2DB0;
    constexpr std::uintptr_t BodyPartBuildHandlerRva = 0x025311D0;
    constexpr std::uintptr_t PlayerVtableRva = 0x02B1DBB4;

    struct ModifierEntry final
    {
        std::int32_t definition;
        std::uint32_t enabled;
    };

    struct ModifierCollection final
    {
        ModifierEntry* begin;
        ModifierEntry* end;
        ModifierEntry* capacity;
        std::uint32_t metadata;
    };

    struct ClothingProjection final
    {
        ModifierEntry entries[3][6] = {};
        ModifierCollection collections[3] = {};
        unsigned int selected = 0;
        unsigned int matched = 0;
    };

    static_assert(sizeof(ModifierEntry) == 8 && sizeof(ModifierCollection) == 0x10);

    bool SelectClothing(void* component,
        const fable::game::hero_pawn::appearance::HeroClothingState& clothing,
        ClothingProjection& projection) noexcept
    {
        __try
        {
            const auto* bytes = static_cast<const std::uint8_t*>(component);
            auto* begin = *reinterpret_cast<ModifierCollection* const*>(bytes + 0x3C);
            auto* end = *reinterpret_cast<ModifierCollection* const*>(bytes + 0x40);
            if (begin == nullptr || reinterpret_cast<std::uintptr_t>(end) !=
                reinterpret_cast<std::uintptr_t>(begin) + 3 * sizeof(ModifierCollection)) return false;
            for (auto definition : clothing.definitionIndices)
                if (definition > 0) ++projection.selected;
            for (unsigned int category = 0; category < 3; ++category)
            {
                auto& source = begin[category];
                auto& target = projection.collections[category];
                target.begin = target.end = projection.entries[category];
                target.capacity = target.begin + 6;
                target.metadata = source.metadata;
                const auto first = reinterpret_cast<std::uintptr_t>(source.begin);
                const auto last = reinterpret_cast<std::uintptr_t>(source.end);
                if (last < first || (last - first) % sizeof(ModifierEntry) != 0 ||
                    (last - first) / sizeof(ModifierEntry) > 128 ||
                    (last > first && source.begin == nullptr)) return false;
                const auto count = (last - first) / sizeof(ModifierEntry);
                for (std::size_t index = 0; index < count; ++index)
                {
                    const ModifierEntry& entry = source.begin[index];
                    bool selected = false;
                    for (auto definition : clothing.definitionIndices)
                        if (definition > 0 && definition == entry.definition) selected = true;
                    if (!selected) continue;
                    if (target.end == target.capacity) return false;
                    *target.end++ = {entry.definition, (entry.enabled & ~0xFFu) | 1u};
                    ++projection.matched;
                }
            }
            // Do not clear an existing outfit while the wear operation is
            // still publishing its modifier entries. Empty selection is valid
            // and must still run native removal when the player undresses.
            for (auto definition : clothing.definitionIndices)
            {
                if (definition <= 0) continue;
                bool found = false;
                for (unsigned int category = 0; category < 3; ++category)
                    for (auto* entry = projection.collections[category].begin;
                        entry != projection.collections[category].end; ++entry)
                        if (entry->definition == definition) found = true;
                if (!found) return false;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SignatureMatches(std::uint8_t* target, std::uintptr_t handler) noexcept
    {
        __try
        {
            const std::uint8_t prefix[] = {0x6A, 0xFF, 0x68};
            return std::memcmp(target, prefix, sizeof(prefix)) == 0 &&
                *reinterpret_cast<const std::uintptr_t*>(target + 3) == handler &&
                target[7] == 0x64 && target[8] == 0xA1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

namespace fable::game::hero_pawn::appearance::hooks
{
    TheresaAppearanceHook* TheresaAppearanceHook::active_ = nullptr;
    std::atomic<void*> TheresaAppearanceHook::remoteDefinition_{nullptr};
    std::atomic<void*> TheresaAppearanceHook::remoteChildDefinition_{nullptr};
    std::atomic<void*> TheresaAppearanceHook::localThing_{nullptr};

    bool TheresaAppearanceHook::Install(HMODULE module, bool guest,
        const core::Diagnostics& diagnostics) noexcept
    {
        if (active_ == this) return true;
        if (module == nullptr || active_ != nullptr) return false;
        module_ = module;
        diagnostics_ = diagnostics;
        guest_ = guest;
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        auto* graphic = reinterpret_cast<std::uint8_t*>(base + PlayerSetGraphicRva);
        auto* modifiers = reinterpret_cast<std::uint8_t*>(base + ModifierBuildRva);
        auto* bodyParts = reinterpret_cast<std::uint8_t*>(base + BodyPartBuildRva);
        std::uint8_t bodyPrefix[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8,
            0x6A, 0xFF, 0x68, 0, 0, 0, 0};
        const auto bodyHandler = base + BodyPartBuildHandlerRva;
        std::memcpy(bodyPrefix + 9, &bodyHandler, sizeof(bodyHandler));
        if (!SignatureMatches(graphic, base + PlayerSetGraphicHandlerRva) ||
            !SignatureMatches(modifiers, base + ModifierBuildHandlerRva) ||
            std::memcmp(bodyParts, bodyPrefix, sizeof(bodyPrefix)) != 0)
        {
            diagnostics_.Event("MultiplayerTheresaAppearanceRejected", "native signature mismatch");
            return false;
        }
        std::uint8_t graphicPrefix[] = {0x6A, 0xFF, 0x68, 0, 0, 0, 0};
        std::uint8_t modifierPrefix[] = {0x6A, 0xFF, 0x68, 0, 0, 0, 0};
        const auto graphicHandler = base + PlayerSetGraphicHandlerRva;
        const auto modifierHandler = base + ModifierBuildHandlerRva;
        std::memcpy(graphicPrefix + 3, &graphicHandler, sizeof(graphicHandler));
        std::memcpy(modifierPrefix + 3, &modifierHandler, sizeof(modifierHandler));
        active_ = this;
        if (!graphicPatch_.Install(graphic, graphicPrefix, sizeof(graphicPrefix),
                reinterpret_cast<void*>(&SetPlayerGraphic), 7))
        {
            active_ = nullptr;
            return false;
        }
        originalGraphic_ = reinterpret_cast<SetGraphic>(graphicPatch_.Original());
        if (!modifierPatch_.Install(modifiers, modifierPrefix, sizeof(modifierPrefix),
                reinterpret_cast<void*>(&RebuildAppearance), 7))
        {
            if (graphicPatch_.Shutdown())
            {
                originalGraphic_ = nullptr;
                active_ = nullptr;
            }
            return false;
        }
        originalModifiers_ = reinterpret_cast<RebuildModifiers>(modifierPatch_.Original());
        if (!bodyPartPatch_.Install(bodyParts, bodyPrefix, sizeof(bodyPrefix),
                reinterpret_cast<void*>(&RebuildBodyParts), sizeof(bodyPrefix)))
        {
            Shutdown();
            return false;
        }
        originalBodyParts_ = reinterpret_cast<RebuildModifiers>(bodyPartPatch_.Original());
        if (!skin_.Install(module, diagnostics))
        {
            Shutdown();
            return false;
        }
        diagnostics_.Event("MultiplayerTheresaAppearanceReady",
            guest ? "guest retains Hero driver with Sister render skin"
                  : "host Hero unchanged; guest replicas use Hero driver with Sister render skin");
        return true;
    }

    void TheresaAppearanceHook::Shutdown() noexcept
    {
        if (!skin_.Shutdown())
        {
            diagnostics_.Event("MultiplayerTheresaHeroSkinShutdownDeferred", "native patch ownership changed");
            return;
        }
        const bool modifiersRestored = modifierPatch_.Shutdown();
        const bool graphicRestored = graphicPatch_.Shutdown();
        const bool bodyPartsRestored = bodyPartPatch_.Shutdown();
        if (!modifiersRestored || !graphicRestored || !bodyPartsRestored)
        {
            diagnostics_.Event("MultiplayerTheresaAppearanceShutdownDeferred", "native patch ownership changed");
            return;
        }
        if (active_ == this) active_ = nullptr;
        localThing_.store(nullptr, std::memory_order_release);
        remoteDefinition_.store(nullptr, std::memory_order_release);
        remoteChildDefinition_.store(nullptr, std::memory_order_release);
        originalGraphic_ = nullptr;
        originalModifiers_ = nullptr;
        originalBodyParts_ = nullptr;
        module_ = nullptr;
        guest_ = false;
        clothingDiagnosticCount_ = 0;
        diagnostics_ = {};
    }

    void TheresaAppearanceHook::RegisterRemoteDefinition(void* definition, bool childhood) noexcept
    {
        (childhood?remoteChildDefinition_:remoteDefinition_).store(definition, std::memory_order_release);
    }

    bool TheresaAppearanceHook::UsesTheresaPresentation(void* thing) noexcept
    {
        if (thing == nullptr) return false;
        __try
        {
            const auto* bytes = static_cast<const std::uint8_t*>(thing);
            if (thing == localThing_.load(std::memory_order_acquire) && active_ &&
                *reinterpret_cast<const std::uintptr_t*>(thing) ==
                    reinterpret_cast<std::uintptr_t>(active_->module_) + PlayerVtableRva)
                return true;
            void* remote = remoteDefinition_.load(std::memory_order_acquire);
            void* child=remoteChildDefinition_.load(std::memory_order_acquire);
            void* definition=*reinterpret_cast<void* const*>(bytes + 0x100);
            return (remote && definition==remote) || (child && definition==child);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void __fastcall TheresaAppearanceHook::SetPlayerGraphic(
        void* thing, void*, int type, unsigned int graphic)
    {
        auto* hook = active_;
        if (hook == nullptr || hook->originalGraphic_ == nullptr) return;
        if (hook->guest_ && thing != nullptr)
        {
            bool player = false;
            __try
            {
                player = *reinterpret_cast<std::uintptr_t*>(thing) ==
                    reinterpret_cast<std::uintptr_t>(hook->module_) + PlayerVtableRva;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { player = false; }
            if (player)
            {
                localThing_.store(thing, std::memory_order_release);
                hook->diagnostics_.Event("MultiplayerTheresaHeroDriverSelected",
                    "stock Hero graphic retained; Sister render skin follows Hero bones");
            }
        }
        hook->originalGraphic_(thing, type, graphic);
    }

    void __fastcall TheresaAppearanceHook::RebuildAppearance(void* component, void*)
    {
        auto* hook = active_;
        if (hook == nullptr || hook->originalModifiers_ == nullptr) return;
        hook->ProjectSelectedClothing(component, hook->originalModifiers_, "appearance");
    }

    void __fastcall TheresaAppearanceHook::RebuildBodyParts(void* component, void*)
    {
        auto* hook = active_;
        if (hook == nullptr || hook->originalBodyParts_ == nullptr) return;
        hook->ProjectSelectedClothing(component, hook->originalBodyParts_, "body-parts");
    }

    void TheresaAppearanceHook::ProjectSelectedClothing(
        void* component, RebuildModifiers original, const char* stage)
    {
        void* owner = nullptr;
        __try { if (component) owner = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(component) + 4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { owner = nullptr; }
        if (!UsesTheresaPresentation(owner))
        {
            original(component);
            return;
        }
        HeroClothingState clothing;
        ClothingProjection projection;
        if (!native::HeroClothingComponent::Capture(owner, clothing) ||
            !SelectClothing(component, clothing, projection))
        {
            if (clothingDiagnosticCount_++ < 16)
                diagnostics_.Event("MultiplayerTheresaClothingDeferred", "selected clothing/component view unavailable");
            return;
        }
        auto** begin = reinterpret_cast<ModifierCollection**>(static_cast<std::uint8_t*>(component) + 0x3C);
        auto** end = reinterpret_cast<ModifierCollection**>(static_cast<std::uint8_t*>(component) + 0x40);
        auto** capacity = reinterpret_cast<ModifierCollection**>(static_cast<std::uint8_t*>(component) + 0x44);
        ModifierCollection* savedBegin = *begin;
        ModifierCollection* savedEnd = *end;
        ModifierCollection* savedCapacity = *capacity;
        // Native callbacks copy queued graphics data / retain definition refs.
        // The bounded collection view itself stays synchronous and is restored
        // even if native processing raises. Never transfer its stack ownership.
        __try
        {
            *begin = projection.collections;
            *end = projection.collections + 3;
            *capacity = projection.collections + 3;
            original(component);
        }
        __finally
        {
            *begin = savedBegin;
            *end = savedEnd;
            *capacity = savedCapacity;
        }
        if (clothingDiagnosticCount_++ < 16)
        {
            char detail[192] = {};
            std::snprintf(detail, sizeof(detail),
                "thing=%p stage=%s selected=%u matched=%u operation=native-clothing-projection visual=unverified",
                owner, stage, projection.selected, projection.matched);
            diagnostics_.Event("MultiplayerTheresaClothingSubmitted", detail);
        }
    }
}
