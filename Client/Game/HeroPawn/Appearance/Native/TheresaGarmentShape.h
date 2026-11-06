#pragma once

#include "Core/Diagnostics/Diagnostics.h"
#include <Windows.h>

namespace fable::game::hero_pawn::appearance::native
{
    // Component-private skeletal mesh copies use ordinary shaders. Only copied
    // vertex geometry is edited; retail meshes, materials and UVs stay intact.
    class TheresaGarmentShape final
    {
    public:
        static bool Initialize(HMODULE module, const core::Diagnostics& diagnostics) noexcept;
        static void Shutdown() noexcept;
        // Call only for an adult Theresa's live apparel on the native graphics
        // command thread. Unknown assets/layouts are left unchanged.
        static void Apply(void* component) noexcept;
        // Enable the native depth profile only while a weighted garment is live.
        static float BoneDepthScale(void* master) noexcept;
    };
}
