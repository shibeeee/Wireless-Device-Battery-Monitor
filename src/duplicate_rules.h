#pragma once
// Pure rule for one device that shows up under two entries. No Windows calls.
//
// Many wireless devices have one USB product id on their cable and another on their receiver
// (Razer Viper V3 Pro: 1532:00C0 wired, 1532:00C1 dongle), so plugging the cable in or out moves
// the device to the other entry and leaves the old one behind as a dimmed last-known copy for its
// grace period. That copy is hidden while the same device is live under another entry.
#include <vector>

#include "devices.h"
#include "labels.h"

namespace duplicates {

// Only shows a last-known value: asleep, or no fresh reading this poll.
inline bool IsStale(const DeviceStatus& s, int lastPct) {
    return s.asleep || (s.percent < 0 && lastPct >= 0);
}

// Answering right now with a real level.
inline bool IsLive(const DeviceStatus& s) {
    return s.connected && !s.asleep && s.percent >= 0;
}

// `others` are the other entries currently shown (user-hidden ones left out).
inline bool HiddenByLiveTwin(const DeviceStatus& s, int lastPct, const std::vector<const DeviceStatus*>& others) {
    if (!s.connected || !IsStale(s, lastPct)) return false;
    for (const DeviceStatus* o : others)
        if (o && o != &s && o->kind == s.kind && IsLive(*o) && SameDeviceName(o->name, s.name)) return true;
    return false;
}

}  // namespace duplicates
