#ifndef MSM_JSON_VALUE_H
#define MSM_JSON_VALUE_H

#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <cstdlib>

namespace msm {

class JsonValue {
public:
    JsonValue() : type_(TYPE_NULL), intVal_(0), doubleVal_(0), boolVal_(false) {}
    JsonValue(const std::string& v) : type_(TYPE_STRING), strVal(v), intVal_(0), doubleVal_(0), boolVal_(false) {}
    JsonValue(int v) : type_(TYPE_INT), intVal_(v), doubleVal_(0), boolVal_(false) {}
    JsonValue(double v) : type_(TYPE_DOUBLE), strVal(""), intVal_(0), doubleVal_(v), boolVal_(false) {}
    JsonValue(bool v) : type_(TYPE_BOOL), strVal(""), intVal_(0), doubleVal_(0), boolVal_(v) {}

    enum Type { TYPE_NULL, TYPE_STRING, TYPE_INT, TYPE_DOUBLE, TYPE_BOOL, TYPE_OBJECT, TYPE_ARRAY };

    bool isNull() const { return type_ == TYPE_NULL; }
    bool isString() const { return type_ == TYPE_STRING; }
    bool isInt() const { return type_ == TYPE_INT; }
    bool isDouble() const { return type_ == TYPE_DOUBLE || type_ == TYPE_INT; }
    bool isBool() const { return type_ == TYPE_BOOL; }
    bool isObject() const { return type_ == TYPE_OBJECT; }
    bool isArray() const { return type_ == TYPE_ARRAY; }

    std::string asString() const { return strVal; }
    int asInt() const { 
        if (type_ == TYPE_INT) return intVal_;
        if (type_ == TYPE_DOUBLE) return (int)doubleVal_;
        if (type_ == TYPE_STRING) return std::atoi(strVal.c_str());
        return 0;
    }
    double asDouble() const {
        if (type_ == TYPE_DOUBLE) return doubleVal_;
        if (type_ == TYPE_INT) return (double)intVal_;
        if (type_ == TYPE_STRING) return std::atof(strVal.c_str());
        return 0.0;
    }
    bool asBool() const { return boolVal_; }

    JsonValue& operator=(const std::string& v) { type_ = TYPE_STRING; strVal = v; return *this; }
    JsonValue& operator=(int v) { type_ = TYPE_INT; intVal_ = v; return *this; }
    JsonValue& operator=(double v) { type_ = TYPE_DOUBLE; doubleVal_ = v; return *this; }
    JsonValue& operator=(bool v) { type_ = TYPE_BOOL; boolVal_ = v; return *this; }

    JsonValue& operator[](const std::string& key) {
        if (type_ != TYPE_OBJECT) { type_ = TYPE_OBJECT; objVal.clear(); }
        return objVal[key];
    }
    JsonValue& operator[](int idx) {
        if (type_ != TYPE_ARRAY) { type_ = TYPE_ARRAY; arrVal.clear(); }
        while ((int)arrVal.size() <= idx) arrVal.push_back(JsonValue());
        return arrVal[idx];
    }

    void append(const JsonValue& v) {
        if (type_ != TYPE_ARRAY) { type_ = TYPE_ARRAY; arrVal.clear(); }
        arrVal.push_back(v);
    }

    bool has(const std::string& key) const {
        return type_ == TYPE_OBJECT && objVal.find(key) != objVal.end();
    }

    std::string toStyledString() const {
        std::ostringstream oss;
        stringify(oss, 0);
        return oss.str();
    }

    static JsonValue parse(const std::string& str) {
        JsonValue v;
        size_t pos = 0;
        parseValue(str, pos, v);
        return v;
    }

    std::map<std::string, JsonValue> objVal;
    std::vector<JsonValue> arrVal;

private:
    Type type_;
    std::string strVal;
    int intVal_;
    double doubleVal_;
    bool boolVal_;

    void stringify(std::ostringstream& oss, int indent) const {
        std::string pad(indent, ' ');
        switch (type_) {
            case TYPE_NULL: oss << "null"; break;
            case TYPE_STRING: oss << "\"" << strVal << "\""; break;
            case TYPE_INT: oss << intVal_; break;
            case TYPE_DOUBLE: oss << doubleVal_; break;
            case TYPE_BOOL: oss << (boolVal_ ? "true" : "false"); break;
            case TYPE_OBJECT:
                if (objVal.empty()) { oss << "{}"; }
                else {
                    oss << "{\n";
                    int i = 0;
                    for (auto& kv : objVal) {
                        oss << pad << "  \"" << kv.first << "\": ";
                        kv.second.stringify(oss, indent + 2);
                        if (++i < (int)objVal.size()) oss << ",";
                        oss << "\n";
                    }
                    oss << pad << "}";
                }
                break;
            case TYPE_ARRAY:
                if (arrVal.empty()) { oss << "[]"; }
                else {
                    oss << "[\n";
                    for (size_t i = 0; i < arrVal.size(); i++) {
                        oss << pad << "  ";
                        arrVal[i].stringify(oss, indent + 2);
                        if (i + 1 < arrVal.size()) oss << ",";
                        oss << "\n";
                    }
                    oss << pad << "]";
                }
                break;
        }
    }

    static void skipSpace(const std::string& str, size_t& pos) {
        while (pos < str.size() && std::isspace(str[pos])) pos++;
    }

    static bool parseValue(const std::string& str, size_t& pos, JsonValue& v) {
        skipSpace(str, pos);
        if (pos >= str.size()) return false;

        char c = str[pos];
        if (c == '{') {
            pos++;
            v.type_ = TYPE_OBJECT;
            v.objVal.clear();
            skipSpace(str, pos);
            if (pos < str.size() && str[pos] == '}') { pos++; return true; }
            while (pos < str.size()) {
                skipSpace(str, pos);
                if (str[pos] != '"') return false;
                pos++;
                size_t end = str.find('"', pos);
                if (end == std::string::npos) return false;
                std::string key = str.substr(pos, end - pos);
                pos = end + 1;
                skipSpace(str, pos);
                if (pos >= str.size() || str[pos] != ':') return false;
                pos++;
                JsonValue child;
                if (!parseValue(str, pos, child)) return false;
                v.objVal[key] = child;
                skipSpace(str, pos);
                if (str[pos] == '}') { pos++; return true; }
                if (str[pos] == ',') pos++;
            }
            return true;
        }
        if (c == '[') {
            pos++;
            v.type_ = TYPE_ARRAY;
            v.arrVal.clear();
            skipSpace(str, pos);
            if (pos < str.size() && str[pos] == ']') { pos++; return true; }
            while (pos < str.size()) {
                JsonValue child;
                if (!parseValue(str, pos, child)) return false;
                v.arrVal.push_back(child);
                skipSpace(str, pos);
                if (str[pos] == ']') { pos++; return true; }
                if (str[pos] == ',') pos++;
            }
            return true;
        }
        if (c == '"') {
            pos++;
            size_t end = str.find('"', pos);
            if (end == std::string::npos) return false;
            v.type_ = TYPE_STRING;
            v.strVal = str.substr(pos, end - pos);
            pos = end + 1;
            return true;
        }
        if (std::isdigit(c) || (c == '-' && pos + 1 < str.size() && std::isdigit(str[pos+1]))) {
            size_t start = pos;
            if (c == '-') pos++;
            while (pos < str.size() && (std::isdigit(str[pos]) || str[pos] == '.')) pos++;
            std::string num = str.substr(start, pos - start);
            if (num.find('.') != std::string::npos) {
                v.type_ = TYPE_DOUBLE;
                v.doubleVal_ = std::atof(num.c_str());
            } else {
                v.type_ = TYPE_INT;
                v.intVal_ = std::atoi(num.c_str());
            }
            return true;
        }
        if (str.compare(pos, 4, "null") == 0) { pos += 4; v.type_ = TYPE_NULL; return true; }
        if (str.compare(pos, 4, "true") == 0) { pos += 4; v.type_ = TYPE_BOOL; v.boolVal_ = true; return true; }
        if (str.compare(pos, 5, "false") == 0) { pos += 5; v.type_ = TYPE_BOOL; v.boolVal_ = false; return true; }
        return false;
    }
};

} // namespace msm

#endif