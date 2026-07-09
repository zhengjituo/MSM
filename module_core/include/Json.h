#ifndef MSM_JSON_H
#define MSM_JSON_H

#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <cstdlib>

namespace Json {

class Value {
public:
    Value() : type_(NULL_TYPE), intVal_(0), doubleVal_(0), boolVal_(false) {}
    Value(const std::string& v) : type_(STRING), strVal(v), intVal_(0), doubleVal_(0), boolVal_(false) {}
    Value(int v) : type_(INT), intVal_(v), doubleVal_(0), boolVal_(false) {}
    Value(double v) : type_(DOUBLE), strVal(""), intVal_(0), doubleVal_(v), boolVal_(false) {}
    Value(bool v) : type_(BOOL), strVal(""), intVal_(0), doubleVal_(0), boolVal_(v) {}

    enum Type { NULL_TYPE, STRING, INT, DOUBLE, BOOL, OBJECT, ARRAY };

    bool isNull() const { return type_ == NULL_TYPE; }
    bool isString() const { return type_ == STRING; }
    bool isInt() const { return type_ == INT; }
    bool isDouble() const { return type_ == DOUBLE || type_ == INT; }
    bool isBool() const { return type_ == BOOL; }
    bool isObject() const { return type_ == OBJECT; }
    bool isArray() const { return type_ == ARRAY; }

    std::string asString() const { return strVal; }
    int asInt() const { 
        if (type_ == INT) return intVal_;
        if (type_ == DOUBLE) return (int)doubleVal_;
        if (type_ == STRING) return std::atoi(strVal.c_str());
        return 0;
    }
    double asDouble() const {
        if (type_ == DOUBLE) return doubleVal_;
        if (type_ == INT) return (double)intVal_;
        if (type_ == STRING) return std::atof(strVal.c_str());
        return 0.0;
    }
    bool asBool() const { return boolVal_; }

    Value& operator=(const std::string& v) { type_ = STRING; strVal = v; return *this; }
    Value& operator=(int v) { type_ = INT; intVal_ = v; return *this; }
    Value& operator=(double v) { type_ = DOUBLE; doubleVal_ = v; return *this; }
    Value& operator=(bool v) { type_ = BOOL; boolVal_ = v; return *this; }

    Value& operator[](const std::string& key) {
        if (type_ != OBJECT) { type_ = OBJECT; objVal.clear(); }
        return objVal[key];
    }
    Value& operator[](int idx) {
        if (type_ != ARRAY) { type_ = ARRAY; arrVal.clear(); }
        while ((int)arrVal.size() <= idx) arrVal.push_back(Value());
        return arrVal[idx];
    }

    void append(const Value& v) {
        if (type_ != ARRAY) { type_ = ARRAY; arrVal.clear(); }
        arrVal.push_back(v);
    }

    bool has(const std::string& key) const {
        return type_ == OBJECT && objVal.find(key) != objVal.end();
    }

    std::string toStyledString() const {
        std::ostringstream oss;
        stringify(oss, 0);
        return oss.str();
    }

    static Value parse(const std::string& str) {
        Value v;
        size_t pos = 0;
        parseValue(str, pos, v);
        return v;
    }

    std::map<std::string, Value> objVal;
    std::vector<Value> arrVal;

private:
    Type type_;
    std::string strVal;
    int intVal_;
    double doubleVal_;
    bool boolVal_;

    void stringify(std::ostringstream& oss, int indent) const {
        std::string pad(indent, ' ');
        switch (type_) {
            case NULL_TYPE: oss << "null"; break;
            case STRING: oss << "\"" << strVal << "\""; break;
            case INT: oss << intVal_; break;
            case DOUBLE: oss << doubleVal_; break;
            case BOOL: oss << (boolVal_ ? "true" : "false"); break;
            case OBJECT:
                if (objVal.empty()) {
                    oss << "{}";
                } else {
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
            case ARRAY:
                if (arrVal.empty()) {
                    oss << "[]";
                } else {
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

    static bool parseValue(const std::string& str, size_t& pos, Value& v) {
        skipSpace(str, pos);
        if (pos >= str.size()) return false;

        char c = str[pos];
        if (c == '{') {
            pos++;
            v.type_ = OBJECT;
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
                Value child;
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
            v.type_ = ARRAY;
            v.arrVal.clear();
            skipSpace(str, pos);
            if (pos < str.size() && str[pos] == ']') { pos++; return true; }
            while (pos < str.size()) {
                Value child;
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
            v.type_ = STRING;
            v.strVal = str.substr(pos, end - pos);
            pos = end + 1;
            return true;
        }
        if (std::isdigit(c) || (c == '-' && std::isdigit(str[pos+1]))) {
            size_t start = pos;
            if (c == '-') pos++;
            while (pos < str.size() && (std::isdigit(str[pos]) || str[pos] == '.')) pos++;
            std::string num = str.substr(start, pos - start);
            if (num.find('.') != std::string::npos) {
                v.type_ = DOUBLE;
                v.doubleVal_ = std::atof(num.c_str());
            } else {
                v.type_ = INT;
                v.intVal_ = std::atoi(num.c_str());
            }
            return true;
        }
        if (str.compare(pos, 4, "null") == 0) { pos += 4; v.type_ = NULL_TYPE; return true; }
        if (str.compare(pos, 4, "true") == 0) { pos += 4; v.type_ = BOOL; v.boolVal_ = true; return true; }
        if (str.compare(pos, 5, "false") == 0) { pos += 5; v.type_ = BOOL; v.boolVal_ = false; return true; }
        return false;
    }
};

} // namespace Json

#endif