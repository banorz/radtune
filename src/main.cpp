#include "ADLXHelper.h"
#include "IGPUManualGFXTuning.h"
#include "IGPUManualVRAMTuning.h"
#include "IGPUManualFanTuning.h"
#include "IGPUManualPowerTuning.h"
#include "IGPUTuning.h"
#include "IPerformanceMonitoring.h"
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <optional>
#include <thread>
#include <chrono>
#include <functional>
#include <io.h>
#include <cstdio>

// Single source of truth is CMakeLists (project VERSION -> compile definition).
// The fallback keeps non-CMake builds compiling.
#ifndef RADTUNE_VERSION
#define RADTUNE_VERSION "dev"
#endif

using namespace adlx;

// Helper to print results
void PrintResult(const std::string& msg, ADLX_RESULT res) {
    std::cout << msg << ": " << (res == ADLX_OK ? "OK" : "Failed (Code: " + std::to_string(res) + ")") << std::endl;
}

// VRAM memory timing preset <-> name mapping (ADLX_MEMORYTIMING_DESCRIPTION).
// This is the "Memory Timing Control" preset in AMD Adrenalin, not manual
// sub-timings: a fixed set of presets the driver exposes per GPU.
const char* MemTimingName(ADLX_MEMORYTIMING_DESCRIPTION d) {
    switch (d) {
        case MEMORYTIMING_DEFAULT:              return "default";
        case MEMORYTIMING_FAST_TIMING:          return "fast";
        case MEMORYTIMING_FAST_TIMING_LEVEL_2:  return "fast2";
        case MEMORYTIMING_AUTOMATIC:            return "auto";
        case MEMORYTIMING_MEMORYTIMING_LEVEL_1: return "level1";
        case MEMORYTIMING_MEMORYTIMING_LEVEL_2: return "level2";
        default:                                return "unknown";
    }
}

// Parses a memtiming= value (name or 0-5) into the enum. Returns -1 (the "unset"
// sentinel used by the -set path) when the token is not a known preset.
int ParseMemTiming(const std::string& v) {
    if (v == "default" || v == "0") return MEMORYTIMING_DEFAULT;
    if (v == "fast"    || v == "1") return MEMORYTIMING_FAST_TIMING;
    if (v == "fast2"   || v == "2") return MEMORYTIMING_FAST_TIMING_LEVEL_2;
    if (v == "auto"    || v == "3") return MEMORYTIMING_AUTOMATIC;
    if (v == "level1"  || v == "4") return MEMORYTIMING_MEMORYTIMING_LEVEL_1;
    if (v == "level2"  || v == "5") return MEMORYTIMING_MEMORYTIMING_LEVEL_2;
    return -1;
}

// Fills `out` with the memory-timing presets this GPU actually supports. The
// ADLX enum lists every preset across all Radeon architectures; a given card
// exposes only a subset (e.g. Adrenalin's "Standard"/"Accelerated"). Returns
// false if the list could not be queried (caller should not hard-block then).
bool GetSupportedMemTimings(IADLXManualVRAMTuning2Ptr vram2, IADLXManualVRAMTuning1Ptr vram1,
                            std::vector<ADLX_MEMORYTIMING_DESCRIPTION>& out) {
    IADLXMemoryTimingDescriptionListPtr list;
    ADLX_RESULT res = ADLX_FAIL;
    if (vram2)      res = vram2->GetSupportedMemoryTimingDescriptionList(&list);
    else if (vram1) res = vram1->GetSupportedMemoryTimingDescriptionList(&list);
    if (ADLX_FAILED(res) || !list) return false;
    for (adlx_uint i = 0; i < list->Size(); ++i) {
        IADLXMemoryTimingDescriptionPtr item;
        if (ADLX_SUCCEEDED(list->At(i, &item)) && item) {
            ADLX_MEMORYTIMING_DESCRIPTION d;
            if (ADLX_SUCCEEDED(item->GetDescription(&d))) out.push_back(d);
        }
    }
    return true;
}

// Joins preset names. Default separator reads well for humans ("default, fast");
// machine-readable output passes "," so the consumer doesn't have to trim.
std::string JoinMemTimingNames(const std::vector<ADLX_MEMORYTIMING_DESCRIPTION>& v,
                               const char* sep = ", ") {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += MemTimingName(v[i]);
    }
    return s;
}

// Outcome of an apply pass. `failed` drives the process exit code: RadTune's
// whole point is unattended re-application from a scheduled task, and a run that
// silently reports success while applying nothing is worse than a loud failure.
struct ApplyResult {
    int applied = 0;
    int failed  = 0;
};

// Reports a setting rejected before we even called ADLX (unsupported feature or
// preset). Counts as a failure so the exit code reflects it.
void ReportRejected(ApplyResult& r, const std::string& message) {
    std::cerr << " [!] " << message << std::endl;
    ++r.failed;
}

// Formats an ADLX_IntRange for the -list output, e.g. "[-200 .. 200]".
std::string RangeText(const ADLX_IntRange& r) {
    return "[" + std::to_string(r.minValue) + " .. " + std::to_string(r.maxValue) + "]";
}

// Checks a requested value against the card's advertised range BEFORE calling
// ADLX. This is not belt-and-braces: ADLX silently ignores an out-of-range
// value and still returns ADLX_OK, so without this check "core=99999" would be
// reported as applied (and exit 0) while the GPU kept its old setting.
// If the range itself can't be read, we let ADLX decide rather than block.
bool InRange(ApplyResult& r, ADLX_RESULT rangeRes, const ADLX_IntRange& range,
             int value, const std::string& what, const std::string& unit) {
    if (ADLX_FAILED(rangeRes)) return true;
    if (value >= range.minValue && value <= range.maxValue) return true;
    ReportRejected(r, what + " " + std::to_string(value) + unit + " is out of range "
                      + RangeText(range) + " for this GPU.");
    return false;
}

using Fmt      = std::function<std::string(int)>;
using ReadBack = std::function<ADLX_RESULT(adlx_int*)>;

Fmt WithUnit(const std::string& unit, bool showSign = false) {
    return [unit, showSign](int v) {
        return std::string(showSign && v >= 0 ? "+" : "") + std::to_string(v) + unit;
    };
}

// Reports a Set* outcome, trusting the value read back over the return code.
// ADLX can answer OK and still apply something else: on an RX 9060 XT a -500 MHz
// core offset - inside the range the card itself advertises - lands as -400
// (issue #11). So after a successful write the setting is read back, and a
// mismatch is reported (and counted as a failure) instead of claiming success.
// If the read-back itself fails we fall back to trusting the write.
void ReportVerified(ApplyResult& r, const std::string& what, int requested, const Fmt& fmt,
                    ADLX_RESULT setRes, const ReadBack& readBack) {
    if (ADLX_FAILED(setRes)) {
        std::cerr << " [!] Failed to set " << what << ": " << fmt(requested)
                  << " (Error: " << setRes << ")" << std::endl;
        ++r.failed;
        return;
    }
    adlx_int actual = 0;
    if (readBack && ADLX_SUCCEEDED(readBack(&actual)) && actual != requested) {
        std::cerr << " [!] " << what << ": requested " << fmt(requested)
                  << ", but the driver applied " << fmt(actual) << "." << std::endl;
        ++r.failed;
        return;
    }
    std::cout << " -> " << what << ": " << fmt(requested) << std::endl;
    ++r.applied;
}

// Core min has no IsSupported gate in ADLX. Cards that expose no minimum clock
// (RX 9060 XT and 9070 XT; Adrenalin shows none there either) answer with an
// unreadable range, so that is what "not supported" means here (issue #10).
bool CoreMinSupported(IADLXManualGraphicsTuning2Ptr gfx2) {
    ADLX_IntRange rg{};
    return gfx2 && ADLX_SUCCEEDED(gfx2->GetGPUMinFrequencyRange(&rg));
}

// From Navi4 (RDNA4) on, the max clock and the voltage are OFFSETS from the
// card's base values; on RDNA2/3 they are absolute MHz / mV (ADLX: "Start from
// Navi4+, the maximum frequency is an offset"). ADLX has no flag for it, so we
// read it off the range: an offset range always contains 0 (= stock), while an
// absolute clock or voltage range never does.
bool IsOffsetRange(const ADLX_IntRange& r) { return r.minValue <= 0 && r.maxValue >= 0; }

// Labels for the two settings whose meaning depends on the generation. Unknown
// (range unreadable) gets the neutral name.
std::string CoreMaxLabel(bool known, const ADLX_IntRange& r) {
    return known && IsOffsetRange(r) ? "Core max offset" : "Core max";
}
std::string VoltageLabel(bool known, const ADLX_IntRange& r) {
    return known && IsOffsetRange(r) ? "Voltage offset" : "Voltage";
}

// --- Fan tuning -------------------------------------------------------------
using FanCurve = std::vector<std::pair<int, int>>;   // (temperature C, speed %)

// Reads the card's current fan curve. False if the curve can't be read.
bool ReadFanCurve(IADLXManualFanTuningPtr fan, FanCurve& out) {
    out.clear();
    IADLXManualFanTuningStateListPtr states;
    if (!fan || ADLX_FAILED(fan->GetFanTuningStates(&states)) || !states) return false;
    for (adlx_uint i = 0; i < states->Size(); ++i) {
        IADLXManualFanTuningStatePtr s;
        adlx_int t = 0, sp = 0;
        if (ADLX_FAILED(states->At(i, &s)) || !s ||
            ADLX_FAILED(s->GetTemperature(&t)) || ADLX_FAILED(s->GetFanSpeed(&sp)))
            return false;
        out.emplace_back(t, sp);
    }
    return !out.empty();
}

// "40:20,55:35,..." - the format fancurve= takes and -get prints.
std::string FanCurveText(const FanCurve& c) {
    std::string s;
    for (size_t i = 0; i < c.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(c[i].first) + ":" + std::to_string(c[i].second);
    }
    return s;
}

bool ParseFanCurve(const std::string& text, FanCurve& out) {
    out.clear();
    size_t p = 0;
    while (true) {
        const size_t comma = text.find(',', p);
        const std::string pt = text.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
        const size_t colon = pt.find(':');
        if (colon == std::string::npos) return false;
        try {
            size_t used = 0;
            const std::string ts = pt.substr(0, colon), ss = pt.substr(colon + 1);
            const int t = std::stoi(ts, &used);
            if (used != ts.size()) return false;
            const int sp = std::stoi(ss, &used);
            if (used != ss.size()) return false;
            out.emplace_back(t, sp);
        } catch (...) {
            return false;
        }
        if (comma == std::string::npos) break;
        p = comma + 1;
    }
    return !out.empty();
}

// What this card's manual fan tuning supports. Each feature has its own gate in
// ADLX and RDNA generations differ, so nothing is assumed - the CLI rejects, and
// the GUI disables, whatever isn't here.
struct FanCaps {
    bool minSpeed = false, target = false, acoustic = false, curve = false;
    ADLX_IntRange minSpeedRange{}, targetRange{}, acousticRange{}, curveSpeed{}, curveTemp{};
    size_t curvePoints = 0;
};

FanCaps ProbeFan(IADLXManualFanTuningPtr fan) {
    FanCaps c;
    if (!fan) return c;
    adlx_bool s = false;
    c.minSpeed = ADLX_SUCCEEDED(fan->IsSupportedMinFanSpeed(&s)) && s &&
                 ADLX_SUCCEEDED(fan->GetMinFanSpeedRange(&c.minSpeedRange));
    s = false;
    c.target   = ADLX_SUCCEEDED(fan->IsSupportedTargetFanSpeed(&s)) && s &&
                 ADLX_SUCCEEDED(fan->GetTargetFanSpeedRange(&c.targetRange));
    s = false;
    c.acoustic = ADLX_SUCCEEDED(fan->IsSupportedMinAcousticLimit(&s)) && s &&
                 ADLX_SUCCEEDED(fan->GetMinAcousticLimitRange(&c.acousticRange));
    // The curve has no IsSupported gate: it is usable when both its ranges and
    // its current points can be read.
    FanCurve cur;
    c.curve = ADLX_SUCCEEDED(fan->GetFanTuningRanges(&c.curveSpeed, &c.curveTemp)) &&
              ReadFanCurve(fan, cur);
    c.curvePoints = cur.size();
    return c;
}

// Everything one -set / -load run may ask for. An empty optional means "leave it
// alone", a present one "apply exactly this value" - so every number is legal,
// including negative offsets and 0, and partial requests are the normal case.
struct TuningRequest {
    std::optional<int> coreMax, coreMin, voltage, vramMax, memTiming, powerLimit, zeroRPM;
    std::optional<int> fanMin, fanTarget, acoustic;
    std::optional<FanCurve> fanCurve;
};

#include "ProfileParser.h"
#include "Scheduler.h"

// Handles the "-schedule" verb. Returns true if the verb was consumed (caller
// should exit with *exitCode). Registering a task does not need ADLX, so this
// runs before the SDK is initialized.
bool HandleScheduleVerb(int argc, char* argv[], int& exitCode) {
    if (argc < 2 || std::string(argv[1]) != "-schedule")
        return false;

    auto usage = []() {
        std::cout << "Usage:\n"
                  << "  RadTune -schedule <trigger> <command>\n"
                  << "  Triggers : logon | startup | daily=HH:MM\n"
                  << "  Manage   : RadTune -schedule status | remove\n\n"
                  << "Examples:\n"
                  << "  RadTune -schedule logon -set gpu=0 core=2500 volt=1050\n"
                  << "  RadTune -schedule daily=09:00 -load \"C:\\profile.xml\"\n"
                  << "  RadTune -schedule remove\n";
    };

    if (argc < 3) { usage(); exitCode = 1; return true; }

    std::string sub = argv[2];
    std::string error;

    if (sub == "remove") {
        exitCode = Scheduler::Remove(error) ? 0 : 1;
        if (exitCode == 0) std::cout << "\033[1;32m[+] Scheduled task removed.\033[0m" << std::endl;
        else               std::cerr << "\033[1;31m[!] " << error << "\033[0m" << std::endl;
        return true;
    }
    if (sub == "status") {
        Scheduler::Status();
        exitCode = 0;
        return true;
    }

    // Otherwise `sub` is the trigger and the remaining args are the payload command.
    std::vector<std::string> payload;
    for (int i = 3; i < argc; ++i) payload.push_back(argv[i]);

    if (Scheduler::Install(sub, payload, error)) {
        std::cout << "\033[1;32m[+] Scheduled task '" << Scheduler::TASK_NAME
                  << "' created (trigger: " << sub << ").\033[0m" << std::endl;
        std::cout << "    Runs with highest privileges. Verify with: RadTune -schedule status" << std::endl;
        exitCode = 0;
    } else {
        std::cerr << "\033[1;31m[!] " << error << "\033[0m" << std::endl;
        exitCode = 1;
    }
    return true;
}

void ShowGPUSettings(IADLXGPUPtr gpu, IADLXGPUTuningServicesPtr tuningServices) {
    const char* name;
    gpu->Name(&name);
    const char* vramType;
    gpu->VRAMType(&vramType);
    adlx_uint vramMB;
    gpu->TotalVRAM(&vramMB);
    const char* devId;
    gpu->DeviceId(&devId);

    std::cout << "\n\033[1;36m[#] GPU Information: " << name << "\033[0m" << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;
    std::cout << " [System]    ID: " << devId << "  VRAM: " << vramMB << " MB " << vramType << std::endl;

    // 1. Manual Graphics Tuning
    adlx_bool supported = false;
    tuningServices->IsSupportedManualGFXTuning(gpu, &supported);
    if (supported) {
        IADLXInterfacePtr manualGFXIfc;
        tuningServices->GetManualGFXTuning(gpu, &manualGFXIfc);
        IADLXManualGraphicsTuning2Ptr gfx2(manualGFXIfc);
        if (gfx2) {
            // Ranges first: they say whether core max / voltage are offsets
            // (RDNA4) or absolute values (RDNA2/3), which picks the labels.
            ADLX_IntRange rMin{}, rMax{}, rVolt{};
            const bool okMin  = ADLX_SUCCEEDED(gfx2->GetGPUMinFrequencyRange(&rMin));
            const bool okMax  = ADLX_SUCCEEDED(gfx2->GetGPUMaxFrequencyRange(&rMax));
            const bool okVolt = ADLX_SUCCEEDED(gfx2->GetGPUVoltageRange(&rVolt));

            adlx_int minFreq = 0, maxFreq = 0, voltage = 0;
            const bool hasMin = okMin && ADLX_SUCCEEDED(gfx2->GetGPUMinFrequency(&minFreq));
            gfx2->GetGPUMaxFrequency(&maxFreq);
            gfx2->GetGPUVoltage(&voltage);
            std::cout << std::left << std::setw(15) << " [GFX]"
                      << "Core min: " << std::setw(10) << (hasMin ? std::to_string(minFreq) + " MHz" : "n/a")
                      << CoreMaxLabel(okMax, rMax) << ": " << std::setw(10) << (std::to_string(maxFreq) + " MHz")
                      << VoltageLabel(okVolt, rVolt) << ": " << (std::to_string(voltage) + " mV") << std::endl;

            // Allowed ranges, so the user doesn't have to discover the card's
            // limits by trial and error - especially when core max is an offset
            // and its span is not guessable.
            if (okMin || okMax || okVolt) {
                std::cout << std::left << std::setw(15) << " [GFX range]";
                if (okMin)  std::cout << "coremin=" << std::setw(15) << RangeText(rMin);
                if (okMax)  std::cout << "core=" << std::setw(15) << RangeText(rMax);
                if (okVolt) std::cout << "volt=" << RangeText(rVolt);
                std::cout << std::endl;
            }
        } else {
            IADLXManualGraphicsTuning1Ptr gfx1(manualGFXIfc);
            if (gfx1) {
                IADLXManualTuningStateListPtr states;
                gfx1->GetGPUTuningStates(&states);
                std::cout << " [GFX] Status: " << states->Size() << " discrete states configured." << std::endl;
            }
        }
    } else {
        std::cout << " [GFX] Status: Manual Tuning NOT supported." << std::endl;
    }

    // 2. VRAM Tuning
    tuningServices->IsSupportedManualVRAMTuning(gpu, &supported);
    if (supported) {
        IADLXInterfacePtr vramIfc;
        tuningServices->GetManualVRAMTuning(gpu, &vramIfc);
        IADLXManualVRAMTuning2Ptr vram2(vramIfc);
        IADLXManualVRAMTuning1Ptr vram1(vramIfc);
        if (vram2) {
            adlx_int maxFreq;
            vram2->GetMaxVRAMFrequency(&maxFreq);
            std::cout << std::left << std::setw(15) << " [VRAM]"
                      << "Max Frequency: " << std::setw(12) << (std::to_string(maxFreq) + " MHz");
            ADLX_IntRange rVram{};
            if (ADLX_SUCCEEDED(vram2->GetMaxVRAMFrequencyRange(&rVram)))
                std::cout << "vram=" << RangeText(rVram);
            std::cout << std::endl;
        } else {
             std::cout << " [VRAM] Status: Manual VRAM Tuning 1 supported." << std::endl;
        }

        // Memory timing preset (VRAM latency control). Guarded by its own
        // IsSupportedMemoryTiming: many cards do manual VRAM freq but not this.
        adlx_bool mtSupported = false;
        ADLX_MEMORYTIMING_DESCRIPTION mt;
        ADLX_RESULT mtRes = ADLX_FAIL;
        if (vram2) {
            vram2->IsSupportedMemoryTiming(&mtSupported);
            if (mtSupported) mtRes = vram2->GetMemoryTimingDescription(&mt);
        } else if (vram1) {
            vram1->IsSupportedMemoryTiming(&mtSupported);
            if (mtSupported) mtRes = vram1->GetMemoryTimingDescription(&mt);
        }
        if (mtSupported && ADLX_SUCCEEDED(mtRes)) {
            std::vector<ADLX_MEMORYTIMING_DESCRIPTION> presets;
            GetSupportedMemTimings(vram2, vram1, presets);
            std::cout << std::left << std::setw(15) << " [VRAM Timing]"
                      << "Preset: " << std::setw(10) << MemTimingName(mt);
            if (!presets.empty())
                std::cout << "Supported: " << JoinMemTimingNames(presets);
            std::cout << std::endl;
        }
    }

    // 3. Fan Tuning
    tuningServices->IsSupportedManualFanTuning(gpu, &supported);
    if (supported) {
        IADLXInterfacePtr fanIfc;
        tuningServices->GetManualFanTuning(gpu, &fanIfc);
        IADLXManualFanTuningPtr fan(fanIfc);
        if (fan) {
            const FanCaps fc = ProbeFan(fan);
            adlx_bool zeroRPM = false;
            fan->GetZeroRPMState(&zeroRPM);
            std::cout << std::left << std::setw(15) << " [Fan]"
                      << "Zero RPM: " << (zeroRPM ? "\033[1;32mON\033[0m" : "\033[1;31mOFF\033[0m") << std::endl;

            // One line per supported feature: label, current value, key=range.
            auto row = [](const char* label, const std::string& value, const char* key,
                          const ADLX_IntRange& rg) {
                std::cout << std::left << std::setw(15) << "" << std::setw(16) << label
                          << std::setw(12) << value << key << "=" << RangeText(rg) << std::endl;
            };
            adlx_int v = 0;
            if (fc.minSpeed && ADLX_SUCCEEDED(fan->GetMinFanSpeed(&v)))
                row("Min speed:", std::to_string(v) + " RPM", "fanmin", fc.minSpeedRange);
            if (fc.target && ADLX_SUCCEEDED(fan->GetTargetFanSpeed(&v)))
                row("Target speed:", std::to_string(v) + " RPM", "fantarget", fc.targetRange);
            if (fc.acoustic && ADLX_SUCCEEDED(fan->GetMinAcousticLimit(&v)))
                row("Acoustic limit:", std::to_string(v) + " MHz", "acoustic", fc.acousticRange);

            FanCurve cur;
            if (fc.curve && ReadFanCurve(fan, cur))
                std::cout << std::left << std::setw(15) << " [Fan curve]"
                          << "fancurve=" << FanCurveText(cur) << "   (C:%, " << fc.curvePoints
                          << " points, temp " << RangeText(fc.curveTemp)
                          << " C, speed " << RangeText(fc.curveSpeed) << " %)" << std::endl;
        }
    }

    // 4. Power Tuning
    tuningServices->IsSupportedManualPowerTuning(gpu, &supported);
    if (supported) {
        IADLXInterfacePtr powerIfc;
        tuningServices->GetManualPowerTuning(gpu, &powerIfc);
        IADLXManualPowerTuningPtr power(powerIfc);
        adlx_int powerLimit;
        power->GetPowerLimit(&powerLimit);
        std::cout << std::left << std::setw(15) << " [Power]"
                  << "Power Limit: " << std::setw(12)
                  << ((powerLimit >= 0 ? "+" : "") + std::to_string(powerLimit) + "%");
        ADLX_IntRange rPower{};
        if (ADLX_SUCCEEDED(power->GetPowerLimitRange(&rPower)))
            std::cout << "power=" << RangeText(rPower);
        std::cout << std::endl;
    }
    std::cout << "--------------------------------------------------------" << std::endl;
}


// Fan curve: the card has a fixed number of points (temperature C -> speed %).
// We fill the driver's own empty state list rather than invent a shape, let ADLX
// judge it, then read the curve back like every other setting. Note that
// IsValidFanTuningStates returns ADLX_OK even for an invalid curve - the verdict
// is in errorIndex (-1 = valid).
void ApplyFanCurve(ApplyResult& r, IADLXManualFanTuningPtr fan, const FanCaps& fc, const FanCurve& want) {
    if (!fc.curve) {
        ReportRejected(r, "Fan curve control is not supported on this GPU.");
        return;
    }
    if (want.size() != fc.curvePoints) {
        ReportRejected(r, "This GPU's fan curve has " + std::to_string(fc.curvePoints) +
                          " points, but " + std::to_string(want.size()) + " were given.");
        return;
    }
    for (size_t i = 0; i < want.size(); ++i) {
        const bool tOk = want[i].first  >= fc.curveTemp.minValue  && want[i].first  <= fc.curveTemp.maxValue;
        const bool sOk = want[i].second >= fc.curveSpeed.minValue && want[i].second <= fc.curveSpeed.maxValue;
        if (!tOk || !sOk) {
            ReportRejected(r, "Fan curve point " + std::to_string(i + 1) + " (" +
                              std::to_string(want[i].first) + ":" + std::to_string(want[i].second) +
                              ") is outside temp " + RangeText(fc.curveTemp) + " C / speed " +
                              RangeText(fc.curveSpeed) + " %.");
            return;
        }
    }
    IADLXManualFanTuningStateListPtr states;
    if (ADLX_FAILED(fan->GetEmptyFanTuningStates(&states)) || !states || states->Size() != want.size()) {
        ReportRejected(r, "Could not prepare the fan curve for this GPU.");
        return;
    }
    for (adlx_uint i = 0; i < states->Size(); ++i) {
        IADLXManualFanTuningStatePtr s;
        if (ADLX_SUCCEEDED(states->At(i, &s)) && s) {
            s->SetTemperature(want[i].first);
            s->SetFanSpeed(want[i].second);
        }
    }
    adlx_int bad = -1;
    if (ADLX_FAILED(fan->IsValidFanTuningStates(states, &bad)) || bad >= 0) {
        ReportRejected(r, "The driver rejected the fan curve" +
                          (bad >= 0 ? " at point " + std::to_string(bad + 1) : std::string()) + ".");
        return;
    }
    const ADLX_RESULT res = fan->SetFanTuningStates(states);
    if (ADLX_FAILED(res)) {
        std::cerr << " [!] Failed to set Fan curve: " << FanCurveText(want)
                  << " (Error: " << res << ")" << std::endl;
        ++r.failed;
        return;
    }
    FanCurve got;
    if (ReadFanCurve(fan, got) && got != want) {
        std::cerr << " [!] Fan curve: requested " << FanCurveText(want)
                  << ", but the driver applied " << FanCurveText(got) << "." << std::endl;
        ++r.failed;
        return;
    }
    std::cout << " -> Fan curve: " << FanCurveText(want) << std::endl;
    ++r.applied;
}

ApplyResult ApplySettings(IADLXGPUPtr gpu, IADLXGPUTuningServicesPtr tuningServices,
                          const TuningRequest& q) {
    ApplyResult r;
    IADLXInterfacePtr ifc;
    std::cout << "\n\033[1;33m[*] Applying Settings...\033[0m" << std::endl;

    tuningServices->GetManualGFXTuning(gpu, &ifc);
    IADLXManualGraphicsTuning2Ptr gfx2(ifc);
    if (gfx2) {
        ADLX_IntRange rg{};
        if (q.coreMax) {
            const ADLX_RESULT rr = gfx2->GetGPUMaxFrequencyRange(&rg);
            const std::string what = CoreMaxLabel(ADLX_SUCCEEDED(rr), rg);
            if (InRange(r, rr, rg, *q.coreMax, what, " MHz"))
                ReportVerified(r, what, *q.coreMax, WithUnit(" MHz"),
                               gfx2->SetGPUMaxFrequency(*q.coreMax),
                               [&](adlx_int* v) { return gfx2->GetGPUMaxFrequency(v); });
        }
        if (q.coreMin) {
            if (!CoreMinSupported(gfx2))
                ReportRejected(r, "Core min is not supported on this GPU (it exposes no "
                                  "minimum core clock) - remove coremin= from the command.");
            else if (InRange(r, gfx2->GetGPUMinFrequencyRange(&rg), rg, *q.coreMin, "Core min", " MHz"))
                ReportVerified(r, "Core min", *q.coreMin, WithUnit(" MHz"),
                               gfx2->SetGPUMinFrequency(*q.coreMin),
                               [&](adlx_int* v) { return gfx2->GetGPUMinFrequency(v); });
        }
        if (q.voltage) {
            const ADLX_RESULT rr = gfx2->GetGPUVoltageRange(&rg);
            const std::string what = VoltageLabel(ADLX_SUCCEEDED(rr), rg);
            if (InRange(r, rr, rg, *q.voltage, what, " mV"))
                ReportVerified(r, what, *q.voltage, WithUnit(" mV"),
                               gfx2->SetGPUVoltage(*q.voltage),
                               [&](adlx_int* v) { return gfx2->GetGPUVoltage(v); });
        }
    } else if (q.coreMax || q.coreMin || q.voltage) {
        ReportRejected(r, "Manual graphics tuning is not available on this GPU.");
    }

    if (q.vramMax || q.memTiming) {
        tuningServices->GetManualVRAMTuning(gpu, &ifc);
        IADLXManualVRAMTuning2Ptr vram2(ifc);
        IADLXManualVRAMTuning1Ptr vram1(ifc);
        if (q.vramMax) {
            if (!vram2) {
                ReportRejected(r, "Manual VRAM frequency tuning is not available on this GPU.");
            } else {
                ADLX_IntRange rv{};
                if (InRange(r, vram2->GetMaxVRAMFrequencyRange(&rv), rv,
                            *q.vramMax, "VRAM max frequency", " MHz"))
                    ReportVerified(r, "VRAM max frequency", *q.vramMax, WithUnit(" MHz"),
                                   vram2->SetMaxVRAMFrequency(*q.vramMax),
                                   [&](adlx_int* v) { return vram2->GetMaxVRAMFrequency(v); });
            }
        }
        if (q.memTiming) {
            const ADLX_MEMORYTIMING_DESCRIPTION mt = (ADLX_MEMORYTIMING_DESCRIPTION)*q.memTiming;
            adlx_bool mtSupported = false;
            if (vram2)      vram2->IsSupportedMemoryTiming(&mtSupported);
            else if (vram1) vram1->IsSupportedMemoryTiming(&mtSupported);

            if (!mtSupported) {
                ReportRejected(r, "Memory timing control not supported on this GPU.");
            } else {
                // The ADLX enum is a superset across architectures; reject a
                // preset this card doesn't expose with a clear, listed message
                // rather than a raw ADLX error.
                std::vector<ADLX_MEMORYTIMING_DESCRIPTION> presets;
                const bool haveList = GetSupportedMemTimings(vram2, vram1, presets);
                bool allowed = !haveList;  // list unavailable -> let ADLX decide
                for (auto d : presets) if (d == mt) allowed = true;

                if (!allowed)
                    ReportRejected(r, "Memory timing '" + std::string(MemTimingName(mt)) +
                                      "' not supported on this GPU. Supported: " +
                                      JoinMemTimingNames(presets));
                else
                    ReportVerified(r, "VRAM memory timing", *q.memTiming,
                                   [](int v) { return std::string(MemTimingName((ADLX_MEMORYTIMING_DESCRIPTION)v)); },
                                   vram2 ? vram2->SetMemoryTimingDescription(mt)
                                         : vram1->SetMemoryTimingDescription(mt),
                                   [&](adlx_int* v) {
                                       ADLX_MEMORYTIMING_DESCRIPTION d = MEMORYTIMING_DEFAULT;
                                       const ADLX_RESULT x = vram2 ? vram2->GetMemoryTimingDescription(&d)
                                                                   : vram1->GetMemoryTimingDescription(&d);
                                       *v = (adlx_int)d;
                                       return x;
                                   });
            }
        }
    }

    if (q.powerLimit) {
        tuningServices->GetManualPowerTuning(gpu, &ifc);
        IADLXManualPowerTuningPtr power(ifc);
        if (!power) {
            ReportRejected(r, "Manual power tuning is not available on this GPU.");
        } else {
            ADLX_IntRange rp{};
            if (InRange(r, power->GetPowerLimitRange(&rp), rp, *q.powerLimit, "Power limit", "%"))
                ReportVerified(r, "Power limit", *q.powerLimit, WithUnit("%", true),
                               power->SetPowerLimit(*q.powerLimit),
                               [&](adlx_int* v) { return power->GetPowerLimit(v); });
        }
    }

    if (q.zeroRPM || q.fanMin || q.fanTarget || q.acoustic || q.fanCurve) {
        tuningServices->GetManualFanTuning(gpu, &ifc);
        IADLXManualFanTuningPtr fan(ifc);
        if (!fan) {
            ReportRejected(r, "Manual fan tuning is not available on this GPU.");
        } else {
            const FanCaps fc = ProbeFan(fan);
            if (q.zeroRPM) {
                const int want = (*q.zeroRPM == 1) ? 1 : 0;
                ReportVerified(r, "Zero RPM", want, [](int v) { return std::string(v ? "ON" : "OFF"); },
                               fan->SetZeroRPMState(want == 1),
                               [&](adlx_int* v) {
                                   adlx_bool b = false;
                                   const ADLX_RESULT x = fan->GetZeroRPMState(&b);
                                   *v = b ? 1 : 0;
                                   return x;
                               });
            }
            if (q.fanMin) {
                if (!fc.minSpeed)
                    ReportRejected(r, "Minimum fan speed is not supported on this GPU.");
                else if (InRange(r, ADLX_OK, fc.minSpeedRange, *q.fanMin, "Min fan speed", " RPM"))
                    ReportVerified(r, "Min fan speed", *q.fanMin, WithUnit(" RPM"),
                                   fan->SetMinFanSpeed(*q.fanMin),
                                   [&](adlx_int* v) { return fan->GetMinFanSpeed(v); });
            }
            if (q.fanTarget) {
                if (!fc.target)
                    ReportRejected(r, "Target fan speed is not supported on this GPU.");
                else if (InRange(r, ADLX_OK, fc.targetRange, *q.fanTarget, "Target fan speed", " RPM"))
                    ReportVerified(r, "Target fan speed", *q.fanTarget, WithUnit(" RPM"),
                                   fan->SetTargetFanSpeed(*q.fanTarget),
                                   [&](adlx_int* v) { return fan->GetTargetFanSpeed(v); });
            }
            if (q.acoustic) {
                if (!fc.acoustic)
                    ReportRejected(r, "Acoustic limit is not supported on this GPU.");
                else if (InRange(r, ADLX_OK, fc.acousticRange, *q.acoustic, "Acoustic limit", " MHz"))
                    ReportVerified(r, "Acoustic limit", *q.acoustic, WithUnit(" MHz"),
                                   fan->SetMinAcousticLimit(*q.acoustic),
                                   [&](adlx_int* v) { return fan->GetMinAcousticLimit(v); });
            }
            if (q.fanCurve) ApplyFanCurve(r, fan, fc, *q.fanCurve);
        }
    }

    // Say what actually happened. The old code printed "Successfully applied!"
    // unconditionally, even when every requested setting had been rejected.
    if (r.failed > 0)
        std::cerr << "\033[1;31m[!] " << r.failed << " setting(s) failed, "
                  << r.applied << " applied.\033[0m" << std::endl;
    else if (r.applied > 0)
        std::cout << "\033[1;32m[+] Successfully applied " << r.applied
                  << " setting(s).\033[0m" << std::endl;
    else
        std::cout << "\033[1;33m[i] Nothing to apply (no tuning values given).\033[0m" << std::endl;
    return r;
}

ApplyResult LoadProfileOnGpu(IADLXGPUPtr gpu, IADLXGPUTuningServicesPtr tuningServices, const std::string& path) {
    GPUProfile profile;
    if (!ProfileParser::Parse(path, profile)) {
        std::cerr << "[!] Error parsing XML profile: " << path << std::endl;
        ApplyResult r;
        r.failed = 1;   // a profile we cannot read is a failed run, not a no-op
        return r;
    }

    std::cout << "\033[1;33m[*] Loading Clean Profile (Custom Mapping): \033[0m" << path << std::endl;

    TuningRequest q;

    if (profile.features.count(12)) {
        q.voltage = profile.features[12].states[0].value;
        std::cout << " -> Found ID 12 (Undervolt): " << *q.voltage << std::endl;
    }

    if (profile.features.count(3)) {
        q.powerLimit = profile.features[3].states[0].value;
        std::cout << " -> Found ID 3 (Power Limit): " << *q.powerLimit << std::endl;
    }

    return ApplySettings(gpu, tuningServices, q);
}


// Prints the GPU's current tuning as machine-readable key=value lines.
// Consumed by RadTuneGUI's "Read from GPU" button.
//
// NOTE: unlike ShowGPUSettings(), this does NOT gate on IsSupportedManual*Tuning.
// On some drivers/GPUs those queries return false even though GetManual*Tuning
// hands back a usable interface - which is exactly why ApplySettings() (the -set
// path) skips the check and works. We mirror that here so reading matches writing.
void PrintGpuValues(IADLXGPUPtr gpu, IADLXGPUTuningServicesPtr tuningServices) {
    IADLXInterfacePtr ifc;

    tuningServices->GetManualGFXTuning(gpu, &ifc);
    IADLXManualGraphicsTuning2Ptr gfx2(ifc);
    if (gfx2) {
        adlx_int minFreq = 0, maxFreq = 0, voltage = 0;
        gfx2->GetGPUMaxFrequency(&maxFreq);
        gfx2->GetGPUVoltage(&voltage);
        std::cout << "core=" << maxFreq << "\nvolt=" << voltage << "\n";
        // Whether those two are offsets (RDNA4) or absolute values (RDNA2/3), so
        // the GUI can label its fields truthfully. Omitted if the range is unknown.
        ADLX_IntRange rg{};
        if (ADLX_SUCCEEDED(gfx2->GetGPUMaxFrequencyRange(&rg)))
            std::cout << "coremode=" << (IsOffsetRange(rg) ? "offset" : "absolute") << "\n";
        if (ADLX_SUCCEEDED(gfx2->GetGPUVoltageRange(&rg)))
            std::cout << "voltmode=" << (IsOffsetRange(rg) ? "offset" : "absolute") << "\n";
        // Only when the card has a minimum clock: a missing key tells the GUI to
        // disable the field, instead of offering one whose every write fails.
        if (CoreMinSupported(gfx2) && ADLX_SUCCEEDED(gfx2->GetGPUMinFrequency(&minFreq)))
            std::cout << "coremin=" << minFreq << "\n";
    }

    tuningServices->GetManualVRAMTuning(gpu, &ifc);
    IADLXManualVRAMTuning2Ptr vram2(ifc);
    IADLXManualVRAMTuning1Ptr vram1(ifc);
    if (vram2) {
        adlx_int maxFreq;
        vram2->GetMaxVRAMFrequency(&maxFreq);
        std::cout << "vram=" << maxFreq << "\n";
    }
    {
        adlx_bool mtSupported = false;
        ADLX_MEMORYTIMING_DESCRIPTION mt;
        ADLX_RESULT mtRes = ADLX_FAIL;
        if (vram2) {
            vram2->IsSupportedMemoryTiming(&mtSupported);
            if (mtSupported) mtRes = vram2->GetMemoryTimingDescription(&mt);
        } else if (vram1) {
            vram1->IsSupportedMemoryTiming(&mtSupported);
            if (mtSupported) mtRes = vram1->GetMemoryTimingDescription(&mt);
        }
        if (mtSupported && ADLX_SUCCEEDED(mtRes)) {
            std::cout << "memtiming=" << MemTimingName(mt) << "\n";
            // The GUI builds its dropdown from this: the ADLX enum is a superset
            // across architectures, so offering all of it would let the user pick
            // a preset this card will refuse.
            std::vector<ADLX_MEMORYTIMING_DESCRIPTION> presets;
            GetSupportedMemTimings(vram2, vram1, presets);
            if (!presets.empty())
                std::cout << "memtimingsupported=" << JoinMemTimingNames(presets, ",") << "\n";
        }
    }

    tuningServices->GetManualPowerTuning(gpu, &ifc);
    IADLXManualPowerTuningPtr power(ifc);
    if (power) {
        adlx_int powerLimit;
        power->GetPowerLimit(&powerLimit);
        std::cout << "power=" << powerLimit << "\n";
    }

    tuningServices->GetManualFanTuning(gpu, &ifc);
    IADLXManualFanTuningPtr fan(ifc);
    if (fan) {
        adlx_bool zeroRPM = false;
        if (ADLX_SUCCEEDED(fan->GetZeroRPMState(&zeroRPM)))
            std::cout << "zerorpm=" << (zeroRPM ? 1 : 0) << "\n";
        // Like coremin: each fan key appears only if this card supports it.
        const FanCaps fc = ProbeFan(fan);
        adlx_int v = 0;
        if (fc.minSpeed && ADLX_SUCCEEDED(fan->GetMinFanSpeed(&v)))      std::cout << "fanmin=" << v << "\n";
        if (fc.target   && ADLX_SUCCEEDED(fan->GetTargetFanSpeed(&v)))   std::cout << "fantarget=" << v << "\n";
        if (fc.acoustic && ADLX_SUCCEEDED(fan->GetMinAcousticLimit(&v))) std::cout << "acoustic=" << v << "\n";
        FanCurve cur;
        if (fc.curve && ReadFanCurve(fan, cur)) std::cout << "fancurve=" << FanCurveText(cur) << "\n";
    }

    // Flush now. When stdout is a pipe (launched by the GUI) it is block-
    // buffered; these lines use "\n", not std::endl, so without an explicit
    // flush they sit in the buffer and are lost when ADLX teardown skips the
    // normal exit-time flush. From a console each "\n" flushes, hiding the bug.
    std::cout.flush();
}

// Prints the GPU's current LIVE telemetry as machine-readable key=value lines.
// Consumed by RadTuneGUI's "Live" tab (Refresh). Unlike the tuning values, these
// are the real, in-the-moment readings (actual boost clock, temps, fan, power).
// Each metric is emitted only if the driver reports it, so unsupported ones are
// silently skipped. Note: `power=` here is watts (GPU power), distinct from the
// tuning `power=` percentage in -get.
void PrintGpuMetrics(IADLXGPUPtr gpu, IADLXPerformanceMonitoringServicesPtr perf) {
    IADLXGPUMetricsPtr m;
    if (ADLX_FAILED(perf->GetCurrentGPUMetrics(gpu, &m)) || !m) {
        std::cerr << "[!] Could not read GPU metrics." << std::endl;
        return;
    }
    adlx_int i;
    adlx_double d;
    if (ADLX_SUCCEEDED(m->GPUClockSpeed(&i)))         std::cout << "gpuclock="  << i << "\n";
    if (ADLX_SUCCEEDED(m->GPUVRAMClockSpeed(&i)))     std::cout << "vramclock=" << i << "\n";
    if (ADLX_SUCCEEDED(m->GPUTemperature(&d)))        std::cout << "temp="      << d << "\n";
    if (ADLX_SUCCEEDED(m->GPUHotspotTemperature(&d))) std::cout << "hotspot="   << d << "\n";
    if (ADLX_SUCCEEDED(m->GPUFanSpeed(&i)))           std::cout << "fan="       << i << "\n";
    if (ADLX_SUCCEEDED(m->GPUPower(&d)))              std::cout << "power="      << d << "\n";
    if (ADLX_SUCCEEDED(m->GPUTotalBoardPower(&d)))    std::cout << "boardpower=" << d << "\n";
    if (ADLX_SUCCEEDED(m->GPUVoltage(&i)))            std::cout << "voltage="   << i << "\n";
    if (ADLX_SUCCEEDED(m->GPUUsage(&d)))              std::cout << "usage="     << d << "\n";
    if (ADLX_SUCCEEDED(m->GPUVRAM(&i)))               std::cout << "vramused="  << i << "\n";
    std::cout.flush();
}

int main(int argc, char* argv[]) {
    // Banner only when a human is watching. Piped output feeds the GUI, scripts
    // and machine-readable verbs (-get/-gpus/-monitor), where five lines of
    // ASCII art are pure noise - and they used to land inside the GUI's dialogs.
    if (_isatty(_fileno(stdout))) {
        std::cout << "\033[1;31m" << "  ____           _ _____                 " << "\033[0m" << std::endl;
        std::cout << "\033[1;31m" << " |  _ \\ __ _  __| |_   _|   _ _ __   ___ " << "\033[0m" << std::endl;
        std::cout << "\033[1;31m" << " | |_) / _` |/ _` | | || | | | '_ \\ / _ \\" << "\033[0m" << std::endl;
        std::cout << "\033[1;31m" << " |  _ < (_| | (_| | | || |_| | | | |  __/" << "\033[0m" << std::endl;
        std::cout << "\033[1;31m" << " |_| \\_\\__,_|\\__,_| |_| \\__,_|_| |_|\\___|" << "\033[0m"
                  << " v" << RADTUNE_VERSION << " (ADLX)" << std::endl;
    }

    // Task Scheduler management does not need the GPU/ADLX; handle it first.
    int scheduleExit = 0;
    if (HandleScheduleVerb(argc, argv, scheduleExit))
        return scheduleExit;

    ADLX_RESULT res = g_ADLX.Initialize();
    if (ADLX_FAILED(res)) {
        std::cerr << "\n\033[1;31m[!] ADLX Initialization failed.\033[0m" << std::endl;
        return 1;
    }

    IADLXSystem* systemServices = g_ADLX.GetSystemServices();
    IADLXGPUTuningServicesPtr tuningServices;
    systemServices->GetGPUTuningServices(&tuningServices);

    IADLXGPUListPtr gpus;
    systemServices->GetGPUs(&gpus);

    // Exit code contract: 0 only when the requested work actually succeeded.
    // Unattended runs (Task Scheduler) are the main consumer, and nobody is
    // watching the console - so failures must be visible to the caller.
    int exitCode = 0;

    if (argc > 1) {
        std::string cmd = argv[1];
        if (cmd == "-list") {
            for (adlx_uint i = 0; i < gpus->Size(); ++i) {
                IADLXGPUPtr gpu;
                gpus->At(i, &gpu);
                ShowGPUSettings(gpu, tuningServices);
            }
        } else if (cmd == "-get") {
            int targetGpu = 0;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg.find("gpu=") == 0) targetGpu = std::stoi(arg.substr(4));
            }
            if (targetGpu < (int)gpus->Size()) {
                IADLXGPUPtr gpu;
                gpus->At(targetGpu, &gpu);
                PrintGpuValues(gpu, tuningServices);
            } else {
                std::cerr << "[!] GPU index " << targetGpu << " out of range." << std::endl;
                exitCode = 1;
            }
        } else if (cmd == "-gpus") {
            // Machine-readable GPU list for the GUI's device dropdown (-list is
            // formatted for humans).
            for (adlx_uint i = 0; i < gpus->Size(); ++i) {
                IADLXGPUPtr gpu;
                gpus->At(i, &gpu);
                const char* name = nullptr;
                gpu->Name(&name);
                std::cout << "gpu" << i << "=" << (name ? name : "Unknown GPU") << "\n";
            }
            std::cout.flush();
        } else if (cmd == "-monitor") {
            int targetGpu = 0;
            int watchMs = 0;   // 0 = single sample and exit
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg.find("gpu=") == 0) targetGpu = std::stoi(arg.substr(4));
                else if (arg.find("watch=") == 0) watchMs = std::stoi(arg.substr(6));
            }
            // Scoped locally so the perf-monitoring interface is released before
            // g_ADLX.Terminate() (same teardown rule as the other interfaces).
            IADLXPerformanceMonitoringServicesPtr perf;
            if (ADLX_FAILED(systemServices->GetPerformanceMonitoringServices(&perf)) || !perf) {
                std::cerr << "[!] Performance monitoring not available." << std::endl;
                exitCode = 1;
            } else if (targetGpu < (int)gpus->Size()) {
                IADLXGPUPtr gpu;
                gpus->At(targetGpu, &gpu);
                if (watchMs <= 0) {
                    PrintGpuMetrics(gpu, perf);
                } else {
                    // Streaming mode. ADLX init costs ~600 ms, so re-launching
                    // the process per sample would make "live" both wasteful and
                    // permanently half a second stale. Here we init once and emit
                    // a sample every watchMs, each terminated by a blank line so
                    // the reader can tell complete records apart. Runs until the
                    // parent closes the pipe or kills us.
                    if (watchMs < 100) watchMs = 100;   // no busy-looping
                    while (std::cout) {
                        PrintGpuMetrics(gpu, perf);
                        std::cout << std::endl;         // record separator + flush
                        std::this_thread::sleep_for(std::chrono::milliseconds(watchMs));
                    }
                }
            } else {
                std::cerr << "[!] GPU index " << targetGpu << " out of range." << std::endl;
                exitCode = 1;
            }
        } else if (cmd == "-load" && argc > 2) {
            std::string path = argv[2];
            int targetGpu = 0;
            if (argc > 3) {
                 std::string gpuArg = argv[3];
                 if (gpuArg.find("gpu=") == 0) targetGpu = std::stoi(gpuArg.substr(4));
            }

            if (targetGpu < (int)gpus->Size()) {
                IADLXGPUPtr gpu;
                gpus->At(targetGpu, &gpu);
                if (LoadProfileOnGpu(gpu, tuningServices, path).failed > 0) exitCode = 1;
            } else {
                std::cerr << "[!] GPU index " << targetGpu << " out of range." << std::endl;
                exitCode = 1;
            }
        } else if (cmd == "-set" && argc > 2) {
            std::optional<int> targetGpu;
            TuningRequest q;
            bool argsOk = true;

            // Every numeric key goes through here: a value that isn't a whole
            // number is a bad argument (std::stoi on "abc" used to throw and kill
            // the run with no message).
            auto parseInt = [&](const std::string& arg, size_t keyLen, std::optional<int>& dst) {
                const std::string v = arg.substr(keyLen);
                try {
                    size_t used = 0;
                    const int n = std::stoi(v, &used);
                    if (used == v.size()) { dst = n; return; }
                } catch (...) {}
                std::cerr << " [!] Invalid value in '" << arg << "' (expected a whole number)." << std::endl;
                argsOk = false;
            };

            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];
                if      (arg.find("gpu=") == 0)       parseInt(arg, 4, targetGpu);
                else if (arg.find("core=") == 0)      parseInt(arg, 5, q.coreMax);
                else if (arg.find("coremin=") == 0)   parseInt(arg, 8, q.coreMin);
                else if (arg.find("volt=") == 0)      parseInt(arg, 5, q.voltage);
                else if (arg.find("vram=") == 0)      parseInt(arg, 5, q.vramMax);
                else if (arg.find("power=") == 0)     parseInt(arg, 6, q.powerLimit);
                else if (arg.find("zerorpm=") == 0)   parseInt(arg, 8, q.zeroRPM);
                else if (arg.find("fanmin=") == 0)    parseInt(arg, 7, q.fanMin);
                else if (arg.find("fantarget=") == 0) parseInt(arg, 10, q.fanTarget);
                else if (arg.find("acoustic=") == 0)  parseInt(arg, 9, q.acoustic);
                else if (arg.find("memtiming=") == 0) {
                    const int mt = ParseMemTiming(arg.substr(10));
                    if (mt >= 0) {
                        q.memTiming = mt;
                    } else {
                        std::cerr << " [!] Unknown memtiming value: " << arg.substr(10)
                                  << " (use default|fast|fast2|auto|level1|level2)" << std::endl;
                        argsOk = false;
                    }
                } else if (arg.find("fancurve=") == 0) {
                    FanCurve c;
                    if (ParseFanCurve(arg.substr(9), c)) {
                        q.fanCurve = c;
                    } else {
                        std::cerr << " [!] Invalid fancurve '" << arg.substr(9) << "' (expected "
                                  << "temp:speed pairs, e.g. 40:20,55:35,70:55,85:80,95:100)." << std::endl;
                        argsOk = false;
                    }
                } else {
                    // Used to be ignored silently, so a typo like "fanmn=" looked
                    // like it had been applied.
                    std::cerr << " [!] Unknown argument '" << arg << "'." << std::endl;
                    argsOk = false;
                }
            }

            const int gpuIdx = targetGpu.value_or(0);
            if (!argsOk) {
                // Validate first, apply second: with a malformed command line the
                // user's intent is unclear, so nothing is touched.
                std::cerr << "[!] Nothing applied - fix the arguments above." << std::endl;
                exitCode = 1;
            } else if (gpuIdx >= 0 && gpuIdx < (int)gpus->Size()) {
                IADLXGPUPtr gpu;
                gpus->At(gpuIdx, &gpu);
                if (ApplySettings(gpu, tuningServices, q).failed > 0)
                    exitCode = 1;
            } else {
                std::cerr << "[!] GPU index " << gpuIdx << " out of range." << std::endl;
                exitCode = 1;
            }

        } else {
            std::cout << "Usage:" << std::endl;
            std::cout << "  RadTune -list" << std::endl;
            std::cout << "  RadTune -get [gpu=N]" << std::endl;
            std::cout << "  RadTune -gpus                 (machine-readable GPU list)" << std::endl;
            std::cout << "  RadTune -monitor [gpu=N] [watch=ms]   (live telemetry; watch= streams samples)" << std::endl;
            std::cout << "  RadTune -set [gpu=N] [core=MHz] [coremin=MHz] [volt=mV] [vram=MHz] [memtiming=default|fast|fast2|auto|level1|level2] [power=%]" << std::endl;
            std::cout << "               [zerorpm=0|1] [fanmin=RPM] [fantarget=RPM] [acoustic=MHz] [fancurve=T:S,T:S,...]" << std::endl;
            std::cout << "               (run -list to see which of these your GPU supports, and their ranges)" << std::endl;
            std::cout << "  RadTune -load profile.xml [gpu=N]" << std::endl;
            std::cout << "  RadTune -schedule <logon|startup|daily=HH:MM> <-set ...|-load ...>" << std::endl;
            std::cout << "  RadTune -schedule status | remove" << std::endl;
            exitCode = 1;   // unknown verb
        }
    } else {
        std::cout << "RadTune v" << RADTUNE_VERSION << " (ADLX based)" << std::endl;
        std::cout << "Usage: RadTune [-list | -get | -monitor | -set ... | -load ...]" << std::endl;
    }

    // ADLX invalidates every outstanding interface when Terminate() runs;
    // releasing a smart pointer *after* that dereferences a freed vtable -> an
    // access violation on exit (see the WARNING in ADLXHelper.h). Release the
    // interfaces we still hold BEFORE terminating so teardown is clean. Any
    // per-GPU interfaces are already scoped inside the dispatch above.
    tuningServices = nullptr;
    gpus = nullptr;

    g_ADLX.Terminate();
    return exitCode;
}
