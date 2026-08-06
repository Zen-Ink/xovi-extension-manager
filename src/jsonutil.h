#pragma once

#include <map>
#include <string>
#include <vector>

namespace jsonutil {
    bool isObject(const std::string &json);
    std::string readString(const std::string &json, const std::string &key, const std::string &fallback = "");
    bool readBool(const std::string &json, const std::string &key, bool fallback);
    int readInt(const std::string &json, const std::string &key, int fallback);
    bool hasKey(const std::string &json, const std::string &key);
    std::string readObject(const std::string &json, const std::string &key);
    std::vector<std::string> readStringArray(const std::string &json, const std::string &key);
    std::map<std::string, std::string> readStringObject(const std::string &json, const std::string &key);
    std::string escape(const std::string &value);
    std::string stringArray(const std::vector<std::string> &values);
    std::string stringObject(const std::map<std::string, std::string> &values);
}
