#include "Module.h"
#include "MessageBus.h"
#include "NIStringParser.h"
#include <fstream>
#include <algorithm>

namespace msm {

// ─── VarTable ─────────────────────────────────────────────────

void VarTable::declare(const std::string& name, const std::string& type,
                       std::function<std::string()> reader,
                       std::function<bool(const std::string&)> writer,
                       const std::string& dir)
{
    entries_[name] = {type, dir, std::move(reader), std::move(writer)};
}

std::string VarTable::read(const std::string& name) const {
    auto it = entries_.find(name);
    return it != entries_.end() ? it->second.read() : "";
}

bool VarTable::write(const std::string& name, const std::string& value) {
    auto it = entries_.find(name);
    if (it == entries_.end() || !it->second.write) return false;
    return it->second.write(value);
}

bool VarTable::has(const std::string& name) const {
    return entries_.find(name) != entries_.end();
}

std::map<std::string, std::string> VarTable::readAll() const {
    std::map<std::string, std::string> m;
    for (auto& [k, v] : entries_)
        m[k] = v.read();
    return m;
}

std::map<std::string, std::string> VarTable::allTypes() const {
    std::map<std::string, std::string> m;
    for (auto& [k, v] : entries_)
        m[k] = v.type;
    return m;
}

std::vector<std::string> VarTable::names() const {
    std::vector<std::string> n;
    for (auto& [k, v] : entries_)
        n.push_back(k);
    return n;
}

// Template specializations
template<> std::string VarTable::typeName<int32_t>() { return "int32"; }
template<> std::string VarTable::typeName<int64_t>() { return "int64"; }
template<> std::string VarTable::typeName<unsigned long>() { return "uint64"; }
template<> std::string VarTable::typeName<double>()  { return "double"; }
template<> std::string VarTable::typeName<bool>()    { return "bool"; }
template<> std::string VarTable::typeName<std::string>() { return "string"; }

template<> std::string VarTable::valToStr<int32_t>(const int32_t& v) { return std::to_string(v); }
template<> std::string VarTable::valToStr<int64_t>(const int64_t& v) { return std::to_string(v); }
template<> std::string VarTable::valToStr<unsigned long>(const unsigned long& v) { return std::to_string(v); }
template<> std::string VarTable::valToStr<double>(const double& v) { return std::to_string(v); }
template<> std::string VarTable::valToStr<bool>(const bool& v) { return v ? "1" : "0"; }
template<> std::string VarTable::valToStr<std::string>(const std::string& v) { return v; }

template<> int32_t VarTable::strToVal<int32_t>(const std::string& s) { return std::stoi(s); }
template<> int64_t VarTable::strToVal<int64_t>(const std::string& s) { return std::stoll(s); }
template<> unsigned long VarTable::strToVal<unsigned long>(const std::string& s) { return std::stoull(s); }
template<> double  VarTable::strToVal<double>(const std::string& s) { return std::stod(s); }
template<> bool    VarTable::strToVal<bool>(const std::string& s) {
    std::string lc = s;
    std::transform(lc.begin(), lc.end(), lc.begin(), ::tolower);
    return lc == "1" || lc == "true" || lc == "yes";
}
template<> std::string VarTable::strToVal<std::string>(const std::string& s) { return s; }

std::string VarTable::toJSON(const std::string& prefix,
                              const std::string& moduleName) const
{
    std::ostringstream os;
    os << "{\n";
    os << "    \"prefix\": \"" << prefix << "\",\n";
    os << "    \"snapshot_interval\": 1,\n";
    os << "    \"modules\": [\n";
    os << "        {\n";
    os << "            \"name\": \"" << moduleName << "\",\n";
    os << "            \"variables\": [\n";

    bool first = true;
    for (auto& [name, entry] : entries_) {
        if (!first) os << ",\n";
        first = false;
        os << "                {"
           << "\"name\":\"" << name << "\","
           << "\"type\":\"" << entry.type << "\","
           << "\"dir\":\"" << entry.dir << "\""
           << "}";
    }

    os << "\n            ]\n";
    os << "        }\n";
    os << "    ]\n";
    os << "}\n";
    return os.str();
}

bool VarTable::saveJSON(const std::string& filepath,
                         const std::string& prefix,
                         const std::string& moduleName) const
{
    std::ofstream f(filepath);
    if (!f) return false;
    f << toJSON(prefix, moduleName);
    return true;
}

// ─── Module ────────────────────────────────────────────────────

Module::Module(const std::string& name)
    : name_(name), running_(false) {
}

Module::~Module() {
    stop();
}

void Module::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::make_unique<std::thread>(&Module::runLoop, this);
}

void Module::stop() {
    if (!running_) return;
    running_ = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(Message{});
    }
    cv_.notify_one();
    if (thread_ && thread_->joinable()) {
        thread_->join();
        thread_.reset();
    }
}

void Module::post(const Message& msg) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(msg);
    }
    cv_.notify_one();
}

Message Module::post(const std::string& niString) {
    Message msg = NIStringParser::parse(niString, name_);
    if (msg.cmd.empty()) {
        Message t;
        t.cmd = "ParseError";
        return t;
    }
    if (msg.isAwaitResponse()) {
        int timeout = msg.getTimeout();
        if (timeout <= 0) timeout = 5000;
        return MessageBus::instance().sendAndWait(msg, timeout);
    }
    MessageBus::instance().send(msg);
    return Message{};
}

void Module::runLoop() {
    while (running_) {
        Message msg;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(idleDelayMs_),
                         [this]{ return !queue_.empty() || !running_; });
            if (!queue_.empty()) {
                msg = queue_.front();
                queue_.pop();
            }
        }

        if (!msg.cmd.empty()) {
            handleState(msg);
        } else if (running_) {
            onIdle();
        }
    }
    running_ = false;
}

} // namespace msm