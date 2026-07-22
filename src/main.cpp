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

// Comma-joins preset names, e.g. "default, fast".
std::string JoinMemTimingNames(const std::vector<ADLX_MEMORYTIMING_DESCRIPTION>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += MemTimingName(v[i]);
    }
    return s;
}

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
            adlx_int minFreq, maxFreq, voltage;
            gfx2->GetGPUMinFrequency(&minFreq);
            gfx2->GetGPUMaxFrequency(&maxFreq);
            gfx2->GetGPUVoltage(&voltage);
            std::cout << std::left << std::setw(15) << " [GFX]"
                      << "Core min: " << std::setw(10) << (std::to_string(minFreq) + " MHz")
                      << "Core max offset: " << std::setw(10) << (std::to_string(maxFreq) + " MHz")
                      << "Voltage offset: " << (std::to_string(voltage) + " mV") << std::endl;
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
                      << "Max Frequency: " << maxFreq << " MHz" << std::endl;
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
        adlx_bool zeroRPM;
        fan->GetZeroRPMState(&zeroRPM);
        adlx_int minFan;
        fan->GetMinFanSpeed(&minFan);
        std::cout << std::left << std::setw(15) << " [Fan]" 
                  << "Zero RPM: " << (zeroRPM ? "\033[1;32mON\033[0m" : "\033[1;31mOFF\033[0m") 
                  << "  Min Speed: " << minFan << " RPM" << std::endl;
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
                  << "Power Limit: " << (powerLimit >= 0 ? "+" : "") << powerLimit << "%" << std::endl;
    }
    std::cout << "--------------------------------------------------------" << std::endl;
}


// Each tuning parameter is an optional: an empty optional means "the caller did
// not ask to change this, leave it alone", a present one means "apply exactly
// this value". This separates "was it requested" from "what is the value", so
// every number is legal - including negative core/voltage offsets (RDNA4) and 0.
void ApplySettings(IADLXGPUPtr gpu, IADLXGPUTuningServicesPtr tuningServices,
                   std::optional<int> coreMaxFreq, std::optional<int> coreMinFreq,
                   std::optional<int> voltage, std::optional<int> vramFreq,
                   std::optional<int> memTiming, std::optional<int> powerLimit,
                   std::optional<int> zeroRPM) {
    IADLXInterfacePtr ifc;
    std::cout << "\n\033[1;33m[*] Applying Settings...\033[0m" << std::endl;

    tuningServices->GetManualGFXTuning(gpu, &ifc);
    IADLXManualGraphicsTuning2Ptr gfx2(ifc);
    if (gfx2) {
        if (coreMaxFreq) {
            gfx2->SetGPUMaxFrequency(*coreMaxFreq);
            std::cout << " -> GFX Max Frequency: " << *coreMaxFreq << " MHz" << std::endl;
        }
        if (coreMinFreq) {
            gfx2->SetGPUMinFrequency(*coreMinFreq);
            std::cout << " -> GFX Min Frequency: " << *coreMinFreq << " MHz" << std::endl;
        }
        if (voltage) {
            ADLX_RESULT res = gfx2->SetGPUVoltage(*voltage);
            if (ADLX_SUCCEEDED(res))
                std::cout << " -> GFX Voltage (Offset): " << *voltage << " mV" << std::endl;
            else
                std::cerr << " [!] Failed to set Voltage: " << *voltage << " (Error: " << res << ")" << std::endl;
        }
    }

    if (vramFreq || memTiming) {
        tuningServices->GetManualVRAMTuning(gpu, &ifc);
        IADLXManualVRAMTuning2Ptr vram2(ifc);
        IADLXManualVRAMTuning1Ptr vram1(ifc);
        if (vram2 && vramFreq) {
            vram2->SetMaxVRAMFrequency(*vramFreq);
            std::cout << " -> VRAM Max Frequency: " << *vramFreq << " MHz" << std::endl;
        }
        if (memTiming) {
            const ADLX_MEMORYTIMING_DESCRIPTION mt = (ADLX_MEMORYTIMING_DESCRIPTION)*memTiming;
            adlx_bool mtSupported = false;
            if (vram2)      vram2->IsSupportedMemoryTiming(&mtSupported);
            else if (vram1) vram1->IsSupportedMemoryTiming(&mtSupported);

            if (!mtSupported) {
                std::cerr << " [!] Memory timing control not supported on this GPU." << std::endl;
            } else {
                // The ADLX enum is a superset across architectures; reject a
                // preset this card doesn't expose with a clear, listed message
                // rather than a raw ADLX error.
                std::vector<ADLX_MEMORYTIMING_DESCRIPTION> presets;
                const bool haveList = GetSupportedMemTimings(vram2, vram1, presets);
                bool allowed = !haveList;  // list unavailable -> let ADLX decide
                for (auto d : presets) if (d == mt) allowed = true;

                if (!allowed) {
                    std::cerr << " [!] Memory timing '" << MemTimingName(mt)
                              << "' not supported on this GPU. Supported: "
                              << JoinMemTimingNames(presets) << std::endl;
                } else {
                    ADLX_RESULT res = vram2 ? vram2->SetMemoryTimingDescription(mt)
                                            : vram1->SetMemoryTimingDescription(mt);
                    if (ADLX_SUCCEEDED(res))
                        std::cout << " -> VRAM Memory Timing: " << MemTimingName(mt) << std::endl;
                    else
                        std::cerr << " [!] Failed to set Memory Timing: " << MemTimingName(mt)
                                  << " (Error: " << res << ")" << std::endl;
                }
            }
        }
    }

    if (powerLimit) {
        tuningServices->GetManualPowerTuning(gpu, &ifc);
        IADLXManualPowerTuningPtr power(ifc);
        if (power) {
            ADLX_RESULT res = power->SetPowerLimit(*powerLimit);
            if (ADLX_SUCCEEDED(res))
                std::cout << " -> Power Limit: " << (*powerLimit >= 0 ? "+" : "") << *powerLimit << "%" << std::endl;
            else
                std::cerr << " [!] Failed to set Power Limit: " << *powerLimit << " (Error: " << res << ")" << std::endl;
        }
    }

    if (zeroRPM) {
        tuningServices->GetManualFanTuning(gpu, &ifc);
        IADLXManualFanTuningPtr fan(ifc);
        if (fan) {
            fan->SetZeroRPMState(*zeroRPM == 1);
            std::cout << " -> Zero RPM: " << (*zeroRPM == 1 ? "ON" : "OFF") << std::endl;
        }
    }
    std::cout << "\033[1;32m[+] Successfully applied!\033[0m" << std::endl;
}

void LoadProfileOnGpu(IADLXGPUPtr gpu, IADLXGPUTuningServicesPtr tuningServices, const std::string& path) {
    GPUProfile profile;
    if (!ProfileParser::Parse(path, profile)) {
        std::cerr << "[!] Error parsing XML profile: " << path << std::endl;
        return;
    }

    std::cout << "\033[1;33m[*] Loading Clean Profile (Custom Mapping): \033[0m" << path << std::endl;

    std::optional<int> voltage, power;

    if (profile.features.count(12)) {
        voltage = profile.features[12].states[0].value;
        std::cout << " -> Found ID 12 (Undervolt): " << *voltage << std::endl;
    }

    if (profile.features.count(3)) {
        power = profile.features[3].states[0].value;
        std::cout << " -> Found ID 3 (Power Limit): " << *power << std::endl;
    }

    ApplySettings(gpu, tuningServices, std::nullopt, std::nullopt, voltage,
                  std::nullopt, std::nullopt, power, std::nullopt);
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
        adlx_int minFreq, maxFreq, voltage;
        gfx2->GetGPUMinFrequency(&minFreq);
        gfx2->GetGPUMaxFrequency(&maxFreq);
        gfx2->GetGPUVoltage(&voltage);
        std::cout << "core=" << maxFreq << "\ncoremin=" << minFreq << "\nvolt=" << voltage << "\n";
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
        if (mtSupported && ADLX_SUCCEEDED(mtRes))
            std::cout << "memtiming=" << MemTimingName(mt) << "\n";
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
        adlx_bool zeroRPM;
        fan->GetZeroRPMState(&zeroRPM);
        std::cout << "zerorpm=" << (zeroRPM ? 1 : 0) << "\n";
    }

    // Flush now. When stdout is a pipe (launched by the GUI) it is block-
    // buffered; these lines use "\n", not std::endl, so without an explicit
    // flush they sit in the buffer and are lost when ADLX teardown skips the
    // normal exit-time flush. From a console each "\n" flushes, hiding the bug.
    std::cout.flush();
}

int main(int argc, char* argv[]) {
    // Print Banner
    std::cout << "\033[1;31m" << "  ____           _ _____                 " << "\033[0m" << std::endl;
    std::cout << "\033[1;31m" << " |  _ \\ __ _  __| |_   _|   _ _ __   ___ " << "\033[0m" << std::endl;
    std::cout << "\033[1;31m" << " | |_) / _` |/ _` | | || | | | '_ \\ / _ \\" << "\033[0m" << std::endl;
    std::cout << "\033[1;31m" << " |  _ < (_| | (_| | | || |_| | | | |  __/" << "\033[0m" << std::endl;
    std::cout << "\033[1;31m" << " |_| \\_\\__,_|\\__,_| |_| \\__,_|_| |_|\\___|" << "\033[0m" << " v1.2.0 (ADLX)" << std::endl;

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
            }
        } else if (cmd == "-load" && argc > 2) {
            std::string path = argv[2];
            int targetGpu = 0;
            if (argc > 3) {
                 std::string gpuArg = argv[3];
                 if (gpuArg.find("gpu=") == 0) targetGpu = std::stoi(gpuArg.substr(4));
            }

            if (targetGpu < gpus->Size()) {
                IADLXGPUPtr gpu;
                gpus->At(targetGpu, &gpu);
                LoadProfileOnGpu(gpu, tuningServices, path);
            }
        } else if (cmd == "-set" && argc > 2) {
            int targetGpu = 0;
            std::optional<int> coreMaxFreq, coreMinFreq, voltage, vramFreq,
                               memTiming, powerLimit, zeroRPM;

            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg.find("gpu=") == 0) targetGpu = std::stoi(arg.substr(4));
                else if (arg.find("core=") == 0) coreMaxFreq = std::stoi(arg.substr(5));
                else if (arg.find("coremin=") == 0) coreMinFreq = std::stoi(arg.substr(8));
                else if (arg.find("volt=") == 0) voltage = std::stoi(arg.substr(5));
                else if (arg.find("vram=") == 0) vramFreq = std::stoi(arg.substr(5));
                else if (arg.find("memtiming=") == 0) {
                    const int mt = ParseMemTiming(arg.substr(10));
                    if (mt >= 0) memTiming = mt;
                    else std::cerr << " [!] Unknown memtiming value: " << arg.substr(10)
                                   << " (use default|fast|fast2|auto|level1|level2)" << std::endl;
                }
                else if (arg.find("power=") == 0) powerLimit = std::stoi(arg.substr(6));
                else if (arg.find("zerorpm=") == 0) zeroRPM = std::stoi(arg.substr(8));
            }

            if (targetGpu < gpus->Size()) {
                IADLXGPUPtr gpu;
                gpus->At(targetGpu, &gpu);
                ApplySettings(gpu, tuningServices, coreMaxFreq, coreMinFreq, voltage, vramFreq, memTiming, powerLimit, zeroRPM);
            } else {
                std::cerr << "Error: GPU index " << targetGpu << " out of range." << std::endl;
            }

        } else {
            std::cout << "Usage:" << std::endl;
            std::cout << "  RadTune -list" << std::endl;
            std::cout << "  RadTune -get [gpu=N]" << std::endl;
            std::cout << "  RadTune -set [gpu=N] [core=MHz] [coremin=MHz] [volt=mV] [vram=MHz] [memtiming=default|fast|fast2|auto|level1|level2] [power=%] [zerorpm=0|1]" << std::endl;
            std::cout << "  RadTune -load profile.xml [gpu=N]" << std::endl;
            std::cout << "  RadTune -schedule <logon|startup|daily=HH:MM> <-set ...|-load ...>" << std::endl;
            std::cout << "  RadTune -schedule status | remove" << std::endl;
        }
    } else {
        std::cout << "RadTune v1.2.0 (ADLX based)" << std::endl;
        std::cout << "Usage: RadTune [-list | -set ... | -load ...]" << std::endl;
    }

    // ADLX invalidates every outstanding interface when Terminate() runs;
    // releasing a smart pointer *after* that dereferences a freed vtable -> an
    // access violation on exit (see the WARNING in ADLXHelper.h). Release the
    // interfaces we still hold BEFORE terminating so teardown is clean. Any
    // per-GPU interfaces are already scoped inside the dispatch above.
    tuningServices = nullptr;
    gpus = nullptr;

    g_ADLX.Terminate();
    return 0;
}
