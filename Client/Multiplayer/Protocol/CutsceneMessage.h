#pragma once

#include <cstddef>
#include <cstdint>

namespace fable::multiplayer::protocol
{
    // "Play this retail cutscene now": sent once every player has gathered.
    inline constexpr std::size_t MaximumCutsceneNameBytes = 64;
    inline constexpr std::size_t CutsceneFixedBytes = 8 + 2 + 1;

    struct CutsceneMessage final
    {
        std::uint64_t revision = 0;
        // Numeric map of the initiator; a peer elsewhere does not play it.
        std::uint16_t mapId = 0;
        std::uint8_t nameLength = 0;
        char name[MaximumCutsceneNameBytes] = {};
    };

    bool EncodeCutsceneMessage(
        const CutsceneMessage& message,
        std::uint8_t* destination,
        std::size_t destinationCapacity,
        std::size_t& encodedSize) noexcept;
    bool DecodeCutsceneMessage(
        const std::uint8_t* bytes,
        std::size_t byteCount,
        CutsceneMessage& message) noexcept;
}
