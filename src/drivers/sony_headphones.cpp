// Sony WH-1000XM6 headphones over Bluetooth.
// Windows does not always show a battery level for them, so they are asked directly over Sony's
// RFCOMM service {956c7b26-d49a-4ba8-b03f-b17d393cb6e2} (the one the Sound Connect app uses),
// with read-only "MDR" commands: the protocol info query the app always sends first, then the
// battery query (frame format in sony_headphones_protocol.h).
// Only paired devices that Windows reports as connected and whose name contains "WH-1000XM6" are
// tried. If another app on this PC holds the channel, the headphones are skipped.
#include <winsock2.h>
#include <ws2bth.h>

#include "drivers.h"
#include "sony_headphones_protocol.h"

#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace {

// {956c7b26-d49a-4ba8-b03f-b17d393cb6e2}
const GUID kSonyMdr = { 0x956c7b26, 0xd49a, 0x4ba8, { 0xb0, 0x3f, 0xb1, 0x7d, 0x39, 0x3c, 0xb6, 0xe2 } };

const DEVPROPKEY kIsConnected = { { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15 };

struct Headphones { std::wstring name; std::wstring addrText; ULONGLONG addr; };

bool LooksLikeXm6(const std::wstring& name) {
    std::wstring n = name;
    for (auto& ch : n) ch = towlower(ch);
    return n.find(L"wh-1000xm6") != std::wstring::npos;
}

std::vector<Headphones> FindConnectedHeadphones() {
    std::vector<Headphones> out;
    HDEVINFO di = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA dd{ sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t inst[512];
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, inst, 512, nullptr)) continue;
        // The device node itself: BTHENUM\DEV_<12 hex digits address>\...
        if (_wcsnicmp(inst, L"BTHENUM\\DEV_", 12) != 0) continue;
        std::wstring hex(inst + 12, wcsnlen(inst + 12, 12));
        if (hex.size() != 12) continue;
        wchar_t* end = nullptr;
        ULONGLONG addr = wcstoull(hex.c_str(), &end, 16);
        if (!addr || *end) continue;

        DEVPROPTYPE t;
        DEVPROP_BOOLEAN conn = DEVPROP_FALSE;
        if (!SetupDiGetDevicePropertyW(di, &dd, &kIsConnected, &t, (PBYTE)&conn, sizeof(conn), nullptr, 0) ||
            t != DEVPROP_TYPE_BOOLEAN || conn == DEVPROP_FALSE)
            continue;  // never page a device that is not connected (it would only time out)
        wchar_t name[256] = {};
        if (!SetupDiGetDevicePropertyW(di, &dd, &DEVPKEY_Device_FriendlyName, &t, (PBYTE)name, sizeof(name) - sizeof(wchar_t),
                                       nullptr, 0) ||
            t != DEVPROP_TYPE_STRING)
            continue;
        if (!LooksLikeXm6(name)) continue;
        out.push_back({ name, hex, addr });
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

bool StartWinsock() {
    static bool ok = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
}

// Waits until `s` is readable (read) or connected (write). Returns false on timeout or error.
bool WaitSocket(SOCKET s, bool forWrite, int timeoutMs) {
    fd_set set, err;
    FD_ZERO(&set);
    FD_ZERO(&err);
    FD_SET(s, &set);
    FD_SET(s, &err);
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    int r = select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr, &err, &tv);
    return r > 0 && FD_ISSET(s, &set) && !FD_ISSET(s, &err);
}

bool SendAll(SOCKET s, const std::vector<std::uint8_t>& data) {
    return send(s, (const char*)data.data(), (int)data.size(), 0) == (int)data.size();
}

// Connects, sends the protocol info query, then the battery query once the headphones have
// answered. The plain battery query is tried first; firmware that only knows the "with threshold"
// variant gets that one if the first stays unanswered. Every frame the headphones send is ACKed,
// as the protocol requires. One absolute deadline covers the whole exchange.
bool ReadBattery(ULONGLONG addr, SonyHeadphones::Battery& battery) {
    using namespace SonyHeadphones;
    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == INVALID_SOCKET) return false;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    SOCKADDR_BTH sa{};
    sa.addressFamily = AF_BTH;
    sa.btAddr = addr;
    sa.serviceClassId = kSonyMdr;  // Windows looks the RFCOMM channel up over SDP
    sa.port = 0;
    bool found = false;
    if ((connect(s, (sockaddr*)&sa, sizeof(sa)) == 0 || (WSAGetLastError() == WSAEWOULDBLOCK && WaitSocket(s, true, 2500))) &&
        SendAll(s, ProtocolInfoQuery(0))) {
        const std::uint8_t kTypes[] = { kBattery, kBatteryWithThreshold };
        std::uint8_t nextSeq = 1;  // what the headphones' ACK of the first command carries
        size_t sent = 0;           // battery queries sent so far
        ULONGLONG sentAt = 0;
        bool ready = false;        // protocol info answered
        std::vector<std::uint8_t> buf;
        ULONGLONG deadline = GetTickCount64() + 1500;
        while (!found && buf.size() < 8192) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            if (ready && (sent == 0 || (sent < 2 && now - sentAt >= 600))) {
                if (!SendAll(s, BatteryQuery(nextSeq, kTypes[sent]))) break;
                sent++;
                sentAt = now;
            }
            if (!WaitSocket(s, false, (int)std::min<ULONGLONG>(deadline - now, 100))) continue;
            char chunk[512];
            int n = recv(s, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buf.insert(buf.end(), chunk, chunk + n);
            std::vector<Frame> frames;
            buf.erase(buf.begin(), buf.begin() + ParseFrames(buf.data(), buf.size(), frames));
            for (const Frame& f : frames) {
                if (f.type == kTypeAck) {
                    nextSeq = f.seq;
                    continue;
                }
                if (f.type != kTypeCommand) continue;
                SendAll(s, Ack(f.seq));
                if (IsProtocolInfoReply(f)) ready = true;
                else if (ParseBattery(f, battery)) found = true;
            }
        }
    }
    closesocket(s);
    return found;
}

}  // namespace

void PollSonyHeadphones(PollContext& ctx) {
    std::vector<Headphones> list = FindConnectedHeadphones();
    if (list.empty() || !StartWinsock()) return;
    for (const Headphones& h : list) {
        SonyHeadphones::Battery b;
        if (!ReadBattery(h.addr, b)) continue;
        DeviceStatus s;
        s.key = L"sonyheadphones:" + h.addrText;
        s.name = h.name;  // the same name the Bluetooth fallback uses, so it skips these headphones
        s.via = L"Bluetooth";
        s.kind = DeviceKind::Headset;
        s.percent = b.level;
        s.charging = b.charging;
        s.full = b.full;
        s.graceSec = 180;  // a busy link should not make them flicker
        ctx.out.push_back(s);
    }
}
