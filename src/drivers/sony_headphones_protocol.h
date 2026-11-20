#pragma once
// Sony "MDR" frames on the Bluetooth RFCOMM channel of the WH-1000XM6 (protocol version 2).
// Kept independent of Win32 so captured frames can be unit-tested without the headphones.
//
// Frame:  3E [escaped: type, seq, len (4 bytes, big endian), payload..., checksum] 3C
//   checksum = 8-bit sum of type, seq, len and payload
//   escape   = 3C / 3D / 3E inside the frame are sent as 3D followed by (byte & 0xEF)
//   type     = 0x01 ACK (no payload), 0x0C MDR command / reply, 0x0E second MDR table (notifications)
//   ACK      = sent for every received command, with seq = 1 - (that command's seq); the
//              headphones' ACK carries the seq to use for the next command.
// Read-only commands used here:
//   22 00 / 22 08   ask for the battery (plain, or "with threshold" on newer firmware)
//   00 00           ask for the protocol info (the official app's first command)
// Battery reply:  23 (reply) or 25 (notification), type 00 / 08, level 0..100, status
//   status: 0 not charging, 1 charging, 2 unknown, 3 charged
// Sources: mos9527/SonyHeadphonesClient (libmdr, ProtocolV2T1.hpp; WH-1000XM6 listed as supported)
// and Gadgetbridge (service/devices/sony/headphones, Message.java and SonyProtocolImplV2.java).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace SonyHeadphones {

constexpr std::uint8_t kStart = 0x3E;
constexpr std::uint8_t kEnd = 0x3C;
constexpr std::uint8_t kEscape = 0x3D;
constexpr std::uint8_t kEscapeMask = 0xEF;

constexpr std::uint8_t kTypeAck = 0x01;
constexpr std::uint8_t kTypeCommand = 0x0C;
constexpr std::uint8_t kTypeCommand2 = 0x0E;

constexpr std::uint8_t kProtocolInfoGet = 0x00;
constexpr std::uint8_t kProtocolInfoRet = 0x01;
constexpr std::uint8_t kPowerGetStatus = 0x22;
constexpr std::uint8_t kPowerRetStatus = 0x23;
constexpr std::uint8_t kPowerNotifyStatus = 0x25;
constexpr std::uint8_t kBattery = 0x00;
constexpr std::uint8_t kBatteryWithThreshold = 0x08;

struct Frame {
    std::uint8_t type = 0;
    std::uint8_t seq = 0;
    std::vector<std::uint8_t> payload;
};

struct Battery {
    int level = -1;
    bool charging = false;
    bool full = false;
};

inline std::vector<std::uint8_t> Encode(std::uint8_t type, std::uint8_t seq, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> body = { type, seq, (std::uint8_t)(payload.size() >> 24), (std::uint8_t)(payload.size() >> 16),
                                       (std::uint8_t)(payload.size() >> 8), (std::uint8_t)payload.size() };
    body.insert(body.end(), payload.begin(), payload.end());
    std::uint8_t sum = 0;
    for (std::uint8_t b : body) sum = (std::uint8_t)(sum + b);
    body.push_back(sum);
    std::vector<std::uint8_t> out = { kStart };
    for (std::uint8_t b : body) {
        if (b == kStart || b == kEnd || b == kEscape) {
            out.push_back(kEscape);
            out.push_back((std::uint8_t)(b & kEscapeMask));
        } else {
            out.push_back(b);
        }
    }
    out.push_back(kEnd);
    return out;
}

inline std::vector<std::uint8_t> Ack(std::uint8_t receivedSeq) {
    return Encode(kTypeAck, (std::uint8_t)(1 - receivedSeq), {});
}

inline std::vector<std::uint8_t> ProtocolInfoQuery(std::uint8_t seq) {
    return Encode(kTypeCommand, seq, { kProtocolInfoGet, 0x00 });
}

inline std::vector<std::uint8_t> BatteryQuery(std::uint8_t seq, std::uint8_t batteryType) {
    return Encode(kTypeCommand, seq, { kPowerGetStatus, batteryType });
}

// Decodes every complete frame in data[0, size) into `out` and returns how many bytes were used,
// so the caller can keep an incomplete frame at the end for the next read. Frames with a bad
// checksum or length are dropped.
inline std::size_t ParseFrames(const std::uint8_t* data, std::size_t size, std::vector<Frame>& out) {
    std::size_t used = 0;
    std::size_t i = 0;
    while (data && i < size) {
        if (data[i] != kStart) { used = ++i; continue; }
        std::size_t end = i + 1;
        while (end < size && data[end] != kEnd && data[end] != kStart) ++end;
        if (end >= size) break;           // incomplete: keep it
        if (data[end] == kStart) {        // a new frame starts before this one ended
            used = i = end;
            continue;
        }
        std::vector<std::uint8_t> body;
        bool ok = true;
        for (std::size_t k = i + 1; k < end; ++k) {
            if (data[k] == kEscape) {
                if (++k >= end) { ok = false; break; }
                body.push_back((std::uint8_t)(data[k] | (std::uint8_t)~kEscapeMask));
            } else {
                body.push_back(data[k]);
            }
        }
        used = i = end + 1;
        if (!ok || body.size() < 7) continue;
        std::uint8_t sum = 0;
        for (std::size_t k = 0; k + 1 < body.size(); ++k) sum = (std::uint8_t)(sum + body[k]);
        if (sum != body.back()) continue;
        std::size_t len = ((std::size_t)body[2] << 24) | ((std::size_t)body[3] << 16) | ((std::size_t)body[4] << 8) | body[5];
        if (len != body.size() - 7) continue;
        Frame f;
        f.type = body[0];
        f.seq = body[1];
        f.payload.assign(body.begin() + 6, body.end() - 1);
        out.push_back(f);
    }
    return used;
}

// Every frame except an ACK must be acknowledged, or the headphones send it again.
inline bool NeedsAck(const Frame& f) { return f.type == kTypeCommand || f.type == kTypeCommand2; }

inline bool IsProtocolInfoReply(const Frame& f) {
    return f.type == kTypeCommand && !f.payload.empty() && f.payload[0] == kProtocolInfoRet;
}

inline bool ParseBattery(const Frame& f, Battery& out) {
    const auto& p = f.payload;
    if (f.type != kTypeCommand || p.size() < 4) return false;
    if (p[0] != kPowerRetStatus && p[0] != kPowerNotifyStatus) return false;
    if (p[1] != kBattery && p[1] != kBatteryWithThreshold) return false;
    if (p[2] > 100) return false;
    out.level = p[2];
    out.charging = p[3] == 1;
    out.full = p[3] == 3;
    return true;
}

// The whole read-only conversation, independent of sockets and clocks so it can be tested:
//   1. Begin(): the protocol info query (seq 0), as the official app does first.
//   2. Once the headphones answer it, Poll() hands out the plain battery query; if that stays
//      unanswered for kRetryMs, the "with threshold" variant used by newer firmware.
//   3. Receive() takes whatever bytes arrived and returns the ACKs to send back.
// The sequence number alternates 0, 1, 0 ... with every command we send (each ACK the headphones
// send back carries exactly that next number).
class Session {
public:
    static constexpr std::uint64_t kRetryMs = 600;

    std::vector<std::uint8_t> Begin() {
        seq_ = 0;
        return ProtocolInfoQuery(seq_);
    }

    std::vector<std::uint8_t> Poll(std::uint64_t nowMs) {
        static constexpr std::uint8_t kTypes[] = { kBattery, kBatteryWithThreshold };
        if (done_ || !ready_ || sent_ >= 2 || (sent_ > 0 && nowMs - sentAt_ < kRetryMs)) return {};
        seq_ = (std::uint8_t)(1 - seq_);
        sentAt_ = nowMs;
        return BatteryQuery(seq_, kTypes[sent_++]);
    }

    std::vector<std::uint8_t> Receive(const std::uint8_t* data, std::size_t size) {
        buf_.insert(buf_.end(), data, data + size);
        std::vector<Frame> frames;
        buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)ParseFrames(buf_.data(), buf_.size(), frames));
        if (buf_.size() > 8192) buf_.clear();  // garbage without an end marker
        std::vector<std::uint8_t> out;
        for (const Frame& f : frames) {
            if (!NeedsAck(f)) continue;
            std::vector<std::uint8_t> ack = Ack(f.seq);
            out.insert(out.end(), ack.begin(), ack.end());
            if (IsProtocolInfoReply(f)) ready_ = true;
            else if (!done_ && ParseBattery(f, battery_)) done_ = true;
        }
        return out;
    }

    bool Done() const { return done_; }
    const Battery& Result() const { return battery_; }

private:
    std::vector<std::uint8_t> buf_;
    Battery battery_;
    std::uint64_t sentAt_ = 0;
    int sent_ = 0;
    std::uint8_t seq_ = 0;
    bool ready_ = false;
    bool done_ = false;
};

}  // namespace SonyHeadphones
