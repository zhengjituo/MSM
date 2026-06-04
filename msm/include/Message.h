#ifndef MSM_MESSAGE_H
#define MSM_MESSAGE_H

#include <string>
#include <map>
#include <sstream>
#include <random>
#include <iomanip>

namespace msm {

class Json {
public:
    std::map<std::string, std::string> data;
    
    std::string get(const std::string& key, const std::string& defaultVal = "") const;
    int getInt(const std::string& key, int defaultVal = 0) const;
    double getDouble(const std::string& key, double defaultVal = 0.0) const;
    bool getBool(const std::string& key, bool defaultVal = false) const;
    
    void set(const std::string& key, const std::string& val);
    void set(const std::string& key, const char* val);
    void set(const std::string& key, int val);
    void set(const std::string& key, double val);
    void set(const std::string& key, bool val);
    
    bool contains(const std::string& key) const;
    void erase(const std::string& key);
    
    std::string dump() const;
};

struct Message {
    std::string cmd;
    Json data;
    
    Message() = default;
    Message(const std::string& c, const Json& d = Json{}) 
        : cmd(c), data(d) {}
    
    std::string getSender() const;
    std::string getTarget() const;
    uint64_t getCorrelationId() const;
    bool isAwaitResponse() const;
    int getTimeout() const;
    std::string getMode() const;
    
    void setSender(const std::string& s);
    void setTarget(const std::string& t);
    void setCorrelationId(uint64_t id);
    void setAwaitResponse(bool await);
    void setTimeout(int ms);
    void setMode(const std::string& m);
    
    std::string toString() const;
    std::string toNIString() const;
    
    bool isBroadcast() const;
    bool isRegister() const;
    bool isUnregister() const;
    bool isResponse() const;
    
    static uint64_t generateCorrelationId();
};

} // namespace msm

#endif