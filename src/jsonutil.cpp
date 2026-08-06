#include "jsonutil.h"

#include <cctype>
#include <cstring>
#include <sstream>

namespace {
    const char *skipWs(const char *cursor) {
        while(cursor != nullptr && *cursor != 0 && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
        return cursor;
    }

    int hexValue(char c) {
        if(c >= '0' && c <= '9') return c - '0';
        if(c >= 'a' && c <= 'f') return c - 'a' + 10;
        if(c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool parseStringAt(const char *&cursor, std::string &out) {
        cursor = skipWs(cursor);
        if(cursor == nullptr || *cursor != '"') return false;
        ++cursor;
        out.clear();
        while(*cursor != 0) {
            char c = *cursor++;
            if(c == '"') return true;
            if(c != '\\') {
                out.push_back(c);
                continue;
            }

            char escaped = *cursor++;
            switch(escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    int value = 0;
                    bool valid = true;
                    for(int i = 0; i < 4; ++i) {
                        int digit = hexValue(cursor[i]);
                        if(digit < 0) valid = false;
                        value = (value << 4) | (digit < 0 ? 0 : digit);
                    }
                    cursor += 4;
                    out.push_back(valid && value > 0 && value < 0x80 ? static_cast<char>(value) : '?');
                    break;
                }
                default:
                    out.push_back(escaped);
                    break;
            }
        }
        return false;
    }

    const char *findKeyValue(const std::string &json, const std::string &key) {
        const char *cursor = json.c_str();
        while((cursor = std::strchr(cursor, '"')) != nullptr) {
            const char *entryStart = cursor;
            std::string foundKey;
            if(!parseStringAt(cursor, foundKey)) return nullptr;
            const char *afterKey = skipWs(cursor);
            if(foundKey == key && afterKey != nullptr && *afterKey == ':') {
                return skipWs(afterKey + 1);
            }
            cursor = entryStart + 1;
        }
        return nullptr;
    }
}

namespace jsonutil {
    bool isObject(const std::string &json) {
        const char *cursor = skipWs(json.c_str());
        if(cursor == nullptr || *cursor != '{') return false;
        const char *end = json.c_str() + json.size();
        while(end > cursor && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
        return end > cursor && *(end - 1) == '}';
    }

    bool hasKey(const std::string &json, const std::string &key) {
        return findKeyValue(json, key) != nullptr;
    }

    std::string readString(const std::string &json, const std::string &key, const std::string &fallback) {
        const char *value = findKeyValue(json, key);
        std::string parsed;
        return parseStringAt(value, parsed) ? parsed : fallback;
    }

    bool readBool(const std::string &json, const std::string &key, bool fallback) {
        const char *value = findKeyValue(json, key);
        if(value == nullptr) return fallback;
        if(std::strncmp(value, "true", 4) == 0) return true;
        if(std::strncmp(value, "false", 5) == 0) return false;
        return fallback;
    }

    int readInt(const std::string &json, const std::string &key, int fallback) {
        const char *cursor = skipWs(findKeyValue(json, key));
        if(cursor == nullptr) return fallback;

        int sign = 1;
        if(*cursor == '-') {
            sign = -1;
            ++cursor;
        }
        if(!std::isdigit(static_cast<unsigned char>(*cursor))) return fallback;

        int value = 0;
        while(std::isdigit(static_cast<unsigned char>(*cursor))) {
            value = (value * 10) + (*cursor - '0');
            ++cursor;
        }
        return value * sign;
    }

    std::string readObject(const std::string &json, const std::string &key) {
        const char *cursor = skipWs(findKeyValue(json, key));
        if(cursor == nullptr || *cursor != '{') return "";

        const char *start = cursor;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        while(*cursor != 0) {
            char c = *cursor++;
            if(inString) {
                if(escaped) {
                    escaped = false;
                } else if(c == '\\') {
                    escaped = true;
                } else if(c == '"') {
                    inString = false;
                }
                continue;
            }
            if(c == '"') {
                inString = true;
            } else if(c == '{') {
                ++depth;
            } else if(c == '}') {
                --depth;
                if(depth == 0) {
                    return std::string(start, cursor - start);
                }
            }
        }
        return "";
    }

    std::vector<std::string> readStringArray(const std::string &json, const std::string &key) {
        std::vector<std::string> values;
        const char *cursor = skipWs(findKeyValue(json, key));
        if(cursor == nullptr || *cursor != '[') return values;
        ++cursor;
        for(;;) {
            cursor = skipWs(cursor);
            if(cursor == nullptr || *cursor == 0 || *cursor == ']') break;
            std::string value;
            if(parseStringAt(cursor, value)) values.push_back(value);
            cursor = skipWs(cursor);
            if(cursor == nullptr || *cursor != ',') break;
            ++cursor;
        }
        return values;
    }

    std::map<std::string, std::string> readStringObject(const std::string &json, const std::string &key) {
        std::map<std::string, std::string> values;
        const char *cursor = skipWs(findKeyValue(json, key));
        if(cursor == nullptr || *cursor != '{') return values;
        ++cursor;
        for(;;) {
            cursor = skipWs(cursor);
            if(cursor == nullptr || *cursor == 0 || *cursor == '}') break;

            std::string objectKey;
            if(!parseStringAt(cursor, objectKey)) break;
            cursor = skipWs(cursor);
            if(cursor == nullptr || *cursor != ':') break;
            ++cursor;

            std::string objectValue;
            if(!parseStringAt(cursor, objectValue)) break;
            values[objectKey] = objectValue;

            cursor = skipWs(cursor);
            if(cursor == nullptr || *cursor != ',') break;
            ++cursor;
        }
        return values;
    }

    std::string escape(const std::string &value) {
        std::ostringstream out;
        for(unsigned char c : value) {
            switch(c) {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if(c < 0x20) {
                        const char *hex = "0123456789abcdef";
                        out << "\\u00" << hex[(c >> 4) & 0xF] << hex[c & 0xF];
                    } else {
                        out << static_cast<char>(c);
                    }
                    break;
            }
        }
        return out.str();
    }

    std::string stringArray(const std::vector<std::string> &values) {
        std::ostringstream out;
        out << "[";
        for(size_t i = 0; i < values.size(); ++i) {
            if(i != 0) out << ",";
            out << "\"" << escape(values[i]) << "\"";
        }
        out << "]";
        return out.str();
    }

    std::string stringObject(const std::map<std::string, std::string> &values) {
        std::ostringstream out;
        out << "{";
        bool first = true;
        for(const auto &entry : values) {
            if(!first) out << ",";
            first = false;
            out << "\"" << escape(entry.first) << "\":\"" << escape(entry.second) << "\"";
        }
        out << "}";
        return out.str();
    }
}
