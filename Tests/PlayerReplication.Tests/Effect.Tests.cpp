#include "Multiplayer/Protocol/EffectMessage.h"
#include "Multiplayer/Transport/PeerDatagramCodec.h"
#include "Multiplayer/Transport/ReliableStreamTransport.h"
#include <cstring>
#include <limits>

int RunEffectTests()
{
    using namespace fable::multiplayer;
    protocol::EffectMessage s;
    s.actor = 9; s.instance = 11; s.revision = 1; s.authorityEpoch = 3;
    s.generation = 2; s.mapEpoch = 4; s.map = 32; s.worldFence = 7;
    s.particle = 40; s.slot = -1; s.extra = 1.0f;
    std::memcpy(s.bone, "HAND_RIGHT", sizeof("HAND_RIGHT"));
    const auto decode = [](const protocol::EffectMessage& source, protocol::EffectMessage& result)
    { return protocol::DecodeEffectMessage(reinterpret_cast<const std::uint8_t*>(&source), sizeof(source), result); };
    protocol::EffectMessage decoded;
    if (!decode(s, decoded) || decoded.extra != 1.0f || decoded.instance != 11 ||
        decoded.worldFence != 7 || std::strcmp(decoded.bone, "HAND_RIGHT")) return 1;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&s);
    if (!transport_codec::IsReliablePacketType(protocol::PacketType::Effect) ||
        !ReliableStreamTransport::IsMessageValid(reliable_stream::Control, protocol::PacketType::Effect, bytes, sizeof(s))) return 1;
    if (protocol::DecodeEffectMessage(bytes, sizeof(s) - 1, decoded) ||
        protocol::DecodeEffectMessage(nullptr, sizeof(s), decoded)) return 1;
    auto bad = s; bad.position.x = std::numeric_limits<float>::quiet_NaN();
    if (decode(bad, decoded)) return 1;
    bad = s; bad.extra = std::numeric_limits<float>::infinity();
    if (decode(bad, decoded)) return 1;
    bad = s; std::memset(bad.bone, 'x', sizeof(bad.bone));
    if (decode(bad, decoded)) return 1;
    bad = s; bad.generation = 0;
    if (decode(bad, decoded)) return 1;
    bad = s; bad.operation = static_cast<protocol::EffectOperation>(255);
    if (decode(bad, decoded)) return 1;
    bad = s; bad.attachment = protocol::EffectAttachment::World;
    if (decode(bad, decoded)) return 1;
    s.operation = protocol::EffectOperation::Stop; s.revision = 2;
    if (!decode(s, decoded) || decoded.operation != protocol::EffectOperation::Stop) return 1;
    s.operation = protocol::EffectOperation::Impact; s.attachment = protocol::EffectAttachment::World;
    s.kind = 1; s.secondary = 42; s.strength = 0.5f;
    if (!decode(s, decoded) || decoded.kind != 1 || decoded.secondary != 42 || decoded.strength != 0.5f) return 1;
    s.strength = std::numeric_limits<float>::quiet_NaN();
    if (decode(s, decoded)) return 1;
    return 0;
}
