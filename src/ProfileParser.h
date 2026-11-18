#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <utility>

struct FeatureState {
    int id;
    bool enabled;
    int value;
};

struct Feature {
    int id;
    bool enabled;
    std::vector<FeatureState> states;
};

// Adrenalin FEATURE IDs that -load maps (reverse-engineered from real exports;
// see .agent/architecture.md "Profile feature IDs").
namespace ProfileFeature {
    constexpr int PowerLimit = 3;    // STATE 0 = power limit %
    constexpr int Voltage    = 12;   // STATE 0 = GPU voltage mV
    constexpr int FanCurve   = 22;   // STATEs alternate temperature C (even ID) / speed % (odd ID)
}

struct GPUProfile {
    std::string devId;
    std::string revId;
    std::map<int, Feature> features;

    // Value of STATE `stateId` in FEATURE `featureId`, if the profile has it.
    std::optional<int> StateValue(int featureId, int stateId) const;

    // The fan curve as (temperature C, speed %) points, from FEATURE 22. Its
    // STATEs come in pairs (0,1), (2,3), ...; a trailing unpaired STATE (ID 10
    // in every export seen, always 0) is not a curve point. Empty if absent.
    std::vector<std::pair<int, int>> FanCurvePoints() const;
};

class ProfileParser {
public:
    // False if the file can't be opened, has no <GPU> section, or is malformed.
    static bool Parse(const std::string& filePath, GPUProfile& profile);
private:
    static std::string GetAttribute(const std::string& tagBody, const std::string& attrName);
};
