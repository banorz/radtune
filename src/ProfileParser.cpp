#include "ProfileParser.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>

bool ProfileParser::Parse(const std::string& filePath, GPUProfile& profile) {
    std::ifstream file(filePath);
    if (!file.is_open()) return false;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string xml = buffer.str();

    // A missing/non-numeric ID or Value makes std::stoi throw: that is a
    // malformed profile, reported as a parse failure rather than a crash.
    try {
    size_t pos = 0;
    size_t gpuStart = xml.find("<GPU", pos);
    if (gpuStart == std::string::npos) return false;

    size_t gpuEndTag = xml.find(">", gpuStart);
    std::string gpuTag = xml.substr(gpuStart, gpuEndTag - gpuStart);
    profile.devId = GetAttribute(gpuTag, "DevID");
    profile.revId = GetAttribute(gpuTag, "RevID");

    pos = gpuEndTag + 1;
    while (true) {
        size_t featStart = xml.find("<FEATURE", pos);
        if (featStart == std::string::npos || featStart > xml.find("</GPU>", pos)) break;

        size_t featEndTag = xml.find(">", featStart);
        std::string featTag = xml.substr(featStart, featEndTag - featStart);
        
        Feature feat;
        feat.id = std::stoi(GetAttribute(featTag, "ID"));
        std::string enabledStr = GetAttribute(featTag, "Enabled");
        feat.enabled = (enabledStr == "True" || enabledStr == "1");

        size_t statesStart = xml.find("<STATES>", featEndTag);
        size_t statesEnd = xml.find("</STATES>", statesStart);
        if (statesStart != std::string::npos && statesEnd != std::string::npos) {
            size_t sPos = statesStart + 8;
            while (true) {
                size_t stateStart = xml.find("<STATE", sPos);
                if (stateStart == std::string::npos || stateStart > statesEnd) break;
                
                size_t stateEndTag = xml.find("/>", stateStart);
                if (stateEndTag == std::string::npos) stateEndTag = xml.find(">", stateStart);
                
                std::string stateTag = xml.substr(stateStart, stateEndTag - stateStart + 2);
                FeatureState fs;
                fs.id = std::stoi(GetAttribute(stateTag, "ID"));
                fs.value = std::stoi(GetAttribute(stateTag, "Value"));
                std::string sEnabled = GetAttribute(stateTag, "Enabled");
                fs.enabled = (sEnabled == "True" || sEnabled == "1");
                
                feat.states.push_back(fs);
                sPos = stateEndTag + 2;
            }
        }

        profile.features[feat.id] = feat;
        pos = featEndTag + 1;
    }
    } catch (const std::exception&) {
        return false;
    }

    return true;
}

std::optional<int> GPUProfile::StateValue(int featureId, int stateId) const {
    const auto f = features.find(featureId);
    if (f == features.end()) return std::nullopt;
    for (const FeatureState& s : f->second.states)
        if (s.id == stateId) return s.value;
    return std::nullopt;
}

std::vector<std::pair<int, int>> GPUProfile::FanCurvePoints() const {
    std::vector<std::pair<int, int>> curve;
    for (int i = 0;; i += 2) {
        const auto temp = StateValue(ProfileFeature::FanCurve, i);
        const auto speed = StateValue(ProfileFeature::FanCurve, i + 1);
        if (!temp || !speed) break;
        curve.emplace_back(*temp, *speed);
    }
    return curve;
}

std::string ProfileParser::GetAttribute(const std::string& tagBody, const std::string& attrName) {
    size_t attrPos = tagBody.find(attrName + "=\"");
    if (attrPos == std::string::npos) return "";
    size_t start = attrPos + attrName.length() + 2;
    size_t end = tagBody.find("\"", start);
    return tagBody.substr(start, end - start);
}
