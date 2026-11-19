#pragma once
// Short captions that tell apart devices of the same kind in the taskbar widget (two mice, two
// controllers...), which are otherwise drawn with the same icon. Pure string logic, no Windows calls.
#include <string>
#include <vector>

#include "devices.h"

struct LabelInput {
    DeviceKind kind;
    std::wstring name;
};

// One caption per input, in the same order. A device gets a caption only when another device of the same
// kind is shown too; otherwise its caption is empty. Captions are at most `maxChars` characters and are
// distinct within a kind: the first significant word of the name when that tells them apart (maker and
// generic words like "Wireless" or "Mouse" are skipped), then more words, then a trailing number in the
// name ("Xbox controller 2" -> "2"), and finally 1, 2, 3... in input order (two identical "DualSense").
std::vector<std::wstring> ShortLabels(const std::vector<LabelInput>& items, size_t maxChars = 3);

// True when two device names are the same once maker and generic words ("Razer", "Wireless", "USB",
// "Receiver"...) are left out, ignoring case: the same device reported twice, e.g. once by its
// receiver and once on its charging cable ("Razer Viper Ultimate" / "Razer Viper Ultimate Wireless").
bool SameDeviceName(const std::wstring& a, const std::wstring& b);
