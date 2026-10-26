#pragma once

#include <cstddef>
#include <cstdint>

namespace fable::multiplayer::protocol
{
    // One changed quest-script progress variable (a named CPersistContext
    // field such as Q_AwakeningTheOracle.TabletsCollected).
    inline constexpr std::size_t MaximumQuestFieldNameBytes = 64;
    inline constexpr std::size_t QuestFieldFixedBytes = 8 + 1 + 4 + 1 + 1;

    struct QuestFieldMessage final
    {
        std::uint64_t revision = 0;
        // QuestFieldRecorder::Kind: 1 bool, 2 int32, 3 float32.
        std::uint8_t kind = 0;
        std::uint32_t bits = 0;
        std::uint8_t questLength = 0;
        std::uint8_t fieldLength = 0;
        char quest[MaximumQuestFieldNameBytes] = {};
        char field[MaximumQuestFieldNameBytes] = {};
    };

    bool EncodeQuestFieldMessage(
        const QuestFieldMessage& message,
        std::uint8_t* destination,
        std::size_t destinationCapacity,
        std::size_t& encodedSize) noexcept;
    bool DecodeQuestFieldMessage(
        const std::uint8_t* bytes,
        std::size_t byteCount,
        QuestFieldMessage& message) noexcept;
}
