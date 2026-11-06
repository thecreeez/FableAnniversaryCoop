#pragma once
#include "Core/Diagnostics/Diagnostics.h"
#include "Game/HeroPawn/Appearance/HeroClothingState.h"
#include "Game/HeroPawn/Appearance/HeroMorphState.h"
#include "Game/Math/Vector3.h"
#include <cstdint>
namespace fable::multiplayer { class MultiplayerRuntimeGraph; }
namespace fable::game { class EntityService; }
namespace fable::automation::multiplayer::features
{
    class TheresaAcceptanceDriver final
    {
    public:
        void Initialize(bool enabled, bool host, game::EntityService& entities,
            ::fable::multiplayer::MultiplayerRuntimeGraph& multiplayer,
            const core::Diagnostics& diagnostics) noexcept;
        bool ProcessGameThreadIdle() noexcept;
        void Shutdown() noexcept;
    private:
        void Fail(const char* reason) noexcept;
        ::fable::multiplayer::MultiplayerRuntimeGraph* multiplayer_ = nullptr;
        game::EntityService* entities_ = nullptr;
        core::Diagnostics diagnostics_;
        game::hero_pawn::appearance::HeroClothingState baseline_;
        game::hero_pawn::appearance::HeroClothingState changed_;
        game::hero_pawn::appearance::HeroBoneScaleState originalScales_;
        bool growthStress_ = false, growthVerified_ = false;
        game::Vector3 savedPosition_;
        std::uint64_t started_ = 0, next_ = 0, phaseAt_ = 0, revision_ = 0;
        unsigned phase_ = 0;
        bool host_ = false, checkpoint_ = false;
    };
}
