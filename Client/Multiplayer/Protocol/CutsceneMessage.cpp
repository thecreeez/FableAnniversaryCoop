#include "CutsceneMessage.h"

#include <cstring>

namespace
{
    // Retail cutscene identifiers: CS_GUILD_ARRIVE, CS_WASPBOSS_INTRO, ...
    bool IsSane(const fable::multiplayer::protocol::CutsceneMessage& message) noexcept
    {
        if (message.revision == 0 || message.nameLength == 0 ||
            message.nameLength >= fable::multiplayer::protocol::MaximumCutsceneNameBytes)
        {
            return false;
        }
        for (std::size_t index = 0; index < message.nameLength; ++index)
        {
            const char value = message.name[index];
            if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                    (value >= '0' && value <= '9') || value == '_'))
            {
                return false;
            }
        }
        return true;
    }
}

namespace fable::multiplayer::protocol
{
    bool EncodeCutsceneMessage(
        const CutsceneMessage& message,
        std::uint8_t* destination,
        const std::size_t destinationCapacity,
        std::size_t& encodedSize) noexcept
    {
        encodedSize = 0;
        const std::size_t total = CutsceneFixedBytes + message.nameLength;
        if (!IsSane(message) || destination == nullptr || destinationCapacity < total) return false;
        std::memcpy(destination, &message.revision, sizeof(message.revision));
        std::memcpy(destination + 8, &message.mapId, sizeof(message.mapId));
        destination[10] = message.nameLength;
        destination[11] = message.skip ? 1 : 0;
        std::memcpy(destination + CutsceneFixedBytes, message.name, message.nameLength);
        encodedSize = total;
        return true;
    }

    bool DecodeCutsceneMessage(
        const std::uint8_t* bytes,
        const std::size_t byteCount,
        CutsceneMessage& message) noexcept
    {
        message = {};
        if (bytes == nullptr || byteCount < CutsceneFixedBytes) return false;
        std::memcpy(&message.revision, bytes, sizeof(message.revision));
        std::memcpy(&message.mapId, bytes + 8, sizeof(message.mapId));
        message.nameLength = bytes[10];
        if (bytes[11] > 1) return false;
        message.skip = bytes[11] != 0;
        if (message.nameLength >= MaximumCutsceneNameBytes ||
            byteCount != CutsceneFixedBytes + message.nameLength)
        {
            message = {};
            return false;
        }
        std::memcpy(message.name, bytes + CutsceneFixedBytes, message.nameLength);
        if (!IsSane(message))
        {
            message = {};
            return false;
        }
        return true;
    }
}
