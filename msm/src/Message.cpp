#include "Message.h"
#include <sstream>

namespace msm {

std::string Json::get(const std::string& key, const std::string& defaultVal) const {
    auto it = data.find(key);
    if (it != data.end()) return it->second;
    return defaultVal;
}

int Json::getInt(const std::string& key, int defaultVal) const {
    auto it = data.find(key);
    if (it != data.end()) {
        try { return std::stoi(it->second); } catch (...) {}
    }
    return defaultVal;
}

double Json::getDouble(const std::string& key, double defaultVal) const {
    auto it = data.find(key);
    if (it != data.end()) {
        try { return std::stod(it->second); } catch (...) {}
    }
    return defaultVal;
}

bool Json::getBool(const std::string& key, bool defaultVal) const {
    auto it = data.find(key);
    if (it != data.end()) {
        return it->second == "true" || it->second == "1";
    }
    return defaultVal;
}

void Json::set(const std::string& key, const std::string& val) {
    data[key] = val;
}

void Json::set(const std::string& key, const char* val) {
    data[key] = val;
}

void Json::set(const std::string& key, int val) {
    data[key] = std::to_string(val);
}

void Json::set(const std::string& key, double val) {
    data[key] = std::to_string(val);
}

void Json::set(const std::string& key, bool val) {
    data[key] = val ? "true" : "false";
}

bool Json::contains(const std::string& key) const {
    return data.find(key) != data.end();
}

void Json::erase(const std::string& key) {
    data.erase(key);
}

std::string Json::dump() const {
    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (auto& p : data) {
        if (!first) ss << ", ";
        ss << "\"" << p.first << "\": \"" << p.second << "\"";
        first = false;
    }
    ss << "}";
    return ss.str();
}

std::string Message::getSender() const {
    return data.get("_sender");
}

std::string Message::getTarget() const {
    return data.get("_target");
}

uint64_t Message::getCorrelationId() const {
    return static_cast<uint64_t>(data.getInt("_correlationId", 0));
}

bool Message::isAwaitResponse() const {
    return data.getBool("_await", false);
}

int Message::getTimeout() const {
    return data.getInt("_timeout", -1);
}

std::string Message::getMode() const {
    return data.get("_mode", "call");
}

void Message::setSender(const std::string& s) {
    data.set("_sender", s);
}

void Message::setTarget(const std::string& t) {
    data.set("_target", t);
}

void Message::setCorrelationId(uint64_t id) {
    data.set("_correlationId", std::to_string(id));
}

void Message::setAwaitResponse(bool await) {
    data.set("_await", await);
}

void Message::setTimeout(int ms) {
    data.set("_timeout", ms);
}

void Message::setMode(const std::string& m) {
    data.set("_mode", m);
}

std::string Message::toString() const {
    return std::string("Message{cmd:\"") + cmd + "\", data:" + data.dump() + "}";
}

std::string Message::toNIString() const {
    std::string result = cmd;
    
    std::string argsStr;
    bool first = true;
    for (auto& p : data.data) {
        if (p.first.substr(0, 1) != "_") {
            if (!first) argsStr += ", ";
            argsStr += p.first + "=" + p.second;
            first = false;
        }
    }
    if (!argsStr.empty()) {
        result += " >> " + argsStr;
    }
    
    std::string target = getTarget();
    std::string mode = getMode();
    
    if (mode == "broadcast") {
        result += " -> <broadcast>";
    } else if (mode == "register") {
        result += " -><register>";
    } else if (mode == "unregister") {
        result += " -><unregister>";
    } else if (isAwaitResponse()) {
        int t = getTimeout();
        if (t >= 0 || t == -1) {
            result += " -@ " + target;
        } else {
            result += " -> " + target;
        }
    } else {
        result += " ->| " + target;
    }
    
    return result;
}

bool Message::isBroadcast() const {
    return getTarget() == "<broadcast>";
}

bool Message::isRegister() const {
    return getTarget() == "<register>";
}

bool Message::isUnregister() const {
    return getTarget() == "<unregister>";
}

bool Message::isResponse() const {
    return cmd == "Response";
}

uint64_t Message::generateCorrelationId() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist(1, 999999999);
    return dist(gen);
}

} // namespace msm