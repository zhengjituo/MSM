#include "ZMQGateway.h"
#include "NIStringParser.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <set>

namespace msm {

ZMQGateway::ZMQGateway(const std::string& endpoint, Mode mode)
    : Module("ZMQGateway"), endpoint_(endpoint), mode_(mode) {}

ZMQGateway::~ZMQGateway()
{
    stop();
}

void ZMQGateway::setPeerModules(const std::vector<std::string>& modules)
{
    std::lock_guard<std::mutex> lock(mtx_);
    peerModules_ = modules;
}

// ─── Lifecycle ──────────────────────────────────────────────────

void ZMQGateway::start()
{
    if (running_.load()) return;

    ctx_ = zmq_ctx_new();
    if (!ctx_) {
        std::cerr << "[ZMQGateway] Failed to create context\n";
        return;
    }

    if (mode_ == Server) {
        sock_ = zmq_socket(ctx_, ZMQ_ROUTER);
        if (!sock_) {
            std::cerr << "[ZMQGateway] Failed to create ROUTER socket\n";
            zmq_ctx_destroy(ctx_); ctx_ = nullptr;
            return;
        }
        int linger = 0;
        zmq_setsockopt(sock_, ZMQ_LINGER, &linger, sizeof(linger));
        if (zmq_bind(sock_, endpoint_.c_str()) != 0) {
            std::cerr << "[ZMQGateway] Failed to bind " << endpoint_ << "\n";
            zmq_close(sock_); sock_ = nullptr;
            zmq_ctx_destroy(ctx_); ctx_ = nullptr;
            return;
        }
        running_ = true;
        thread_ = std::thread(&ZMQGateway::runServer, this);
        std::cout << "[ZMQGateway] Server listening on " << endpoint_ << "\n";
    } else {
        sock_ = zmq_socket(ctx_, ZMQ_DEALER);
        if (!sock_) {
            std::cerr << "[ZMQGateway] Failed to create DEALER socket\n";
            zmq_ctx_destroy(ctx_); ctx_ = nullptr;
            return;
        }
        int linger = 0;
        zmq_setsockopt(sock_, ZMQ_LINGER, &linger, sizeof(linger));
        int hwm = 1000;
        zmq_setsockopt(sock_, ZMQ_SNDHWM, &hwm, sizeof(hwm));
        zmq_setsockopt(sock_, ZMQ_RCVHWM, &hwm, sizeof(hwm));
        if (zmq_connect(sock_, endpoint_.c_str()) != 0) {
            std::cerr << "[ZMQGateway] Failed to connect " << endpoint_ << "\n";
            zmq_close(sock_); sock_ = nullptr;
            zmq_ctx_destroy(ctx_); ctx_ = nullptr;
            return;
        }
        running_ = true;
        thread_ = std::thread(&ZMQGateway::runClient, this);
        std::cout << "[ZMQGateway] Client connecting to " << endpoint_ << "\n";
        // Give the connection a moment, then discover
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        sendDiscovery();
    }

    Module::start();
}

void ZMQGateway::stop()
{
    if (!running_.exchange(false)) return;

    if (sock_) {
        zmq_close(sock_);
        sock_ = nullptr;
    }
    if (ctx_) {
        zmq_ctx_destroy(ctx_);
        ctx_ = nullptr;
    }
    if (thread_.joinable()) thread_.join();

    std::cout << "[ZMQGateway] Stopped\n";
    Module::stop();
}

void ZMQGateway::handleState(const Message& msg)
{
    // Messages addressed to the Gateway itself are forwarded to remote
    std::string target = msg.getTarget();

    // If target starts with "Remote:", forward it
    if (target.compare(0, 7, "Remote:") == 0) {
        Message fwd = msg;
        fwd.setTarget(target.substr(7));  // strip "Remote:" prefix
        fwd.setSender(name_ + ":" + msg.getSender());
        forwardToRemote(fwd);
        return;
    }
}

ModuleVars* ZMQGateway::getVars() { return &vars_; }

// ─── Server Mode ────────────────────────────────────────────────

void ZMQGateway::runServer()
{
    zmq_pollitem_t item = {sock_, 0, ZMQ_POLLIN, 0};

    while (running_.load()) {
        int rc = zmq_poll(&item, 1, 500);
        if (rc < 0) break;
        if (!(item.revents & ZMQ_POLLIN)) continue;

        // Receive multipart: [identity, "", JSON-body]
        zmq_msg_t identity, delimiter, body;
        zmq_msg_init(&identity);
        zmq_msg_init(&delimiter);
        zmq_msg_init(&body);

        if (zmq_msg_recv(&identity, sock_, 0) < 0 ||
            zmq_msg_recv(&delimiter, sock_, 0) < 0 ||
            zmq_msg_recv(&body, sock_, 0) < 0) {
            zmq_msg_close(&identity);
            zmq_msg_close(&delimiter);
            zmq_msg_close(&body);
            continue;
        }

        std::string id(static_cast<const char*>(zmq_msg_data(&identity)), zmq_msg_size(&identity));
        std::string payload(static_cast<const char*>(zmq_msg_data(&body)), zmq_msg_size(&body));

        zmq_msg_close(&identity);
        zmq_msg_close(&delimiter);
        zmq_msg_close(&body);

        try {
            auto j = nlohmann::json::parse(payload);
            processRemoteMessage(id, j);
        } catch (const std::exception& e) {
            std::cerr << "[ZMQGateway] JSON parse error: " << e.what() << "\n";
            nlohmann::json err;
            err["type"] = "error";
            err["error"] = "Parse failed";
            std::string resp = err.dump();
            zmq_send(sock_, id.data(), id.size(), ZMQ_SNDMORE);
            zmq_send(sock_, "", 0, ZMQ_SNDMORE);
            zmq_send(sock_, resp.data(), resp.size(), 0);
        }
    }
}

// ─── Client Mode ────────────────────────────────────────────────

void ZMQGateway::runClient()
{
    zmq_pollitem_t item = {sock_, 0, ZMQ_POLLIN, 0};

    while (running_.load()) {
        int rc = zmq_poll(&item, 1, 500);
        if (rc < 0) break;
        if (!(item.revents & ZMQ_POLLIN)) continue;

        // Receive multipart: ["", JSON-body] (DEALER adds empty identity)
        zmq_msg_t delimiter, body;
        zmq_msg_init(&delimiter);
        zmq_msg_init(&body);

        if (zmq_msg_recv(&delimiter, sock_, 0) < 0 ||
            zmq_msg_recv(&body, sock_, 0) < 0) {
            zmq_msg_close(&delimiter);
            zmq_msg_close(&body);
            continue;
        }

        std::string payload(static_cast<const char*>(zmq_msg_data(&body)), zmq_msg_size(&body));
        zmq_msg_close(&delimiter);
        zmq_msg_close(&body);

        try {
            auto j = nlohmann::json::parse(payload);
            processRemoteMessage("", j);
        } catch (const std::exception& e) {
            std::cerr << "[ZMQGateway] JSON parse error: " << e.what() << "\n";
        }
    }
}

// ─── Message Processing ─────────────────────────────────────────

void ZMQGateway::processRemoteMessage(const std::string& identity, const nlohmann::json& j)
{
    std::string type = j.value("type", "");

    if (type == "discovery") {
        // Reply with local module list
        nlohmann::json resp;
        resp["type"] = "discovery";
        auto& bus = MessageBus::instance();
        resp["modules"] = nlohmann::json::array();
        // We don't have a public listModules(), so build from peerModules_
        {
            std::lock_guard<std::mutex> lock(mtx_);
            for (auto& m : peerModules_) {
                resp["modules"].push_back(m);
            }
        }
        std::string out = resp.dump();

        if (mode_ == Server) {
            zmq_send(sock_, identity.data(), identity.size(), ZMQ_SNDMORE);
            zmq_send(sock_, "", 0, ZMQ_SNDMORE);
        }
        zmq_send(sock_, out.data(), out.size(), 0);
        return;
    }

    if (type == "call" || type == "cast") {
        // Reconstruct a Message and inject into local MessageBus
        Message msg;
        msg.cmd = j.value("cmd", "");
        if (j.contains("data") && j["data"].is_object()) {
            for (auto& [k, v] : j["data"].items()) {
                if (v.is_string())
                    msg.data.set(k, v.get<std::string>());
                else
                    msg.data.set(k, v.dump());
            }
        }
        msg.setTarget(j.value("_target", ""));
        msg.setSender(j.value("_sender", "Remote"));
        msg.setMode(type);

        if (type == "call") {
            uint64_t cid = j.value("_correlationId", 0ULL);
            msg.setCorrelationId(cid);
            msg.setAwaitResponse(true);

            // Forward to local bus and wait for response
            Message resp = MessageBus::instance().sendAndWait(msg, 5000);

            nlohmann::json out;
            out["type"] = "response";
            out["_correlationId"] = cid;
            out["cmd"] = resp.cmd;
            out["data"] = nlohmann::json::object();
            for (auto& [k, v] : resp.data.data) {
                out["data"][k] = v;
            }
            std::string outStr = out.dump();

            if (mode_ == Server) {
                zmq_send(sock_, identity.data(), identity.size(), ZMQ_SNDMORE);
                zmq_send(sock_, "", 0, ZMQ_SNDMORE);
            }
            zmq_send(sock_, outStr.data(), outStr.size(), 0);
        } else {
            // Async cast — fire and forget
            MessageBus::instance().send(msg);
        }
    }
}

// ─── Forwarding ─────────────────────────────────────────────────

void ZMQGateway::forwardToRemote(const Message& msg)
{
    if (!running_.load() || !sock_) return;

    nlohmann::json j;
    j["type"] = "call";
    j["cmd"] = msg.cmd;
    j["_target"] = msg.getTarget();
    j["_sender"] = msg.getSender();
    j["_correlationId"] = msg.getCorrelationId();
    j["data"] = nlohmann::json::object();
    for (auto& [k, v] : msg.data.data) {
        j["data"][k] = v;
    }

    std::string payload = j.dump();

    std::lock_guard<std::mutex> lock(mtx_);
    if (mode_ == Server) {
        // Server: we need a specific identity — use first connected client
        // For simplicity, send to "default" identity or broadcast
        std::cerr << "[ZMQGateway] Server mode cannot initiate forward calls\n";
        return;
    }

    // Client: send via DEALER
    zmq_send(sock_, "", 0, ZMQ_SNDMORE);
    zmq_send(sock_, payload.data(), payload.size(), 0);

    if (msg.isAwaitResponse()) {
        // Wait for response — poll with timeout
        zmq_pollitem_t item = {sock_, 0, ZMQ_POLLIN, 0};
        int rc = zmq_poll(&item, 1, 5000);
        if (rc > 0 && (item.revents & ZMQ_POLLIN)) {
            zmq_msg_t delim, body;
            zmq_msg_init(&delim);
            zmq_msg_init(&body);
            zmq_msg_recv(&delim, sock_, 0);
            zmq_msg_recv(&body, sock_, 0);
            std::string resp(static_cast<const char*>(zmq_msg_data(&body)), zmq_msg_size(&body));
            zmq_msg_close(&delim);
            zmq_msg_close(&body);

            // The response is processed by the caller — store in a pending response map
            // For now, just log it
            std::cout << "[ZMQGateway] Forward response: " << resp << "\n";
        }
    }
}

void ZMQGateway::sendDiscovery()
{
    if (!sock_) return;

    nlohmann::json j;
    j["type"] = "discovery";
    std::string payload = j.dump();

    zmq_send(sock_, "", 0, ZMQ_SNDMORE);
    zmq_send(sock_, payload.data(), payload.size(), 0);

    // Wait for response
    zmq_pollitem_t item = {sock_, 0, ZMQ_POLLIN, 0};
    int rc = zmq_poll(&item, 1, 3000);
    if (rc > 0 && (item.revents & ZMQ_POLLIN)) {
        zmq_msg_t delim, body;
        zmq_msg_init(&delim);
        zmq_msg_init(&body);
        zmq_msg_recv(&delim, sock_, 0);
        zmq_msg_recv(&body, sock_, 0);
        std::string resp(static_cast<const char*>(zmq_msg_data(&body)), zmq_msg_size(&body));
        zmq_msg_close(&delim);
        zmq_msg_close(&body);

        try {
            auto jr = nlohmann::json::parse(resp);
            if (jr.value("type", "") == "discovery" && jr.contains("modules")) {
                std::lock_guard<std::mutex> lock(mtx_);
                peerModules_.clear();
                for (auto& m : jr["modules"]) {
                    peerModules_.push_back(m);
                    std::cout << "[ZMQGateway] Discovered peer module: "
                              << m.get<std::string>() << "\n";
                }
            }
        } catch (...) {}
    }
}

} // namespace msm
