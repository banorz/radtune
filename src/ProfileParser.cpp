#include "ProfileParser.h"
#include <fstream>
#include <sstream>
#include <iostream>

bool ProfileParser::Parse(const std::string& filePath, GPUProfile& profile) {
    std::ifstream file(filePath);
    if (!file.is_open()) return false;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string xml = buffer.str();

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

    return true;
}

std::string ProfileParser::GetAttribute(const std::string& tagBody, const std::string& attrName) {
    size_t attrPos = tagBody.find(attrName + "=\"");
    if (attrPos == std::string::npos) return "";
    size_t start = attrPos + attrName.length() + 2;
    size_t end = tagBody.find("\"", start);
    return tagBody.substr(start, end - start);
}
