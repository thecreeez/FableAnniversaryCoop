#pragma once

#include "Game/HeroPawn/Appearance/HeroClothingState.h"

#include <cstdint>

namespace fable::game::hero_pawn::appearance::native
{
    struct HeroClothingCatalog final
    {
        std::array<std::int32_t, HeroClothingState::SlotCount*128> definitions{};
        std::size_t count=0;
    };

    class HeroClothingComponent final
    {
    public:
        [[nodiscard]] static bool Capture(
            void* nativeThing,
            HeroClothingState& state) noexcept;

        // Catalog membership is stable while a menu wear transaction is
        // publishing the selected slot; native modifier enabled flags decide
        // which owned garments are currently being drawn.
        [[nodiscard]] static bool CaptureCatalog(
            void* nativeThing, HeroClothingCatalog& catalog) noexcept;

        // Read enabled modifiers of the menu's data clone, including hover
        // selection. The real Hero's equipped snapshot can still be older.
        [[nodiscard]] static bool CapturePreview(
            void* nativeThing, HeroClothingState& state) noexcept;

        // Re-publish the already equipped clothing modifiers after a world
        // transition. Does not wear/unwear items or alter inventory entries.
        [[nodiscard]] static bool RefreshEquipped(void* nativeThing) noexcept;

        // Makes the proxy's actor-local clothing catalog aware of the selected
        // visual definitions, then uses CTCInventoryClothing's own wear and
        // appearance-rebuild lifecycle. The player's real inventory is never
        // modified and no renderer-owned buffers are copied.
        [[nodiscard]] static bool Apply(
            void* nativeThing,
            const HeroClothingState& state,
            std::uint32_t* insertedCount = nullptr) noexcept;
    };
}
