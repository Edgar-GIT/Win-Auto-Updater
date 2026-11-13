#include "battery.hpp"

#include <setupapi.h>
#include <winioctl.h>

#include <poclass.h>

#include <algorithm>
#include <cwctype>

namespace nt {

namespace {

// GUID_DEVCLASS_BATTERY, spelled out so no initguid ordering is involved.
const GUID kBatteryClass = {0x72631e54, 0x78A4, 0x11d0, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}};

std::wstring trimmed(std::wstring text) {
    while (!text.empty() && (text.back() == L'\0' || std::iswspace(text.back()))) {
        text.pop_back();
    }
    size_t start = 0;
    while (start < text.size() && std::iswspace(text[start])) {
        ++start;
    }
    return text.substr(start);
}

std::wstring queryString(HANDLE battery, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level) {
    BATTERY_QUERY_INFORMATION query{};
    query.BatteryTag = tag;
    query.InformationLevel = level;
    wchar_t buffer[256]{};
    DWORD returned = 0;
    if (!DeviceIoControl(battery, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query), buffer,
                         sizeof(buffer) - sizeof(wchar_t), &returned, nullptr)) {
        return {};
    }
    return trimmed(std::wstring(buffer, returned / sizeof(wchar_t)));
}

std::wstring chemistryName(const UCHAR chemistry[4]) {
    std::string raw;
    for (int i = 0; i < 4 && chemistry[i] != 0; ++i) {
        raw.push_back(static_cast<char>(std::toupper(chemistry[i])));
    }
    if (raw == "LION" || raw == "LI-I" || raw == "LIIO") {
        return L"Li-ion";
    }
    if (raw == "LIP" || raw == "LIPO" || raw == "LI-P") {
        return L"Li-polímero";
    }
    if (raw == "NIMH") {
        return L"NiMH";
    }
    if (raw == "NICD") {
        return L"NiCd";
    }
    if (raw == "PBAC") {
        return L"Chumbo";
    }
    return std::wstring(raw.begin(), raw.end());
}

// Adds one battery device to the totals. Returns false when it is not a
// (present) battery.
bool readDevice(const wchar_t* path, BatteryInfo* info) {
    HANDLE battery = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (battery == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = false;
    ULONG wait = 0;
    ULONG tag = 0;
    DWORD returned = 0;
    if (DeviceIoControl(battery, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &tag, sizeof(tag), &returned,
                        nullptr) &&
        tag != 0) {
        BATTERY_QUERY_INFORMATION query{};
        query.BatteryTag = tag;
        query.InformationLevel = BatteryInformation;
        BATTERY_INFORMATION facts{};
        if (DeviceIoControl(battery, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query), &facts,
                            sizeof(facts), &returned, nullptr) &&
            (facts.Capabilities & BATTERY_SYSTEM_BATTERY) != 0) {
            ok = true;
            ++info->count;
            if ((facts.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0) {
                info->relativeUnits = true;
            }
            if (facts.DesignedCapacity != BATTERY_UNKNOWN_CAPACITY) {
                info->designMwh += facts.DesignedCapacity;
            }
            if (facts.FullChargedCapacity != BATTERY_UNKNOWN_CAPACITY) {
                info->fullMwh += facts.FullChargedCapacity;
            }
            info->cycles = std::max<uint32_t>(info->cycles, facts.CycleCount);
            if (info->chemistry.empty()) {
                info->chemistry = chemistryName(facts.Chemistry);
            }
            if (info->manufacturer.empty()) {
                info->manufacturer = queryString(battery, tag, BatteryManufactureName);
            }
            if (info->name.empty()) {
                info->name = queryString(battery, tag, BatteryDeviceName);
            }

            BATTERY_WAIT_STATUS waitStatus{};
            waitStatus.BatteryTag = tag;
            BATTERY_STATUS status{};
            if (DeviceIoControl(battery, IOCTL_BATTERY_QUERY_STATUS, &waitStatus, sizeof(waitStatus), &status,
                                sizeof(status), &returned, nullptr)) {
                if (status.Capacity != BATTERY_UNKNOWN_CAPACITY) {
                    info->currentMwh += status.Capacity;
                }
                if (status.Voltage != BATTERY_UNKNOWN_VOLTAGE && info->voltageMv == 0) {
                    info->voltageMv = status.Voltage;
                }
                if (static_cast<ULONG>(status.Rate) != BATTERY_UNKNOWN_RATE) {
                    info->rateMw += status.Rate;
                }
                if ((status.PowerState & BATTERY_CHARGING) != 0) {
                    info->charging = true;
                }
                if ((status.PowerState & BATTERY_DISCHARGING) != 0) {
                    info->discharging = true;
                }
            }
        }
    }
    CloseHandle(battery);
    return ok;
}

}  // namespace

BatteryInfo readBattery() {
    BatteryInfo info;

    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        info.onAc = power.ACLineStatus == 1;
        // BatteryFlag 128 = no system battery, 255 = unknown.
        info.present = power.BatteryFlag != 128 && power.BatteryFlag != 255;
        if (power.BatteryLifePercent <= 100) {
            info.percent = power.BatteryLifePercent;
        }
        if (power.BatteryLifeTime != static_cast<DWORD>(-1)) {
            info.secondsLeft = static_cast<int>(power.BatteryLifeTime);
        }
        if ((power.BatteryFlag & 8) != 0) {
            info.charging = true;
        }
    }

    HDEVINFO devices = SetupDiGetClassDevsW(&kBatteryClass, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices != INVALID_HANDLE_VALUE) {
        for (DWORD index = 0; index < 16; ++index) {
            SP_DEVICE_INTERFACE_DATA iface{};
            iface.cbSize = sizeof(iface);
            if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &kBatteryClass, index, &iface)) {
                break;
            }
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &needed, nullptr);
            if (needed == 0) {
                continue;
            }
            std::vector<uint8_t> buffer(needed + sizeof(wchar_t));
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, needed, nullptr, nullptr)) {
                readDevice(detail->DevicePath, &info);
            }
        }
        SetupDiDestroyDeviceInfoList(devices);
    }

    if (info.count > 0) {
        info.present = true;
        info.detailed = info.designMwh > 0 && info.fullMwh > 0;
        if (info.detailed) {
            info.healthPct = std::min(100.0, 100.0 * static_cast<double>(info.fullMwh) /
                                                 static_cast<double>(info.designMwh));
            if (info.percent < 0 && info.fullMwh > 0) {
                info.percent = static_cast<int>(
                    std::min<uint64_t>(100, 100ULL * info.currentMwh / info.fullMwh));
            }
        }
        // A battery that is neither charging nor discharging has no rate.
        if (!info.charging && !info.discharging) {
            info.rateMw = 0;
        }
    }
    if (info.charging) {
        info.discharging = false;
    } else if (info.present && !info.onAc) {
        info.discharging = true;
    }
    return info;
}

BatteryGrade gradeBattery(const BatteryInfo& info) {
    if (!info.present) {
        return BatteryGrade::None;
    }
    if (info.healthPct < 0.0) {
        return BatteryGrade::Unknown;
    }
    if (info.healthPct >= 80.0) {
        return BatteryGrade::Good;
    }
    if (info.healthPct >= 50.0) {
        return BatteryGrade::Fair;
    }
    return BatteryGrade::Poor;
}

const wchar_t* batteryGradeText(BatteryGrade grade) {
    switch (grade) {
        case BatteryGrade::None:
            return L"Sem bateria";
        case BatteryGrade::Unknown:
            return L"Sem dados";
        case BatteryGrade::Good:
            return L"Boa";
        case BatteryGrade::Fair:
            return L"Usada";
        case BatteryGrade::Poor:
            return L"Gasta — substituir";
    }
    return L"";
}

std::wstring batteryStatusText(const BatteryInfo& info) {
    if (!info.present) {
        return L"Sem bateria";
    }
    if (info.charging) {
        return L"A carregar";
    }
    if (info.discharging) {
        return L"A descarregar";
    }
    if (info.percent >= 99) {
        return L"Carregada";
    }
    return info.onAc ? L"Na corrente" : L"Em repouso";
}

}  // namespace nt
