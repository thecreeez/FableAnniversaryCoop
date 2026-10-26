#include "QuestEventMessage.h"

#include <cstring>
#include <type_traits>

namespace
{
#pragma pack(push, 1)
    struct WireQuestEventHeader final
    {
        std::uint8_t operation = 0;
        std::uint64_t revision = 0;
        std::uint32_t arguments[3] = {};
        std::uint8_t nameLength = 0;
    };
#pragma pack(pop)

    static_assert(std::is_trivially_copyable_v<WireQuestEventHeader>);
    static_assert(sizeof(WireQuestEventHeader) ==
        fable::multiplayer::protocol::QuestEventFixedBytes);

    // Retail quest identifiers are ASCII symbols such as Q_BanditCamp.
    bool IsQuestNameByte(const char value) noexcept
    {
        return (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') || value == '_';
    }

    bool IsSane(
        const fable::multiplayer::protocol::QuestEventMessage& message)
        noexcept
    {
        using namespace fable::multiplayer::protocol;
        if (!IsValidQuestEventOperation(
                static_cast<std::uint8_t>(message.operation)) ||
            message.revision == 0 || message.nameLength == 0 ||
            message.nameLength >= MaximumQuestEventNameBytes)
        {
            return false;
        }
        for (std::size_t index = 0; index < message.nameLength; ++index)
        {
            if (!IsQuestNameByte(message.name[index]))
            {
                return false;
            }
        }
        return true;
    }
}

namespace fable::multiplayer::protocol
{
    bool IsValidQuestEventOperation(const std::uint8_t value) noexcept
    {
        return value >= static_cast<std::uint8_t>(QuestEventOperation::Activate) &&
            value <= static_cast<std::uint8_t>(QuestEventOperation::ShowQuestScreen);
    }

    bool EncodeQuestEventMessage(
        const QuestEventMessage& message,
        std::uint8_t* destination,
        const std::size_t destinationCapacity,
        std::size_t& encodedSize) noexcept
    {
        encodedSize = 0;
        const std::size_t totalSize =
            sizeof(WireQuestEventHeader) + message.nameLength;
        if (!IsSane(message) || destination == nullptr ||
            destinationCapacity < totalSize)
        {
            return false;
        }
        WireQuestEventHeader wire;
        wire.operation = static_cast<std::uint8_t>(message.operation);
        wire.revision = message.revision;
        std::memcpy(wire.arguments, message.arguments, sizeof(wire.arguments));
        wire.nameLength = message.nameLength;
        std::memcpy(destination, &wire, sizeof(wire));
        std::memcpy(destination + sizeof(wire), message.name, message.nameLength);
        encodedSize = totalSize;
        return true;
    }

    bool DecodeQuestEventMessage(
        const std::uint8_t* bytes,
        const std::size_t byteCount,
        QuestEventMessage& message) noexcept
    {
        message = {};
        if (bytes == nullptr || byteCount < sizeof(WireQuestEventHeader))
        {
            return false;
        }
        WireQuestEventHeader wire;
        std::memcpy(&wire, bytes, sizeof(wire));
        if (byteCount != sizeof(wire) + wire.nameLength ||
            wire.nameLength >= MaximumQuestEventNameBytes)
        {
            return false;
        }
        message.operation = static_cast<QuestEventOperation>(wire.operation);
        message.revision = wire.revision;
        std::memcpy(message.arguments, wire.arguments, sizeof(wire.arguments));
        message.nameLength = wire.nameLength;
        std::memcpy(message.name, bytes + sizeof(wire), wire.nameLength);
        message.name[wire.nameLength] = '\0';
        if (!IsSane(message))
        {
            message = {};
            return false;
        }
        return true;
    }
}
