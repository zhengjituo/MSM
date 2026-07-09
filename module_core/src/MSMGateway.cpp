#include "MSMGateway.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <chrono>
#include <thread>

namespace msm {

MSMGateway::MSMGateway()
    : Module("MSMGateway"), repPort_(5555), pubPort_(5557) {}

MSMGateway::~MSMGateway() { stop(); }

bool MSMGateway::loadConfig(const std::string& jsonPath) {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        std::cerr << "[MSMGateway] Cannot open " << jsonPath << "\n";
        return false;
    }
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        std::cerr << "[MSMGateway] JSON parse error: " << e.what() << "\n";
        return false;
    }
    auto gw = j.find("gateway");
    if (gw == j.end() || !gw->is_object()) {
        std::cerr << "[MSMGateway] No 'gateway' section in config\n";
        return false;
    }
    repPort_ = gw->value("rep_port", 5555);
    pubPort_ = gw->value("pub_port", 5557);
    restPort_ = gw->value("rest_port", 8080);
    if (gw->contains("peers") && (*gw)["peers"].is_array()) {
        for (auto& p : (*gw)["peers"]) {
            addPeer(p["name"], p["endpoint"]);
        }
    }
    if (gw->contains("subscriptions") && (*gw)["subscriptions"].is_array()) {
        for (auto& s : (*gw)["subscriptions"]) {
            SubConfig sc;
            sc.name = s["name"];
            sc.endpoint = s["endpoint"];
            if (s.contains("bindings") && s["bindings"].is_array()) {
                for (auto& b : s["bindings"]) {
                    sc.bindings.push_back({b["remote_var"], b["local_var"]});
                }
            }
            subConfigs_.push_back(sc);
        }
    }
    return true;
}

void MSMGateway::addPeer(const std::string& name, const std::string& repEndpoint) {
    std::lock_guard<std::mutex> lock(peerMtx_);
    peerConfigs_.push_back({name, repEndpoint});
}

std::string MSMGateway::firstPeerName() const {
    return peerConfigs_.empty() ? "" : peerConfigs_[0].name;
}

void MSMGateway::start() {
    if (running_.load()) return;

    ctx_ = zmq_ctx_new();
    if (!ctx_) {
        std::cerr << "[MSMGateway] Failed to create ZMQ context\n";
        return;
    }

    {
        std::string addr = "tcp://*:" + std::to_string(repPort_);
        repSock_ = zmq_socket(ctx_, ZMQ_REP);
        if (!repSock_) { std::cerr << "[MSMGateway] Failed to create REP\n"; goto fail; }
        int linger = 0;
        zmq_setsockopt(repSock_, ZMQ_LINGER, &linger, sizeof(linger));
        if (zmq_bind(repSock_, addr.c_str()) != 0) {
            std::cerr << "[MSMGateway] Failed to bind REP " << addr << "\n";
            goto fail;
        }
    }

    {
        std::string addr = "tcp://*:" + std::to_string(pubPort_);
        pubSock_ = zmq_socket(ctx_, ZMQ_PUB);
        if (!pubSock_) { std::cerr << "[MSMGateway] Failed to create PUB\n"; goto fail; }
        int linger = 0;
        zmq_setsockopt(pubSock_, ZMQ_LINGER, &linger, sizeof(linger));
        if (zmq_bind(pubSock_, addr.c_str()) != 0) {
            std::cerr << "[MSMGateway] Failed to bind PUB " << addr << "\n";
            goto fail;
        }
    }

    running_ = true;

    {
        std::lock_guard<std::mutex> lock(peerMtx_);
        for (auto& cfg : peerConfigs_) {
            PeerRuntime pr;
            pr.name = cfg.name;

            pr.dealerSock = zmq_socket(ctx_, ZMQ_DEALER);
            if (!pr.dealerSock) {
                std::cerr << "[MSMGateway] Failed to create DEALER for " << cfg.name << "\n";
                continue;
            }
            {
                int linger = 0;
                zmq_setsockopt(pr.dealerSock, ZMQ_LINGER, &linger, sizeof(linger));
                int hwm = 1000;
                zmq_setsockopt(pr.dealerSock, ZMQ_SNDHWM, &hwm, sizeof(hwm));
            }
            if (zmq_connect(pr.dealerSock, cfg.repEndpoint.c_str()) != 0) {
                std::cerr << "[MSMGateway] Failed to connect DEALER to " << cfg.repEndpoint << "\n";
                zmq_close(pr.dealerSock);
                continue;
            }

            pr.dealerThread = std::thread(&MSMGateway::runDEALER, this, (int)peerRuntimes_.size());
            peerRuntimes_.push_back(std::move(pr));
            std::cout << "[MSMGateway] DEALER -> " << cfg.name
                      << " (" << cfg.repEndpoint << ")\n";
        }
    }

    for (int i = 0; i < (int)subConfigs_.size(); i++) {
        auto& sc = subConfigs_[i];
        void* subSock = zmq_socket(ctx_, ZMQ_SUB);
        if (!subSock) {
            std::cerr << "[MSMGateway] Failed to create SUB for " << sc.name << "\n";
            continue;
        }
        int linger = 0;
        zmq_setsockopt(subSock, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_setsockopt(subSock, ZMQ_SUBSCRIBE, "", 0);
        if (zmq_connect(subSock, sc.endpoint.c_str()) != 0) {
            std::cerr << "[MSMGateway] Failed to connect SUB to " << sc.endpoint << "\n";
            zmq_close(subSock);
            continue;
        }
        SubRuntime sr;
        sr.subSock = subSock;
        sr.subThread = std::thread(&MSMGateway::runSUB, this, i);
        subRuntimes_.push_back(std::move(sr));
        std::cout << "[MSMGateway] SUB <- " << sc.name
                  << " (" << sc.endpoint << ")\n";
    }

    repThread_ = std::thread(&MSMGateway::runREP, this);
    pubThread_ = std::thread(&MSMGateway::runPUB, this);

    std::cout << "[MSMGateway] REP tcp://*:" << repPort_
              << ", PUB tcp://*:" << pubPort_ << "\n";

    Module::start();
    return;

fail:
    if (repSock_) { zmq_close(repSock_); repSock_ = nullptr; }
    if (pubSock_) { zmq_close(pubSock_); pubSock_ = nullptr; }
    if (ctx_) { zmq_ctx_destroy(ctx_); ctx_ = nullptr; }
}

void MSMGateway::stop() {
    if (!running_.exchange(false)) return;

    if (repSock_) { zmq_close(repSock_); repSock_ = nullptr; }
    if (pubSock_) { zmq_close(pubSock_); pubSock_ = nullptr; }
    {
        std::lock_guard<std::mutex> lock(peerMtx_);
        for (auto& pr : peerRuntimes_) {
            if (pr.dealerSock) { zmq_close(pr.dealerSock); pr.dealerSock = nullptr; }
            if (pr.dealerThread.joinable()) pr.dealerThread.join();
        }
        peerRuntimes_.clear();
    }
    for (auto& sr : subRuntimes_) {
        if (sr.subSock) { zmq_close(sr.subSock); sr.subSock = nullptr; }
        if (sr.subThread.joinable()) sr.subThread.join();
    }
    subRuntimes_.clear();
    if (repThread_.joinable()) repThread_.join();
    if (pubThread_.joinable()) pubThread_.join();

    if (ctx_) { zmq_ctx_destroy(ctx_); ctx_ = nullptr; }

    std::cout << "[MSMGateway] Stopped\n";
    Module::stop();
}

void MSMGateway::handleState(const Message& msg) {
    std::string peer = msg.getRemote();
    if (!peer.empty()) {
        std::lock_guard<std::mutex> lock(peerMtx_);
        for (auto& pr : peerRuntimes_) {
            if (pr.name == peer) {
                nlohmann::json j = messageToJson(msg);
                if (msg.isAwaitResponse()) {
                    j["_await"] = true;
                    j["_correlationId"] = msg.getCorrelationId();
                }
                std::string payload = j.dump();
                zmq_send(pr.dealerSock, "", 0, ZMQ_SNDMORE);
                zmq_send(pr.dealerSock, payload.data(), payload.size(), 0);
                return;
            }
        }
        std::cerr << "[MSMGateway] Peer not found: " << peer << "\n";
        return;
    }

    if (!msg.cmd.empty()) {
        std::lock_guard<std::mutex> lock(pubMtx_);
        pubQueue_.push(messageToJson(msg).dump());
    }
}

ModuleVars* MSMGateway::getVars() { return &vars_; }

// ─── REP: receive requests from any client ─────────────────────

void MSMGateway::runREP() {
    zmq_pollitem_t item = {repSock_, 0, ZMQ_POLLIN, 0};

    while (running_.load()) {
        int rc = zmq_poll(&item, 1, 500);
        if (rc < 0) break;
        if (!(item.revents & ZMQ_POLLIN)) continue;

        zmq_msg_t msg;
        zmq_msg_init(&msg);
        if (zmq_msg_recv(&msg, repSock_, 0) < 0) {
            zmq_msg_close(&msg);
            continue;
        }
        std::string raw(static_cast<const char*>(zmq_msg_data(&msg)), zmq_msg_size(&msg));
        zmq_msg_close(&msg);

        try {
            auto j = nlohmann::json::parse(raw);
            bool isAwait = j.value("_await", false);
            Message m = jsonToMessage(j);
            if (m.cmd.empty()) {
                nlohmann::json err;
                err["status"] = "error";
                err["msg"] = "Empty cmd";
                std::string resp = err.dump();
                zmq_send(repSock_, resp.data(), resp.size(), 0);
                continue;
            }

            if (isAwait) {
                m.setAwaitResponse(true);
                if (j.contains("_correlationId")) {
                    m.setCorrelationId(j["_correlationId"].get<uint64_t>());
                }
                Message result = MessageBus::instance().sendAndWait(m, 5000);
                nlohmann::json out = messageToJson(result, "response");
                out["_correlationId"] = result.getCorrelationId();
                std::string outStr = out.dump();
                zmq_send(repSock_, outStr.data(), outStr.size(), 0);
            } else {
                MessageBus::instance().send(m);
                nlohmann::json ok;
                ok["status"] = "ok";
                std::string resp = ok.dump();
                zmq_send(repSock_, resp.data(), resp.size(), 0);
            }
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["status"] = "error";
            err["msg"] = e.what();
            std::string resp = err.dump();
            zmq_send(repSock_, resp.data(), resp.size(), 0);
        }
    }
}

// ─── DEALER: receive async responses from peer ────────────────

void MSMGateway::runDEALER(int idx) {
    PeerRuntime* pr = nullptr;
    {
        std::lock_guard<std::mutex> lock(peerMtx_);
        if (idx < (int)peerRuntimes_.size())
            pr = &peerRuntimes_[idx];
    }
    if (!pr) return;

    zmq_pollitem_t item = {pr->dealerSock, 0, ZMQ_POLLIN, 0};

    while (running_.load()) {
        int rc = zmq_poll(&item, 1, 500);
        if (rc < 0) break;
        if (!(item.revents & ZMQ_POLLIN)) continue;

        zmq_msg_t delim, body;
        zmq_msg_init(&delim);
        zmq_msg_init(&body);
        if (zmq_msg_recv(&delim, pr->dealerSock, 0) < 0 ||
            zmq_msg_recv(&body, pr->dealerSock, 0) < 0) {
            zmq_msg_close(&delim);
            zmq_msg_close(&body);
            continue;
        }
        std::string raw(static_cast<const char*>(zmq_msg_data(&body)), zmq_msg_size(&body));
        zmq_msg_close(&delim);
        zmq_msg_close(&body);

        try {
            auto j = nlohmann::json::parse(raw);
            std::string type = j.value("type", "");

            if (type == "response") {
                uint64_t cid = j.value("_correlationId", 0ULL);
                Message resp;
                resp.cmd = j.value("cmd", "Response");
                if (j.contains("data") && j["data"].is_object()) {
                    for (auto& [k, v] : j["data"].items()) {
                        if (v.is_string())
                            resp.data.set(k, v.get<std::string>());
                        else
                            resp.data.set(k, v.dump());
                    }
                }
                resp.setCorrelationId(cid);
                MessageBus::instance().send(resp);
            }
        } catch (const std::exception& e) {
            std::cerr << "[MSMGateway] DEALER parse error: " << e.what() << "\n";
        }
    }
}

// ─── PUB: pop messages from queue and publish ─────────────────

void MSMGateway::runPUB() {
    while (running_.load()) {
        std::string payload;
        {
            std::lock_guard<std::mutex> lock(pubMtx_);
            if (!pubQueue_.empty()) {
                payload = pubQueue_.front();
                pubQueue_.pop();
            }
        }
        if (!payload.empty()) {
            zmq_send(pubSock_, payload.data(), payload.size(), 0);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// ─── SUB: receive broadcasts from remote PUB ──────────────────

void MSMGateway::runSUB(int idx) {
    if (idx >= (int)subRuntimes_.size()) return;
    void* sock = subRuntimes_[idx].subSock;
    if (!sock) return;

    zmq_pollitem_t item = {sock, 0, ZMQ_POLLIN, 0};

    while (running_.load()) {
        int rc = zmq_poll(&item, 1, 500);
        if (rc < 0) break;
        if (!(item.revents & ZMQ_POLLIN)) continue;

        zmq_msg_t msg;
        zmq_msg_init(&msg);
        if (zmq_msg_recv(&msg, sock, 0) < 0) {
            zmq_msg_close(&msg);
            continue;
        }
        std::string raw(static_cast<const char*>(zmq_msg_data(&msg)), zmq_msg_size(&msg));
        zmq_msg_close(&msg);

        try {
            auto j = nlohmann::json::parse(raw);
            Message m = jsonToMessage(j);
            if (m.cmd.empty()) continue;

            if (m.cmd == "BROADCAST_TEST") {
                std::cout << "[MSMGateway] SUB received: " << m.data.get("msg", "") << "\n";
            }

            auto& bindings = subConfigs_[idx].bindings;
            for (auto& b : bindings) {
                auto colon = b.remoteVar.find(':');
                if (colon == std::string::npos) continue;
                std::string rMod = b.remoteVar.substr(0, colon);
                std::string rVar = b.remoteVar.substr(colon + 1);
                std::string rModVal = m.data.get("module", "");
                std::string rVarVal = m.data.get("var", "");
                if (rModVal != rMod || rVarVal != rVar) continue;
                std::string val = m.data.get("value", "");
                if (val.empty()) continue;

                auto lc = b.localVar.find(':');
                if (lc == std::string::npos) continue;
                std::string lMod = b.localVar.substr(0, lc);
                std::string lVar = b.localVar.substr(lc + 1);
                Module* mod = MessageBus::instance().findModule(lMod);
                if (mod && mod->getVars()) {
                    mod->getVars()->set(lVar, val);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[MSMGateway] SUB parse error: " << e.what() << "\n";
        }
    }
}

// ─── Serialization ────────────────────────────────────────────

Message MSMGateway::jsonToMessage(const nlohmann::json& j) {
    Message m;
    m.cmd = j.value("cmd", "");
    if (m.cmd.empty()) return m;

    m.setSender(j.value("_sender", "Remote"));
    m.setTarget(j.value("_target", ""));
    m.setCorrelationId(j.value("_correlationId", 0ULL));
    m.setAwaitResponse(j.value("_await", false));
    m.setMode(j.value("_mode", "call"));

    if (j.contains("data") && j["data"].is_object()) {
        for (auto& [k, v] : j["data"].items()) {
            if (v.is_string()) m.data.set(k, v.get<std::string>());
            else m.data.set(k, v.dump());
        }
    }

    return m;
}

nlohmann::json MSMGateway::messageToJson(const Message& msg, const std::string& type) {
    nlohmann::json j;
    if (!type.empty()) j["type"] = type;
    j["cmd"] = msg.cmd;
    j["_sender"] = msg.getSender();
    j["_target"] = msg.getTarget();
    j["_correlationId"] = msg.getCorrelationId();
    j["_await"] = msg.isAwaitResponse();
    j["_mode"] = msg.getMode();

    j["data"] = nlohmann::json::object();
    for (auto& [k, v] : msg.data.data) {
        if (k != "_remote")
            j["data"][k] = v;
    }

    return j;
}

} // namespace msm
