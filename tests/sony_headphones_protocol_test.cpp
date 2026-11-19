#include "../src/drivers/sony_headphones_protocol.h"

#include <cassert>
#include <cstdio>
#include <vector>

using Bytes = std::vector<std::uint8_t>;
using namespace SonyHeadphones;

int main() {
    // Protocol info query, seq 0: 0C 00 00000002 00 00, checksum 0x0E.
    assert(ProtocolInfoQuery(0) == (Bytes{ 0x3E, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x0E, 0x3C }));

    // Battery query, seq 1, type 00: checksum 0x0C + 1 + 2 + 0x22 = 0x31.
    assert(BatteryQuery(1, kBattery) == (Bytes{ 0x3E, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x02, 0x22, 0x00, 0x31, 0x3C }));

    // ACK for a command received with seq 0 carries seq 1: checksum 0x02.
    assert(Ack(0) == (Bytes{ 0x3E, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x3C }));

    // A checksum of 0x3C (the end marker) must be escaped as 3D 2C, and must survive a round trip.
    Bytes escaped = Encode(kTypeCommand, 0, { 0x22, 0x0C });  // 0x0C + 2 + 0x22 + 0x0C = 0x3C
    assert((escaped == Bytes{ 0x3E, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x22, 0x0C, 0x3D, 0x2C, 0x3C }));
    std::vector<Frame> frames;
    assert(ParseFrames(escaped.data(), escaped.size(), frames) == escaped.size());
    assert(frames.size() == 1 && frames[0].payload == (Bytes{ 0x22, 0x0C }));

    // Headphones' stream: ACK, then a battery reply (72%, charging), then half of the next frame.
    Bytes stream = { 0x00, 0x3E, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x3C };
    Bytes reply = { 0x3E, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x04, 0x23, 0x00, 72, 0x01, 0x00, 0x3C };
    std::uint8_t sum = 0;
    for (std::size_t k = 1; k + 2 < reply.size(); ++k) sum = (std::uint8_t)(sum + reply[k]);
    reply[reply.size() - 2] = sum;
    stream.insert(stream.end(), reply.begin(), reply.end());
    Bytes partial = { 0x3E, 0x0C, 0x00, 0x00 };
    stream.insert(stream.end(), partial.begin(), partial.end());
    frames.clear();
    std::size_t used = ParseFrames(stream.data(), stream.size(), frames);
    assert(used == stream.size() - partial.size());
    assert(frames.size() == 2);
    assert(frames[0].type == kTypeAck && frames[0].seq == 1 && frames[0].payload.empty());
    Battery b;
    assert(!ParseBattery(frames[0], b));
    assert(ParseBattery(frames[1], b));
    assert(b.level == 72 && b.charging && !b.full);

    // Notification of the "with threshold" battery: 100%, charged.
    Bytes ntfy = Encode(kTypeCommand, 0, { kPowerNotifyStatus, kBatteryWithThreshold, 100, 0x03, 30 });
    frames.clear();
    ParseFrames(ntfy.data(), ntfy.size(), frames);
    assert(frames.size() == 1);
    b = {};
    assert(ParseBattery(frames[0], b));
    assert(b.level == 100 && !b.charging && b.full);

    // Left/right (earbuds) and out-of-range levels are not a single headphone battery.
    for (const Bytes& p : { Bytes{ 0x23, 0x01, 50, 0x00, 60, 0x00 }, Bytes{ 0x23, 0x00, 101, 0x00 } }) {
        Bytes f = Encode(kTypeCommand, 0, p);
        frames.clear();
        ParseFrames(f.data(), f.size(), frames);
        assert(frames.size() == 1 && !ParseBattery(frames[0], b));
    }

    // Protocol info reply is recognised.
    Bytes info = Encode(kTypeCommand, 0, { 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 });
    frames.clear();
    ParseFrames(info.data(), info.size(), frames);
    assert(frames.size() == 1 && IsProtocolInfoReply(frames[0]));

    // A bad checksum or a wrong length is dropped.
    Bytes bad = reply;
    bad[bad.size() - 2] ^= 0x01;
    frames.clear();
    assert(ParseFrames(bad.data(), bad.size(), frames) == bad.size() && frames.empty());
    Bytes badLen = { 0x3E, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x05, 0x23, 0x00, 0x34, 0x3C };
    frames.clear();
    ParseFrames(badLen.data(), badLen.size(), frames);
    assert(frames.empty());

    std::printf("sony_headphones_protocol_test: all assertions passed\n");
    return 0;
}
