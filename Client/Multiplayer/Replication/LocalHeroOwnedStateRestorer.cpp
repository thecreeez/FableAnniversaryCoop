#include "LocalHeroOwnedStateRestorer.h"

#include "Game/Entity/EntityService.h"
#include "Game/Entity/Entity.h"
#include "Game/Creature/Actions/Hooks/CreatureActionLifecycleObserver.h"
#include "Game/HeroPawn/Appearance/Native/HeroAttachableAppearanceComponent.h"
#include "Game/HeroPawn/Appearance/Native/HeroClothingComponent.h"
#include "Game/HeroPawn/Appearance/Native/HeroMorphComponent.h"
#include "Game/HeroPawn/Equipment/Native/HeroWeaponComponent.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

namespace
{
    constexpr std::uint64_t RestoreRetryMilliseconds = 100;

    bool HasPendingNativeWeaponAction(void* creature) noexcept
    {
        void* action = nullptr;
        __try
        {
            const auto* bytes = static_cast<const unsigned char*>(creature);
            // CThingCreature action queue, verified in native update01B42E20.
            if (*reinterpret_cast<const unsigned int*>(bytes + 0x12C)) return true;
            action = *reinterpret_cast<void* const*>(bytes + 0x120);
            if (!action || *(static_cast<const unsigned char*>(action) + 0x61)) return false;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return true; }
        char type[128] = {};
        return !fable::game::creature::actions::CreatureActionLifecycleObserver::DescribeActionType(action, type, sizeof(type)) ||
            std::strstr(type, "Sheathe") || std::strstr(type, "Unsheathe");
    }
}

namespace fable::multiplayer::replication
{
    void LocalHeroOwnedStateRestorer::Preserve(
        const PlayerState* const state,
        const core::Diagnostics& diagnostics) noexcept
    {
        Clear();
        if (state == nullptr || !state->heroMorph.IsSane() ||
            !state->heroClothing.IsSane() ||
            !state->heroBoneScales.IsSane() ||
            !state->heroAppearanceModifiers.IsSane() ||
            !state->heroEquipment.IsSane())
        {
            return;
        }
        preserved_ = *state;
        // A destination Construct restores current state, never an animation
        // transition from the departed map incarnation.
        preserved_.heroEquipment.transitionActionId = 0;
        pending_ = true;
        char detail[192] = {};
        std::snprintf(
            detail,
            sizeof(detail),
            "actor_id=%llu clothing=(%d,%d,%d,%d,%d,%d) melee=%d ranged=%d",
            static_cast<unsigned long long>(state->actorId),
            state->heroClothing.definitionIndices[0],
            state->heroClothing.definitionIndices[1],
            state->heroClothing.definitionIndices[2],
            state->heroClothing.definitionIndices[3],
            state->heroClothing.definitionIndices[4],
            state->heroClothing.definitionIndices[5],
            state->heroEquipment.meleeDefinitionIndex,
            state->heroEquipment.rangedDefinitionIndex);
        diagnostics.Event("MultiplayerLocalHeroOwnedStatePreserved", detail);
    }

    LocalHeroRestoreResult LocalHeroOwnedStateRestorer::Reconcile(
        game::EntityService& entities,
        game::Entity& hero,
        void* const nativeHero,
        const game::hero_pawn::appearance::HeroMorphState& morph,
        const game::hero_pawn::appearance::HeroClothingState& clothing,
        const game::hero_pawn::appearance::HeroBoneScaleState& boneScales,
        const game::hero_pawn::appearance::HeroAppearanceModifierState&
            modifiers,
        const game::hero_pawn::equipment::HeroEquipmentState& equipment,
        const core::Diagnostics& diagnostics) noexcept
    {
        if (!pending_)
        {
            return LocalHeroRestoreResult::Ready;
        }
        if (nativeHero == nullptr)
        {
            return LocalHeroRestoreResult::Pending;
        }
        const std::uint64_t now = GetTickCount64();
        if (now < nextAttemptAt_)
        {
            return LocalHeroRestoreResult::Pending;
        }
        nextAttemptAt_ = now + RestoreRetryMilliseconds;
        const auto failed = [this, &diagnostics](
            const std::uint8_t stage,
            const char* const name)
        {
            if (lastFailedStage_ != stage)
            {
                lastFailedStage_ = stage;
                diagnostics.Event(
                    "MultiplayerLocalHeroOwnedStateStagePending",
                    name);
            }
            return LocalHeroRestoreResult::Failed;
        };

        if (!clothing.Equals(preserved_.heroClothing))
        {
            return game::hero_pawn::appearance::native::HeroClothingComponent::
                    Apply(nativeHero, preserved_.heroClothing)
                ? LocalHeroRestoreResult::Pending
                : failed(1, "clothing");
        }
        if (!modifiersDegraded_ &&
            !modifiers.Equals(preserved_.heroAppearanceModifiers))
        {
            if (game::hero_pawn::appearance::native::
                    HeroAttachableAppearanceComponent::Apply(
                        nativeHero,
                        preserved_.heroAppearanceModifiers))
            {
                return LocalHeroRestoreResult::Pending;
            }
            // The retail component can reject refresh while a destination
            // Hero graph is still settling. Preserve every deterministic
            // owner field and avoid blocking the world on this optional
            // attachable layer; a later owner mutation can refresh it.
            modifiersDegraded_ = true;
            diagnostics.Event(
                "MultiplayerLocalHeroOwnedModifiersDegraded",
                "native destination modifier refresh unavailable; continuing with preserved clothing, morph, bone scales, and equipped weapons");
            return LocalHeroRestoreResult::Pending;
        }
        if (!morph.Equals(preserved_.heroMorph))
        {
            return game::hero_pawn::appearance::native::HeroMorphComponent::
                    ApplyValues(nativeHero, preserved_.heroMorph)
                ? LocalHeroRestoreResult::Pending
                : failed(3, "morph");
        }
        if (!boneScales.Equals(preserved_.heroBoneScales))
        {
            return game::hero_pawn::appearance::native::HeroMorphComponent::
                    ApplyBoneScaleState(
                        nativeHero,
                        preserved_.heroBoneScales)
                ? LocalHeroRestoreResult::Pending
                : failed(4, "bone-scales");
        }

        const auto& desiredEquipment = preserved_.heroEquipment;
        if (equipment.meleeDefinitionIndex !=
                desiredEquipment.meleeDefinitionIndex ||
            equipment.rangedDefinitionIndex !=
                desiredEquipment.rangedDefinitionIndex)
        {
            if (!definitionsRequested_ ||
                now - definitionsRequestedAt_ >= 2'000)
            {
                definitionsRequested_ = true;
                definitionsRequestedAt_ = now;
                const bool applied = game::hero_pawn::equipment::native::
                    HeroWeaponComponent::ApplyDefinitions(
                        entities, nativeHero, desiredEquipment);
                game::hero_pawn::equipment::native::HeroWeaponInspection
                    inspection;
                const bool inspected = game::hero_pawn::equipment::native::
                    HeroWeaponComponent::Inspect(nativeHero, inspection);
                char detail[256] = {};
                std::snprintf(
                    detail,
                    sizeof(detail),
                    "applied=%s inspected=%s readable=%s current=(%d,%d) requested=(%d,%d)",
                    applied ? "true" : "false",
                    inspected ? "true" : "false",
                    inspection.readable ? "true" : "false",
                    inspection.meleeDefinitionIndex,
                    inspection.rangedDefinitionIndex,
                    desiredEquipment.meleeDefinitionIndex,
                    desiredEquipment.rangedDefinitionIndex);
                diagnostics.Event(
                    "MultiplayerLocalHeroOwnedWeaponRestoreRequested",
                    detail);
            }
            return LocalHeroRestoreResult::Pending;
        }
        definitionsRequested_ = false;
        definitionsRequestedAt_ = 0;
        const bool nativeTransition = HasPendingNativeWeaponAction(nativeHero);
        if (!presentationRequested_)
        {
            // Local Heroes must enter the retail inventory/action path. Raw
            // carry-slot edits are only a projection: the next inventory
            // reconciliation can immediately remove those attached weapons.
            using game::creature::actions::CreatureActionLifecycleObserver;
            CreatureActionLifecycleObserver::BeginAuthoritativeReplay();
            const bool requested = game::hero_pawn::equipment::native::HeroWeaponComponent::RequestActiveFamily(
                entities.Interface(), hero.NativeHandle(), desiredEquipment.activeFamily);
            CreatureActionLifecycleObserver::EndAuthoritativeReplay();
            if (!requested) return failed(6, "native-weapon-presentation-request");
            presentationRequested_ = true;
            diagnostics.Event("MultiplayerLocalHeroNativeWeaponRestoreRequested", "retail draw/sheathe action requested; waiting for native completion");
            return LocalHeroRestoreResult::Pending;
        }
        if (nativeTransition)
            return LocalHeroRestoreResult::Pending;
        if (equipment.activeFamily != desiredEquipment.activeFamily)
            diagnostics.Event("MultiplayerLocalHeroWeaponRestoreNativePolicy", "retail selected a different weapon family; publishing actual state without forcing town/script restrictions");

        char detail[160] = {};
        std::snprintf(
            detail,
            sizeof(detail),
            "actor_id=%llu clothing-and-equipped-weapons-restored-after-destination-baseline",
            static_cast<unsigned long long>(preserved_.actorId));
        diagnostics.Event("MultiplayerLocalHeroOwnedStateRestored", detail);
        Clear();
        return LocalHeroRestoreResult::Ready;
    }

    bool LocalHeroOwnedStateRestorer::HasPendingState() const noexcept
    {
        return pending_;
    }

    const PlayerState* LocalHeroOwnedStateRestorer::PreservedState() const
        noexcept
    {
        return pending_ ? &preserved_ : nullptr;
    }

    void LocalHeroOwnedStateRestorer::Clear() noexcept
    {
        preserved_ = {};
        nextAttemptAt_ = 0;
        definitionsRequestedAt_ = 0;
        lastFailedStage_ = 0;
        pending_ = false;
        definitionsRequested_ = false;
        modifiersDegraded_ = false;
        presentationRequested_ = false;
    }
}
