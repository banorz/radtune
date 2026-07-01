#include "Scheduler.h"

#include <Windows.h>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>

namespace {

std::string XmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

// Re-serialize the payload into a command line. Arguments containing spaces are
// quoted so they survive as a single token when the task fires (e.g. -load paths).
std::string JoinPayload(const std::vector<std::string>& payload) {
    std::string joined;
    for (size_t i = 0; i < payload.size(); ++i) {
        if (i) joined += ' ';
        const std::string& a = payload[i];
        if (a.find(' ') != std::string::npos)
            joined += "\"" + a + "\"";
        else
            joined += a;
    }
    return joined;
}

std::string GetExePath() {
    char buf[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf);
}

std::string TempXmlPath() {
    char tmp[MAX_PATH] = { 0 };
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "radtune_task.xml";
}

bool BuildTriggerXml(const std::string& trigger, std::string& triggerXml, std::string& error) {
    if (trigger == "logon") {
        triggerXml = "    <LogonTrigger>\n      <Enabled>true</Enabled>\n    </LogonTrigger>\n";
        return true;
    }
    if (trigger == "startup") {
        triggerXml = "    <BootTrigger>\n      <Enabled>true</Enabled>\n    </BootTrigger>\n";
        return true;
    }
    if (trigger.rfind("daily=", 0) == 0) {
        const std::string hhmm = trigger.substr(6);   // expect HH:MM
        if (hhmm.size() != 5 || hhmm[2] != ':') {
            error = "Invalid time for daily, expected daily=HH:MM (e.g. daily=09:00)";
            return false;
        }
        // The date part of StartBoundary only needs to be in the past for a daily
        // schedule to be active; a fixed past date keeps the trigger deterministic.
        triggerXml =
            "    <CalendarTrigger>\n"
            "      <StartBoundary>2020-01-01T" + hhmm + ":00</StartBoundary>\n"
            "      <Enabled>true</Enabled>\n"
            "      <ScheduleByDay>\n"
            "        <DaysInterval>1</DaysInterval>\n"
            "      </ScheduleByDay>\n"
            "    </CalendarTrigger>\n";
        return true;
    }
    error = "Unknown trigger '" + trigger + "' (use: logon | startup | daily=HH:MM)";
    return false;
}

std::string BuildTaskXml(const std::string& triggerXml, const std::string& exePath, const std::string& args) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        << "  <RegistrationInfo>\n"
        << "    <Description>Re-applies AMD GPU tuning automatically (RadTune)</Description>\n"
        << "  </RegistrationInfo>\n"
        << "  <Triggers>\n" << triggerXml << "  </Triggers>\n"
        << "  <Principals>\n"
        << "    <Principal id=\"Author\">\n"
        << "      <LogonType>InteractiveToken</LogonType>\n"
        << "      <RunLevel>HighestAvailable</RunLevel>\n"
        << "    </Principal>\n"
        << "  </Principals>\n"
        << "  <Settings>\n"
        << "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
        << "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
        << "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
        << "    <StartWhenAvailable>true</StartWhenAvailable>\n"
        << "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
        << "    <Enabled>true</Enabled>\n"
        << "  </Settings>\n"
        << "  <Actions Context=\"Author\">\n"
        << "    <Exec>\n"
        << "      <Command>" << XmlEscape(exePath) << "</Command>\n"
        << "      <Arguments>" << XmlEscape(args) << "</Arguments>\n"
        << "    </Exec>\n"
        << "  </Actions>\n"
        << "</Task>\n";
    return xml.str();
}

// system() forwards to `cmd /c <command>`. cmd only strips outer quotes when the
// whole string both starts and ends with one; our commands start with `schtasks`
// and end with a flag, so inner path quotes are preserved as-is.
int RunSchtasks(const std::string& cmdline) {
    return std::system(cmdline.c_str());
}

} // namespace

namespace Scheduler {

bool Install(const std::string& trigger, const std::vector<std::string>& payload, std::string& error) {
    if (payload.empty()) {
        error = "Nothing to schedule: provide a -set or -load command after the trigger.";
        return false;
    }
    const std::string& verb = payload[0];
    if (verb != "-set" && verb != "-load") {
        error = "Scheduled command must start with -set or -load (got '" + verb + "').";
        return false;
    }

    std::string triggerXml;
    if (!BuildTriggerXml(trigger, triggerXml, error))
        return false;

    const std::string xml = BuildTaskXml(triggerXml, GetExePath(), JoinPayload(payload));
    const std::string xmlPath = TempXmlPath();
    {
        std::ofstream f(xmlPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "Could not write temporary task file: " + xmlPath;
            return false;
        }
        f << xml;
    }

    const std::string cmd =
        "schtasks /create /tn \"" + std::string(TASK_NAME) + "\" /xml \"" + xmlPath + "\" /f";
    const int rc = RunSchtasks(cmd);
    std::remove(xmlPath.c_str());

    if (rc != 0) {
        error = "schtasks failed (code " + std::to_string(rc) + "). Creating a task with highest "
                "privileges usually requires running RadTune from an elevated (Administrator) console.";
        return false;
    }
    return true;
}

bool Remove(std::string& error) {
    const std::string cmd = "schtasks /delete /tn \"" + std::string(TASK_NAME) + "\" /f";
    if (RunSchtasks(cmd) != 0) {
        error = "Could not delete the task. It may not exist, or admin rights are required.";
        return false;
    }
    return true;
}

void Status() {
    const std::string cmd = "schtasks /query /tn \"" + std::string(TASK_NAME) + "\" /v /fo LIST";
    RunSchtasks(cmd);
}

} // namespace Scheduler
