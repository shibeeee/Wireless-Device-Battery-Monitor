#include "../src/drivers/sony_headphones_protocol.h"

#include <cassert>
#include <algorithm>
#include <cstdio>
#include <vector>

using Bytes = std::vector<std::uint8_t>;
using namespace SonyHeadphones;

// A pretend WH-1000XM6: ACKs every command with the next sequence number and answers the
// read-only queries. `thresholdOnly` firmware ACKs the plain battery query but never answers it.
struct FakeHeadphones {
    bool thresholdOnly = false;
    bool ignorePlain = false;     // drops the plain battery query entirely: no ACK, no reply
    bool replyBeforeAck = false;  // some links deliver the reply ahead of the ACK
    bool table2Noise = false;     // a table-2 notification arrives before the protocol info reply
    int level = 64;
    std::uint8_t status = 0;
    std::uint8_t mySeq = 0;
    std::vector<std::uint8_t> commandSeqs;  // seq of every command received, in order
    int acksReceived = 0;
    Bytes pending;

    Bytes Feed(const Bytes& in) {
        pending.insert(pending.end(), in.begin(), in.end());
        std::vector<Frame> frames;
        pending.erase(pending.begin(), pending.begin() + (std::ptrdiff_t)ParseFrames(pending.data(), pending.size(), frames));
        Bytes out;
        auto add = [&](const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); };
        for (const Frame& f : frames) {
            if (f.type == kTypeAck) { acksReceived++; continue; }
            commandSeqs.push_back(f.seq);
            if (ignorePlain && f.payload.size() == 2 && f.payload[0] == kPowerGetStatus && f.payload[1] == kBattery) continue;
            Bytes ack = Ack(f.seq), reply;
            const Bytes& p = f.payload;
            if (p.size() == 2 && p[0] == kProtocolInfoGet) {
                if (table2Noise) add(Encode(kTypeCommand2, mySeq, { 0x29, 0x01 })), mySeq = (std::uint8_t)(1 - mySeq);
                reply = Encode(kTypeCommand, mySeq, { 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 });
            } else if (p.size() == 2 && p[0] == kPowerGetStatus && p[1] == kBattery && !thresholdOnly) {
                reply = Encode(kTypeCommand, mySeq, { kPowerRetStatus, kBattery, (std::uint8_t)level, status });
            } else if (p.size() == 2 && p[0] == kPowerGetStatus && p[1] == kBatteryWithThreshold) {
                reply = Encode(kTypeCommand, mySeq, { kPowerRetStatus, kBatteryWithThreshold, (std::uint8_t)level, status, 20 });
            }
            if (!reply.empty()) mySeq = (std::uint8_t)(1 - mySeq);
            if (replyBeforeAck) { add(reply); add(ack); }
            else { add(ack); add(reply); }
        }
        return out;
    }
};

// Drives a Session against the fake the way the driver does, delivering bytes `chunk` at a time.
// Returns false if it did not finish within `budgetMs` of simulated time.
static bool Converse(FakeHeadphones& hp, Battery& result, std::size_t chunk = 512, std::uint64_t budgetMs = 1500) {
    Session session;
    Bytes toHp = session.Begin();
    for (std::uint64_t now = 0; now < budgetMs && !session.Done(); now += 50) {
        Bytes q = session.Poll(now);
        toHp.insert(toHp.end(), q.begin(), q.end());
        Bytes fromHp = hp.Feed(toHp);
        toHp.clear();
        for (std::size_t i = 0; i < fromHp.size(); i += chunk) {
            std::size_t n = std::min(chunk, fromHp.size() - i);
            Bytes acks = session.Receive(fromHp.data() + i, n);
            toHp.insert(toHp.end(), acks.begin(), acks.end());
        }
        hp.Feed(toHp);  // our ACKs
        toHp.clear();
    }
    if (!session.Done()) return false;
    result = session.Result();
    return true;
}

static bool Alternates(const std::vector<std::uint8_t>& seqs) {
    for (std::size_t i = 0; i < seqs.size(); ++i)
        if (seqs[i] != (i % 2)) return false;
    return true;
}

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

    // Both MDR tables are ACKed, an ACK is not.
    Frame f2;
    f2.type = kTypeCommand2;
    assert(NeedsAck(f2) && NeedsAck(frames[0]));
    f2.type = kTypeAck;
    assert(!NeedsAck(f2));

    // Protocol info reply is recognised.
    Bytes info = Encode(kTypeCommand, 0, { 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 });
    frames.clear();
    ParseFrames(info.data(), info.size(), frames);
    assert(frames.size() == 1 && IsProtocolInfoReply(frames[0]));

    // Whole conversations against the pretend headphones.
    {
        FakeHeadphones hp;
        hp.status = 1;
        Battery r;
        assert(Converse(hp, r));
        assert(r.level == 64 && r.charging && !r.full);
        assert(hp.commandSeqs.size() == 2 && Alternates(hp.commandSeqs));  // info, battery
        assert(hp.acksReceived == 2);                                     // both replies ACKed
    }
    {   // Firmware that only answers the "with threshold" query: the fallback is sent after the retry
        // delay, with the next sequence number even though the first query got no reply.
        FakeHeadphones hp;
        hp.thresholdOnly = true;
        hp.level = 35;
        hp.status = 3;
        Battery r;
        assert(Converse(hp, r));
        assert(r.level == 35 && !r.charging && r.full);
        assert(hp.commandSeqs.size() == 3 && Alternates(hp.commandSeqs));
    }
    {   // The plain query is dropped without an ACK: the fallback still gets a fresh sequence number.
        FakeHeadphones hp;
        hp.ignorePlain = true;
        hp.level = 50;
        Battery r;
        assert(Converse(hp, r));
        assert(r.level == 50);
        assert(hp.commandSeqs.size() == 3 && Alternates(hp.commandSeqs));
    }
    {   // The fallback waits for the retry delay, so a short budget gives up cleanly.
        FakeHeadphones hp;
        hp.thresholdOnly = true;
        Battery r;
        assert(!Converse(hp, r, 512, Session::kRetryMs - 100));
    }
    {   // Reply ahead of the ACK, a table-2 notification first, and one byte per read.
        FakeHeadphones hp;
        hp.replyBeforeAck = true;
        hp.table2Noise = true;
        hp.level = 9;
        Battery r;
        assert(Converse(hp, r, 1));
        assert(r.level == 9 && !r.charging);
        assert(Alternates(hp.commandSeqs));
        assert(hp.acksReceived == 3);  // the notification is ACKed too
    }
    {   // Headphones that never answer: no battery query is sent and nothing is reported.
        Session session;
        session.Begin();
        assert(session.Poll(0).empty() && session.Poll(5000).empty() && !session.Done());
    }

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
