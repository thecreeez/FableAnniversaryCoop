#include "Multiplayer/Protocol/WeatherMessage.h"
#include "Multiplayer/Transport/PeerDatagramCodec.h"
#include "Multiplayer/Transport/ReliableStreamTransport.h"
#include <limits>

int RunWeatherTests()
{
    using namespace fable::multiplayer;
    protocol::WeatherMessage s;
    s.epoch = 9; s.revision = 3; s.sampledAt = 1000; s.worldFence = 7;
    s.actorGeneration = 2; s.mapEpoch = 4; s.map = 32; s.enabled = 5;
    s.rain = 0.75f; s.snow = 0.25f; s.mist = 0.5f;
    unsigned char bytes[protocol::WeatherBytes];
    protocol::WeatherMessage decoded;
    if (!protocol::EncodeWeather(s, bytes, sizeof(bytes)) || !protocol::DecodeWeather(bytes, sizeof(bytes), decoded) ||
        decoded.enabled != 5 || decoded.snow != 0.25f || decoded.worldFence != 7 || decoded.mapEpoch != 4 ||
        decoded.rain != 0.75f || decoded.mist != 0.5f || decoded.map != 32) return 1;
    if (!transport_codec::IsReliablePacketType(protocol::PacketType::Weather) ||
        !ReliableStreamTransport::IsMessageValid(reliable_stream::Control, protocol::PacketType::Weather, bytes, sizeof(bytes))) return 1;
    if (protocol::DecodeWeather(bytes, sizeof(bytes) - 1, decoded)) return 1;
    bytes[43] = 1;
    if (protocol::DecodeWeather(bytes, sizeof(bytes), decoded)) return 1;
    s.rain = std::numeric_limits<float>::quiet_NaN();
    if (protocol::EncodeWeather(s, bytes, sizeof(bytes))) return 1;
    s.rain = -1;
    if (protocol::EncodeWeather(s, bytes, sizeof(bytes))) return 1;
    s.rain = 0; s.enabled = 8;
    if (protocol::EncodeWeather(s, bytes, sizeof(bytes))) return 1;
    s.enabled = 0; s.actorGeneration = 0;
    if (protocol::EncodeWeather(s, bytes, sizeof(bytes))) return 1;
    return 0;
}
