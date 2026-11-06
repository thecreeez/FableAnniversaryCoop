#pragma once
#include "HeroClothingState.h"
#include <cstring>

namespace fable::game::hero_pawn::appearance
{
    inline bool TheresaHeroFacialMesh(const char* name) noexcept
    {
        return name && std::strstr(name,"MESH_HERO_") &&
            (std::strstr(name,"_BEARD") || std::strstr(name,"_MOUSTACHE") ||
             std::strstr(name,"_MUSTACHE") || std::strstr(name,"_HAIR") || std::strstr(name,"_SIDEBURN"));
    }
    inline bool TheresaOpenTorsoGarment(const char* name) noexcept
    {
        return name && (std::strstr(name,"_DRESS") || std::strstr(name,"_LADY_GREY") ||
            std::strstr(name,"_WHISPER"));
    }
    inline unsigned TheresaCoverageForMesh(const char* name) noexcept
    {
        if (!name) return 0;
        unsigned coverage=0;
        if (std::strstr(name,"_SHIRT") || std::strstr(name,"_TUNIC") || std::strstr(name,"_ROBE")) coverage|=1;
        if (std::strstr(name,"_TROUSER") || std::strstr(name,"_PANTS") || std::strstr(name,"_LEGS")) coverage|=2;
        if (std::strstr(name,"_DRESS")) coverage|=3;
        if (std::strstr(name,"_GLOVE")) coverage|=4;
        if (std::strstr(name,"_BOOT")) coverage|=8;
        if (std::strstr(name,"_HEAD") || std::strstr(name,"_HAT") || std::strstr(name,"_HELM") || std::strstr(name,"_MASK")) coverage|=16;
        return coverage;
    }
    // Native NO_* and HERO_SUIT_NAKED definitions are positive inventory IDs,
    // not visible garments. Resolve their IDs from this executable's catalog.
    inline unsigned TheresaOutfitCoverage(const HeroClothingState& clothing,
        const std::array<int,6>& empty) noexcept
    {
        if (!clothing.IsSane()) return 0;
        const auto worn=[&](int slot) { const int id=clothing.definitionIndices[slot];
            return id>0 && id!=empty[slot]; };
        const bool suit=worn(5);
        return (suit || worn(1)?1u:0u) | (suit || worn(3)?2u:0u) |
            (suit || worn(2)?4u:0u) | (suit || worn(4)?8u:0u) | (worn(0)?16u:0u);
    }
    inline bool TheresaBodyMaterialVisible(int material,unsigned coverage) noexcept
    {
        if (material<0 || material>=16) return false; // Hero face/mouth never.
        // Empty slots use Sister's authored outfit, never naked Hero geometry.
        if (material==6 || material==8 || material==10 || material==11) return (coverage&1u)!=0;
        if (material==9 || material==12) return (coverage&1u) && !(coverage&4u);
        if (material==5 || material==15) return (coverage&2u) && !(coverage&8u);
        // Authored female garments can expose front/back/shoulder skin.
        // They need the underlying torso, not Sister's opaque stock dress.
        if ((coverage&65u)==65u && (material==0 || material==2 || material==7)) return true;
        // Bandit shirt leaves chest/shoulder skin exposed (bit32).
        if (material==0 || material==7) return (coverage&33u)==33u;
        return false;
    }
    inline bool TheresaHiddenForScene(bool scene,bool local) noexcept
    { return scene && !local; }
}
