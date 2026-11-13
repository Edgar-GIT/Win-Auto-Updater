#pragma once

#include "nt_core.hpp"

// Native battery check (replaces BatteryCare): reads what the battery itself
// reports through the Windows battery driver — design capacity, capacity when
// full now, charge, charge / discharge rate, voltage, cycle count — and grades
// the wear. Nothing is written to the battery; it is a read-out, not a
// calibration.

namespace nt {

struct BatteryInfo {
    bool present = false;       // the PC has a battery
    bool detailed = false;      // capacities came from the battery driver
    bool relativeUnits = false; // the battery reports percentages, not mWh
    bool onAc = false;
    bool charging = false;
    bool discharging = false;
    int count = 0;              // batteries found
    int percent = -1;           // charge, 0..100
    int secondsLeft = -1;       // Windows' estimate while discharging
    uint32_t designMwh = 0;
    uint32_t fullMwh = 0;
    uint32_t currentMwh = 0;
    int32_t rateMw = 0;         // > 0 charging, < 0 discharging, 0 = unknown / idle
    uint32_t voltageMv = 0;
    uint32_t cycles = 0;        // 0 = not reported
    double healthPct = -1.0;    // full / design × 100; < 0 = unknown
    std::wstring chemistry;     // "Li-ion"
    std::wstring manufacturer;
    std::wstring name;
};

enum class BatteryGrade {
    None,     // no battery (desktop)
    Unknown,  // battery present but it does not report its capacities
    Good,     // ≥ 80 % of the design capacity
    Fair,     // 50–80 %
    Poor,     // < 50 %: replace
};

BatteryInfo readBattery();
BatteryGrade gradeBattery(const BatteryInfo& info);
// "Boa", "Usada", "Gasta — substituir", ...
const wchar_t* batteryGradeText(BatteryGrade grade);
// "A carregar", "A descarregar", "Carregada", "Na corrente".
std::wstring batteryStatusText(const BatteryInfo& info);

}  // namespace nt
