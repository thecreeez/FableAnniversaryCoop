#include "QuestFieldMessage.h"

#include <cstring>
#include <type_traits>

namespace
{
#pragma pack(push, 1)
    struct WireQuestFieldHeader final
    {
        std::uint64_t revision = 0;
        std::uint8_t kind = 0;
        std::uint32_t bits = 0;
        std::uint8_t questLength = 0;
        std::uint8_t fieldLength = 0;
    };
#pragma pack(pop)

    static_assert(std::is_trivially_copyable_v<WireQuestFieldHeader>);
    static_assert(sizeof(WireQuestFieldHeader) ==
        fable::multiplayer::protocol::QuestFieldFixedBytes);

    bool IsSymbol(const char* text, const std::size_t length) noexcept
    {
        if (length == 0 || length >= fable::multiplayer::protocol::MaximumQuestFieldNameBytes)
        {
            return false;
        }
        for (std::size_t index = 0; index < length; ++index)
        {
            const char value = text[index];
            if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                    (value >= '0' && value <= '9') || value == '_'))
            {
                return false;
            }
        }
        return true;
    }

    bool IsSane(const fable::multiplayer::protocol::QuestFieldMessage& message) noexcept
    {
        return message.revision != 0 && message.kind >= 1 && message.kind <= 3 &&
            (message.kind != 1 || message.bits <= 1) &&
            IsSymbol(message.quest, message.questLength) &&
            IsSymbol(message.field, message.fieldLength);
    }
}

namespace fable::multiplayer::protocol
{
    bool EncodeQuestFieldMessage(
        const QuestFieldMessage& message,
        std::uint8_t* destination,
        const std::size_t destinationCapacity,
        std::size_t& encodedSize) noexcept
    {
        encodedSize = 0;
        const std::size_t total = sizeof(WireQuestFieldHeader) +
            message.questLength + message.fieldLength;
        if (!IsSane(message) || destination == nullptr || destinationCapacity < total)
        {
            return false;
        }
        WireQuestFieldHeader wire;
        wire.revision = message.revision;
        wire.kind = message.kind;
        wire.bits = message.bits;
        wire.questLength = message.questLength;
        wire.fieldLength = message.fieldLength;
        std::memcpy(destination, &wire, sizeof(wire));
        std::memcpy(destination + sizeof(wire), message.quest, message.questLength);
        std::memcpy(destination + sizeof(wire) + message.questLength,
            message.field, message.fieldLength);
        encodedSize = total;
        return true;
    }

    bool DecodeQuestFieldMessage(
        const std::uint8_t* bytes,
        const std::size_t byteCount,
        QuestFieldMessage& message) noexcept
    {
        message = {};
        if (bytes == nullptr || byteCount < sizeof(WireQuestFieldHeader)) return false;
        WireQuestFieldHeader wire;
        std::memcpy(&wire, bytes, sizeof(wire));
        if (wire.questLength >= MaximumQuestFieldNameBytes ||
            wire.fieldLength >= MaximumQuestFieldNameBytes ||
            byteCount != sizeof(wire) + wire.questLength + wire.fieldLength)
        {
            return false;
        }
        message.revision = wire.revision;
        message.kind = wire.kind;
        message.bits = wire.bits;
        message.questLength = wire.questLength;
        message.fieldLength = wire.fieldLength;
        std::memcpy(message.quest, bytes + sizeof(wire), wire.questLength);
        std::memcpy(message.field, bytes + sizeof(wire) + wire.questLength, wire.fieldLength);
        if (!IsSane(message))
        {
            message = {};
            return false;
        }
        return true;
    }
}
