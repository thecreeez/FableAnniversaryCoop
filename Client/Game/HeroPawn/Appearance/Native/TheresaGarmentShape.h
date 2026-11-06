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
        struct LowerBodyProfile final
        {
            float pelvisWidth=1,pelvisDepth=1,hipSpacing=1,thighWidth=1,thighDepth=1;
        };
        struct ShoulderProfile final
        {
            float clavicleLength=1,upperArmScale=1;
        };
        static bool Initialize(HMODULE module, const core::Diagnostics& diagnostics) noexcept;
        static void Shutdown() noexcept;
        // Call only for an adult Theresa's live apparel on the native graphics
        // command thread. Unknown assets/layouts are left unchanged.
        static void Apply(void* component) noexcept;
        // Enable the native depth profile only while a weighted garment is live.
        static float BoneDepthScale(void* master) noexcept;
        static LowerBodyProfile LowerBodyScales() noexcept;
        static ShoulderProfile ShoulderScales() noexcept;
        static float WaistScale() noexcept;
        static float TorsoScale() noexcept;
        // Game-thread held-key polling; returns true when the live size changed.
        static bool TickSizeHotkeys() noexcept;
        // Window-thread input reservation; reads only atomic activation state.
        static bool CaptureSizeKey(unsigned int message,unsigned int key) noexcept;
    };
}
