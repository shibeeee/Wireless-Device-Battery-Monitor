// Checks which dimmed copies of a device are hidden (src/duplicate_rules.h).
#include <cassert>
#include <cstdio>
#include <map>
#include <string>

#include "../src/duplicate_rules.h"

using duplicates::HiddenByLiveTwin;

namespace {

// A tiny stand-in for the app's entry list: the last status and the last real level of each key.
struct Entry {
    DeviceStatus st;
    int lastPct = -1;
};

// One poll, the way main.cpp applies it: reported keys get their fresh status; a key that was not
// reported stays for its grace period as a last-known value (no level, asleep, no charge flags).
void Poll(std::map<std::wstring, Entry>& entries, const std::vector<DeviceStatus>& results) {
    std::map<std::wstring, bool> seen;
    for (const auto& s : results) {
        auto& e = entries[s.key];
        e.st = s;
        if (s.percent >= 0) e.lastPct = s.percent;
        seen[s.key] = true;
    }
    for (auto& [k, e] : entries)
        if (!seen.count(k)) {
            e.st.percent = -1;
            e.st.asleep = true;
            e.st.charging = false;
            e.st.full = false;
        }
}

bool Hidden(const std::map<std::wstring, Entry>& entries, const std::wstring& key) {
    std::vector<const DeviceStatus*> others;
    for (const auto& [k, e] : entries)
        if (k != key) others.push_back(&e.st);
    const Entry& e = entries.at(key);
    return HiddenByLiveTwin(e.st, e.lastPct, others);
}

std::vector<std::wstring> Shown(const std::map<std::wstring, Entry>& entries) {
    std::vector<std::wstring> out;
    for (const auto& [k, e] : entries)
        if (!Hidden(entries, k)) out.push_back(k);
    return out;
}

DeviceStatus Razer(const wchar_t* pid, int pct, bool charging = false, bool full = false) {
    DeviceStatus s;
    s.key = std::wstring(L"razer:") + pid;
    s.name = L"Razer Viper V3 Pro";  // the Razer driver strips " (Wired)" / " (Wireless)" / " Dongle"
    s.via = L"Razer receiver";
    s.kind = DeviceKind::Mouse;
    s.percent = pct;
    s.charging = charging;
    s.full = full;
    s.graceSec = 120;
    return s;
}

}  // namespace

int main() {
    using V = std::vector<std::wstring>;

    // Razer Viper V3 Pro: dongle 1532:00C1, cable 1532:00C0.
    std::map<std::wstring, Entry> e;
    Poll(e, { Razer(L"00C1", 85) });
    assert(Shown(e) == (V{ L"razer:00C1" }));

    // Plugged in: the cable answers first and is the one listed; the dongle entry goes stale.
    Poll(e, { Razer(L"00C0", 85, true) });
    assert(Shown(e) == (V{ L"razer:00C0" }));  // first screenshot: only one mouse while charging

    Poll(e, { Razer(L"00C0", 100, true, true) });
    assert(Shown(e) == (V{ L"razer:00C0" }));

    // Unplugged: the dongle is live again; the cable entry is a stale copy that still says "full".
    Poll(e, { Razer(L"00C1", 100) });
    assert(Shown(e) == (V{ L"razer:00C1" }));  // second screenshot: the dimmed 100% with a check is gone
    assert(!e[L"razer:00C0"].st.full && !e[L"razer:00C0"].st.charging);

    // Mouse switched off: both entries are stale, nothing live to stand in, so the last value stays.
    Poll(e, {});
    assert(Shown(e).size() == 2);

    // A different mouse that is asleep is never hidden by a live one.
    std::map<std::wstring, Entry> two;
    DeviceStatus other = Razer(L"00AB", 40);
    other.name = L"Razer DeathAdder V3 Pro";
    Poll(two, { Razer(L"00C1", 70), other });
    Poll(two, { Razer(L"00C1", 70) });
    assert(Shown(two).size() == 2);

    // Same name but a different kind (a headset called like the mouse) is not a duplicate.
    std::map<std::wstring, Entry> kinds;
    DeviceStatus headset = Razer(L"0527", 50);
    headset.kind = DeviceKind::Headset;
    Poll(kinds, { Razer(L"00C1", 70), headset });
    Poll(kinds, { Razer(L"00C1", 70) });
    assert(Shown(kinds).size() == 2);

    // An explicitly offline entry (receiver present, mouse off) is not a last-known copy.
    DeviceStatus off = Razer(L"00C1", -1);
    off.connected = false;
    DeviceStatus cable = Razer(L"00C0", 60, true);
    assert(!HiddenByLiveTwin(off, 80, { &cable }));

    // A stale entry with no level ever seen is not "last known" and stays as it is.
    DeviceStatus fresh = Razer(L"00C1", -1);
    assert(!HiddenByLiveTwin(fresh, -1, { &cable }));

    std::printf("duplicate_rules_test: all passed\n");
    return 0;
}
