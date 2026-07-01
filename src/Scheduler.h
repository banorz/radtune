#pragma once
#include <string>
#include <vector>

// Registers/removes a Windows Task Scheduler task that re-applies GPU tuning
// automatically. Backs the "-schedule" CLI verb. Does not depend on ADLX:
// it only wraps a RadTune apply command (-set / -load) into a scheduled task.
namespace Scheduler {

    constexpr const char* TASK_NAME = "RadTune";

    // trigger: "logon" | "startup" | "daily=HH:MM"
    // payload: the apply command to schedule, e.g. {"-set","core=2500"} or {"-load","C:\\p.xml"}
    // On failure returns false and fills `error`.
    bool Install(const std::string& trigger, const std::vector<std::string>& payload, std::string& error);

    // Deletes the scheduled task. On failure returns false and fills `error`.
    bool Remove(std::string& error);

    // Prints the current task definition (delegates to `schtasks /query`).
    void Status();
}
