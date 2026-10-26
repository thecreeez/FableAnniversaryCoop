#pragma once

#include <cstddef>
#include <cstdint>

namespace fable::multiplayer::protocol
{
    // One live CQuestManager lifecycle transition. The global snapshot only
    // covers joins and loads; these events keep already-connected peers on
    // the same quest state while everyone is in the world.
    enum class QuestEventOperation : std::uint8_t
    {
        Activate = 1,
        Deactivate = 2,
        Complete = 3,
        Fail = 4,
        // Present the quest card screen (start/completion) for the quest;
        // arguments[0] carries the card mode. Not a lifecycle transition.
        ShowQuestScreen = 5,
    };

    inline constexpr std::size_t MaximumQuestEventNameBytes = 96;
    inline constexpr std::size_t QuestEventFixedBytes = 1 + 8 + 4 * 3 + 1;

    struct QuestEventMessage final
    {
        QuestEventOperation operation = QuestEventOperation::Activate;
        // Originating peer's monotonically increasing event number. Hosts
        // rebroadcast with their own revision; receivers deduplicate by
        // (source actor, revision) and by the current native quest state.
        std::uint64_t revision = 0;
        // Raw scalar arguments the retail call received after the quest name.
        // Receivers replay them only when they are bool-like.
        std::uint32_t arguments[3] = {};
        std::uint8_t nameLength = 0;
        char name[MaximumQuestEventNameBytes] = {};
    };

    [[nodiscard]] bool IsValidQuestEventOperation(std::uint8_t value) noexcept;
    bool EncodeQuestEventMessage(
        const QuestEventMessage& message,
        std::uint8_t* destination,
        std::size_t destinationCapacity,
        std::size_t& encodedSize) noexcept;
    bool DecodeQuestEventMessage(
        const std::uint8_t* bytes,
        std::size_t byteCount,
        QuestEventMessage& message) noexcept;
}
