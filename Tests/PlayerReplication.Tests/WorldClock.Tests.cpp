#include "Multiplayer/Protocol/WorldClockMessage.h"
#include "Multiplayer/Transport/PeerDatagramCodec.h"
#include "Multiplayer/Transport/ReliableStreamTransport.h"
#include <cstring>
#include <limits>

int RunWorldClockTests()
{
    using namespace fable::multiplayer::protocol;
    WorldClockMessage s;
    s.epoch = 7; s.revision = 19; s.sampledAt = 2000;
    s.day = 42; s.fraction = 0.999f; s.previous = 0.998f; s.rate = 0.02f;
    s.accumulated = 0.5f; s.target = 0.1f; s.remaining = 3; s.jumping = 1; s.frozen = 1;
    unsigned char bytes[WorldClockBytes];
    WorldClockMessage decoded;
    if (!EncodeWorldClock(s, bytes, sizeof(bytes)) || !DecodeWorldClock(bytes, sizeof(bytes), decoded) ||
        decoded.day != 42 || decoded.fraction != s.fraction || decoded.frozen != 1 ||
        decoded.target != 0.1f || decoded.remaining != 3 || decoded.epoch != 7) return 1;
    if (!fable::multiplayer::transport_codec::IsReliablePacketType(PacketType::WorldClock) ||
        !fable::multiplayer::ReliableStreamTransport::IsMessageValid(
            fable::multiplayer::reliable_stream::Control, PacketType::WorldClock, bytes, sizeof(bytes))) return 1;
    if (DecodeWorldClock(bytes, sizeof(bytes) - 1, decoded) ||
        EncodeWorldClock(s, bytes, sizeof(bytes) - 1)) return 1;
    if (!EncodeWorldClock(s, bytes, sizeof(bytes))) return 1;
    bytes[WorldClockBytes - 1] = 4;
    if (DecodeWorldClock(bytes, sizeof(bytes), decoded)) return 1;
    s.fraction = std::numeric_limits<float>::quiet_NaN();
    if (EncodeWorldClock(s, bytes, sizeof(bytes))) return 1;
    s.fraction = 0.5f; s.revision = 0;
    if (EncodeWorldClock(s, bytes, sizeof(bytes))) return 1;
    s.revision = 1; s.day = -1;
    if (EncodeWorldClock(s, bytes, sizeof(bytes))) return 1;
    s.day = 1; s.remaining = std::numeric_limits<float>::infinity();
    if (EncodeWorldClock(s, bytes, sizeof(bytes))) return 1;
    return 0;
}
