# Cross-Process RPC Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use inline execution. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two processes communicating via ZMQ, each hosting one compute module, asking the other to calculate.

**Architecture:** MSMGateway with REP (入口) + DEALER (定点发) + PUB (被订阅广播). Message `_remote` flag routes through MessageBus to gateway. Static peer config.

**Tech Stack:** ZMQ (REP/DEALER/PUB), MSM (MessageBus/Module), nlohmann/json

---

### Task 1: Message — add `_remote` field

**Files:**
- Modify: `module_core/include/Message.h`
- Modify: `module_core/src/Message.cpp`

- [ ] **Add `_remote` getter/setter declarations to Message.h**

After `getMode()` / `setMode()`:

```cpp
std::string getRemote() const;
void setRemote(const std::string& peer);
bool isRemote() const;
```

- [ ] **Add `_remote` implementation to Message.cpp**

```cpp
std::string Message::getRemote() const {
    return data.get("_remote");
}
void Message::setRemote(const std::string& peer) {
    data.set("_remote", peer);
}
bool Message::isRemote() const {
    return !getRemote().empty();
}
```

- [ ] **Commit**

```bash
git add module_core/include/Message.h module_core/src/Message.cpp
git commit -m "feat: add _remote field to Message"
```

### Task 2: MessageBus — route `_remote` messages to MSMGateway

**Files:**
- Modify: `module_core/src/MessageBus.cpp`

- [ ] **In `send()`, check for `_remote` and forward to MSMGateway**

Add after `msg.isResponse()` check and before `findModule`:

```cpp
if (msg.isRemote()) {
    Module* gw = findModule("MSMGateway");
    if (gw) {
        gw->post(msg);
    }
    return;
}
```

- [ ] **In `sendAndWait()`, add same check at top**

```cpp
if (msg.isRemote()) {
    Module* gw = findModule("MSMGateway");
    if (gw) {
        gw->post(msg);
        return gw->post(msg);  // post forwards to gateway queue
```

Wait — `sendAndWait` needs to block. `gw->post(msg)` just queues. This won't work for sync. Let me think...

The issue: `sendAndWait` expects `notifyResponse` to resolve the promise. If we just queue to gateway, the response will eventually come back through DEALER thread. That's fine — `notifyResponse` on the same MessageBus resolves the promise.

So `sendAndWait` for remote:
1. Creates promise, stores in pendingResponses_
2. Sends to gateway via `send` (which posts to gateway queue)
3. Gateway processes, sends via DEALER
4. DEALER thread receives response
5. Calls `notifyResponse` → resolves promise
6. `wait_for` returns

This works! So the remote routing in `sendAndWait` is:

```cpp
if (msg.isRemote()) {
    // Set up correlationId and promise normally
    if (msg.getCorrelationId() == 0) {
        msg.setCorrelationId(Message::generateCorrelationId());
    }
    msg.setAwaitResponse(true);
    std::promise<Message> promise;
    std::future<Message> future = promise.get_future();
    uint64_t cid = msg.getCorrelationId();
    {
        std::lock_guard<std::mutex> lock(pendingMtx_);
        pendingResponses_[cid] = std::move(promise);
    }
    // Route through send() which forwards to gateway
    send(msg);
    // Wait for response
    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready) {
        return future.get();
    }
    Message t;
    t.cmd = "Timeout";
    return t;
}
```

Actually, this is essentially the same as the existing `sendAndWait` code. The only difference is that `send()` now routes to gateway instead of finding the module locally. So we just need to modify `send()` for `_remote`, and `sendAndWait` will work automatically!

- [ ] **Update `send()` to handle `_remote`**

Add right after `if (msg.isResponse()) { notifyResponse... }`:

```cpp
if (msg.isRemote()) {
    Module* gw = findModule("MSMGateway");
    if (gw) {
        gw->post(msg);
    }
    return;
}
```

- [ ] **Commit**

```bash
git add module_core/src/MessageBus.cpp
git commit -m "feat: route _remote messages to MSMGateway in MessageBus"
```

### Task 3: MSMGateway — rewrite with REP + DEALER + PUB

**Files:**
- Rewrite: `module_core/include/MSMGateway.h`
- Rewrite: `module_core/src/MSMGateway.cpp`

- [ ] **Rewrite MSMGateway.h**

```cpp
#ifndef MSM_MSGATEWAY_H
#define MSM_MSGATEWAY_H

#include "Module.h"
#include "MessageBus.h"
#include "Message.h"
#include <zmq.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <future>

namespace msm {

class MSMGateway : public Module {
public:
    struct PeerConfig {
        std::string name;
        std::string repEndpoint;   // DEALER connects here (peer's REP)
    };

    MSMGateway(int repPort, int pubPort);
    ~MSMGateway();

    void addPeer(const std::string& name, const std::string& repEndpoint);

    void start() override;
    void stop() override;
    void handleState(const Message& msg) override;
    ModuleVars* getVars() override;

private:
    void runREP();
    void runDEALER(int idx);
    void runPUB();

    Message jsonToMessage(const nlohmann::json& j);
    nlohmann::json messageToJson(const Message& msg, const std::string& type = "");

    int repPort_;
    int pubPort_;

    void* ctx_ = nullptr;
    void* repSock_ = nullptr;
    void* pubSock_ = nullptr;

    std::atomic<bool> running_{false};

    struct PeerRuntime {
        std::string name;
        void* dealerSock = nullptr;
        std::thread dealerThread;
    };

    std::vector<PeerConfig> peerConfigs_;
    std::vector<PeerRuntime> peerRuntimes_;
    std::mutex peerMtx_;

    std::thread repThread_;
    std::thread pubThread_;

    struct GatewayVars : public ModuleVars {
        std::map<std::string, std::string> list() override { return {}; }
        void* get(const std::string&) override { return nullptr; }
        bool set(const std::string&, const std::string&) override { return false; }
    } vars_;
};

} // namespace msm

#endif
```

- [ ] **Rewrite MSMGateway.cpp**

```cpp
#include "MSMGateway.h"
#include <iostream>
#include <cstring>
#include <chrono>

namespace msm {

MSMGateway::MSMGateway(int repPort, int pubPort)
    : Module("MSMGateway"), repPort_(repPort), pubPort_(pubPort) {}

MSMGateway::~MSMGateway() { stop(); }

void MSMGateway::addPeer(const std::string& name, const std::string& repEndpoint) {
    std::lock_guard<std::mutex> lock(peerMtx_);
    peerConfigs_.push_back({name, repEndpoint});
}

void MSMGateway::start() {
    if (running_.load()) return;

    ctx_ = zmq_ctx_new();
    if (!ctx_) { std::cerr << "[MSMGateway] Failed to create ZMQ context\n"; return; }

    // REP — 多路入口
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

    // PUB — 被订阅广播口
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

    // DEALER — 每个 peer 一个，定点发送
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
            int linger = 0;
            zmq_setsockopt(pr.dealerSock, ZMQ_LINGER, &linger, sizeof(linger));
            int hwm = 1000;
            zmq_setsockopt(pr.dealerSock, ZMQ_SNDHWM, &hwm, sizeof(hwm));
            if (zmq_connect(pr.dealerSock, cfg.repEndpoint.c_str()) != 0) {
                std::cerr << "[MSMGateway] Failed to connect DEALER to " << cfg.repEndpoint << "\n";
                zmq_close(pr.dealerSock);
                continue;
            }

            pr.dealerThread = std::thread(&MSMGateway::runDEALER, this, (int)peerRuntimes_.size());
            peerRuntimes_.push_back(std::move(pr));
            std::cout << "[MSMGateway] DEALER → " << cfg.name
                      << " (" << cfg.repEndpoint << ")\n";
        }
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
    if (repThread_.joinable()) repThread_.join();
    if (pubThread_.joinable()) pubThread_.join();

    if (ctx_) { zmq_ctx_destroy(ctx_); ctx_ = nullptr; }
    std::cout << "[MSMGateway] Stopped\n";
    Module::stop();
}

void MSMGateway::handleState(const Message& msg) {
    // 从 MessageBus 收到的远程消息，转发到对端
    std::string peer = msg.getRemote();
    if (peer.empty()) return;

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
}

ModuleVars* MSMGateway::getVars() { return &vars_; }

// ─── REP: 接收远程请求 ──────────────────────────────────────────

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
                    m.setCorrelationId(static_cast<uint64_t>(j["_correlationId"].get<uint64_t>()));
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

// ─── DEALER: 接收对端响应 ──────────────────────────────────────

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
                // 响应走 MessageBus 路由到 notifyResponse
                MessageBus::instance().send(resp);
            }
        } catch (const std::exception& e) {
            std::cerr << "[MSMGateway] DEALER parse error: " << e.what() << "\n";
        }
    }
}

// ─── PUB: 广播 ─────────────────────────────────────────────────

void MSMGateway::runPUB() {
    zmq_pollitem_t item = {pubSock_, 0, ZMQ_POLLIN, 0};
    (void)item;
    while (running_.load()) {
        // PUB is out-only; data comes from handleState or directly
        // Future: subscribe to MessageBus broadcasts and relay
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// ─── Serialization ─────────────────────────────────────────────

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
```

- [ ] **Commit**

```bash
git add module_core/include/MSMGateway.h module_core/src/MSMGateway.cpp
git commit -m "feat: rewrite MSMGateway with REP+DEALER+PUB"
```

### Task 4: AddModule — add callRemote + remoteResult

**Files:**
- Modify: `module_calculator/include/AddModule.h`

- [ ] **Add fields and handleState logic**

Add to struct:
```cpp
int callRemote = 0;
int remoteResult = 0;
```

Add to handleState `if (data_.callRemote)` block (after existing `action` logic):
```cpp
if (data_.callRemote) {
    data_.callRemote = 0;
    Message req("Calculate");
    req.setTarget("MultiplyModule");
    req.setSender("AddModule");
    req.data.set("a", data_.a);
    req.data.set("b", data_.b);
    req.setAwaitResponse(true);
    req.setRemote(gatewayPeer_);
    Message resp = MessageBus::instance().sendAndWait(req, 5000);
    if (resp.cmd == "Response") {
        data_.remoteResult = std::stoi(resp.data.get("result", "0"));
    }
    // Notify PVServer etc.
    for (auto& v : {"callRemote", "remoteResult"}) {
        Message u("PV_UPDATE");
        u.setTarget("PVServer");
        u.data.set("module", "AddModule");
        u.data.set("var", v);
        MessageBus::instance().send(u);
    }
}
```

Add to list()/getTypes():
```
{"callRemote", std::to_string(d->callRemote)},
{"remoteResult", std::to_string(d->remoteResult)}
```

Add to get():
```
if (name == "callRemote") return &d->callRemote;
if (name == "remoteResult") return &d->remoteResult;
```

Add to set():
```
if (name == "callRemote") { d->callRemote = v; return true; }
```

Wait — `gatewayPeer_` is not a module field. AddModule shouldn't know the peer name. Better approach: the main program configures the module.

Actually, simpler: use `_remote` in the Message and let MessageBus route it. AddModule doesn't need to know the peer name — it just sets `_remote` and the MessageBus/Gateway handle it.

But the module needs to know which peer to send to. Options:
1. Hardcode in the module (bad)
2. Pass as constructor arg or config
3. Use a convention — the main program sets it

For simplicity, I'll have the main program set a `peerName` field on the module after construction.

Actually, even simpler: the module doesn't specify `_remote` itself. Instead, the main program registers a _remote mapping for the module. But that complicates things.

Let me just add a simple config field: each module has a `remotePeer_` string that the main program sets.

Wait, I'm overcomplicating this. The module's callRemote handler:
1. Creates Message with target "MultiplyModule"
2. Sets `_remote` to the peer name
3. Calls `sendAndWait`

The module needs to know the peer name. Let me add a `setRemotePeer(const std::string&)` method.

Actually you know what, for this demo, just put it in the AddModule as a field. The constructor can take it as a parameter, or we can add a setter. Let me just have the main program call `module.setRemotePeer("PeerBeta")`.

Hmm but that means modifying the module interface for all modules. Let me think of a simpler way.

Simplest: the module's handleState checks `_remote` on the incoming message (the one that triggered callRemote), and reuses it. The external trigger (IPC PUT AddModule:callRemote 1) doesn't have `_remote` set.

OK, simplest approach: the main program sets a variable on the module directly. Let me just add a public member `std::string remotePeer` to AddModule. Main program sets it.

- [ ] **Update AddModule.h with all changes**

- [ ] **Commit**

```bash
git add module_calculator/include/AddModule.h
git commit -m "feat: add callRemote/remoteResult to AddModule"
```

### Task 5: MultiplyModule — add callRemote + remoteResult

**Files:**
- Modify: `module_calculator/include/MultiplyModule.h`

- [ ] **Same pattern as AddModule, but target is "AddModule" and reads "sum"**

```cpp
if (data_.callRemote) {
    data_.callRemote = 0;
    Message req("Calculate");
    req.setTarget("AddModule");
    req.setSender("MultiplyModule");
    req.data.set("a", data_.a);
    req.data.set("b", data_.b);
    req.setAwaitResponse(true);
    req.setRemote(remotePeer_);
    Message resp = MessageBus::instance().sendAndWait(req, 5000);
    if (resp.cmd == "Response") {
        data_.remoteResult = std::stoi(resp.data.get("sum", "0"));
    }
    // PV_UPDATE notifications
}
```

- [ ] **Commit**

```bash
git add module_calculator/include/MultiplyModule.h
git commit -m "feat: add callRemote/remoteResult to MultiplyModule"
```

### Task 6: New main programs — calc_alpha and calc_beta

**Files:**
- Create: `module_calculator/src/CalcAlpha.cpp`
- Create: `module_calculator/src/CalcBeta.cpp`
- Modify: `module_calculator/CMakeLists.txt`

- [ ] **Create CalcAlpha.cpp**

```cpp
#include "AddModule.h"
#include "MSMGateway.h"
#include "IPCServer.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

using namespace msm;
static std::atomic<bool> g_running{true};

int main(int argc, char* argv[]) {
    int repPort = 5555, pubPort = 5557;
    std::string peerName = "Beta";
    std::string peerEndpoint = "tcp://localhost:5556";

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--rep-port" && i+1 < argc) repPort = std::stoi(argv[++i]);
        else if (a == "--pub-port" && i+1 < argc) pubPort = std::stoi(argv[++i]);
        else if (a == "--peer" && i+1 < argc) {
            std::string val = argv[++i];
            auto comma = val.find(',');
            if (comma != std::string::npos) {
                peerName = val.substr(0, comma);
                peerEndpoint = val.substr(comma + 1);
            }
        }
    }

    AddModule addMod;
    addMod.remotePeer = peerName;

    MSMGateway gw(repPort, pubPort);
    gw.addPeer(peerName, peerEndpoint);

    MessageBus::instance().registerModule(&addMod);
    MessageBus::instance().registerModule(&gw);

    addMod.start();
    gw.start();

    std::cout << "[Alpha] AddModule + MSMGateway\n"
              << "  REP: " << repPort << ", PUB: " << pubPort << "\n"
              << "  Peer " << peerName << " → " << peerEndpoint << "\n";

    signal(SIGINT, [](int) { g_running = false; });
    signal(SIGTERM, [](int) { g_running = false; });

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    gw.stop();
    addMod.stop();
    MessageBus::instance().unregisterModule("AddModule");
    MessageBus::instance().unregisterModule("MSMGateway");
    return 0;
}
```

- [ ] **Create CalcBeta.cpp** (similar, with MultiplyModule)

```cpp
// Same pattern, uses MultiplyModule
// Default ports: repPort=5556, pubPort=5558
// Peer: Alpha,tcp://localhost:5555
```

- [ ] **Update CMakeLists.txt** to add both executables

```cmake
add_executable(calc_alpha src/CalcAlpha.cpp src/AddModule.cpp)
target_link_libraries(calc_alpha msm_core)

add_executable(calc_beta src/CalcBeta.cpp src/MultiplyModule.cpp)
target_link_libraries(calc_beta msm_core)
```

- [ ] **Commit**

```bash
git add module_calculator/src/CalcAlpha.cpp module_calculator/src/CalcBeta.cpp module_calculator/CMakeLists.txt
git commit -m "feat: add calc_alpha and calc_beta programs"
```

### Task 7: Build and test

- [ ] **Build**

```bash
cd /mnt/hgfs/share/opencode/MSM/module_calculator
cmake -B build
cmake --build build
```

- [ ] **Run cross-process test**

Terminal 1:
```bash
./build/calc_alpha --rep-port 5555 --pub-port 5557 \
    --peer Beta,tcp://localhost:5556
```

Terminal 2:
```bash
./build/calc_beta --rep-port 5556 --pub-port 5558 \
    --peer Alpha,tcp://localhost:5555
```

Terminal 3 (test via IPC/CLI):
```bash
# Set AddModule values on Alpha
echo "PUT AddModule:a 3" | socat - /tmp/msm.sock
echo "PUT AddModule:b 4" | socat - /tmp/msm.sock
echo "PUT AddModule:callRemote 1" | socat - /tmp/msm.sock
echo "GET AddModule:remoteResult" | socat - /tmp/msm.sock
# Expected: 12
```
