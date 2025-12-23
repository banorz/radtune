#pragma once
#include <string>
#include <vector>
#include <map>

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

struct GPUProfile {
    std::string devId;
    std::string revId;
    std::map<int, Feature> features;
};

class ProfileParser {
public:
    static bool Parse(const std::string& filePath, GPUProfile& profile);
private:
    static std::string ExtractTagContent(const std::string& xml, const std::string& tag, size_t& pos);
    static std::string GetAttribute(const std::string& tagBody, const std::string& attrName);
};
