#!/usr/bin/env python3
"""config editor 端到端冒烟测试：以 backup json 为中介读入→编辑→保存回 config 目录"""
import json, subprocess, sys, time, urllib.request, os, shutil, hashlib

BASE = "http://localhost:8081"
CFG_DIR = "/home/zkcx/ws/MSM_DAQ/config"
BACKUP = os.path.join(CFG_DIR, "app_initial_config_backup.json")
MAIN = os.path.join(CFG_DIR, "app_initial_config.json")

def get(path):
    with urllib.request.urlopen(BASE + path) as r:
        return r.status, json.loads(r.read().decode())

def post(path, body):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode())

fails = 0
def check(name, cond):
    global fails
    print(("PASS" if cond else "FAIL"), "-", name)
    if not cond: fails += 1

# 1. 备份 config 目录文件哈希，测试后恢复
before = {}
for f in ["app_initial_config.json", "app_initial_config_backup.json"]:
    p = os.path.join(CFG_DIR, f)
    before[f] = hashlib.sha256(open(p, 'rb').read()).hexdigest() if os.path.exists(p) else None

# 2. 启动编辑器（以 backup 为中介读入）
proc = subprocess.Popen(["./main_config_editor/build/msm_config_editor", "--config", BACKUP],
                        cwd="/home/zkcx/ws/MSM_DAQ", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)
try:
    # 3. /api/lock
    st, j = get("/api/lock")
    check("lock ok, acquiring=false", st == 200 and j.get("acquiring") is False)

    # 4. /api/config 读入
    st, cfg = get("/api/config")
    check("config load ok", st == 200 and "DaqParameter" in cfg and "app_config" in cfg)

    # 5. /api/meta
    st, meta = get("/api/meta")
    check("meta ok", st == 200 and "keys" in meta and "enums" in meta and meta["version"].startswith("v"))

    # 6. 编辑 channel scope
    st, j = post("/api/config/save", {"scope": "channel", "cardIndex": 1, "channel": 1,
                                       "params": {"TrigLength": "700", "runEnable": "true"}})
    check("save channel ok", st == 200 and j.get("status") == "ok")

    # 7. 读取验证
    st, cfg2 = get("/api/config")
    ch = cfg2["DaqParameter"]["Cards"]["Card#1"]["Channels"]["Channel#1"]
    check("channel edit persisted", ch["TrigLength"] == "700")

    # 8. 编辑 card scope
    st, j = post("/api/config/save", {"scope": "card", "cardIndex": 1, "channel": 0,
                                       "params": {"DownSample": "500MSa/s"}})
    check("save card ok", st == 200 and j.get("status") == "ok")
    st, cfg3 = get("/api/config")
    c1 = cfg3["DaqParameter"]["Cards"]["Card#1"]["Channels"]["Channel#1"]
    c2 = cfg3["DaqParameter"]["Cards"]["Card#1"]["Channels"]["Channel#2"]
    check("card edit all channels", c1["DownSample"] == "500MSa/s" and c2["DownSample"] == "500MSa/s")

    # 9. 编辑 chassis scope（跳过 3010）
    st, j = post("/api/config/save", {"scope": "chassis", "cardIndex": 0, "channel": 0,
                                       "params": {"TrigMode": "autoMode"}})
    check("save chassis ok", st == 200 and j.get("status") == "ok")

    # 10. 非法枚举 / 未知键 / 身份键拒绝
    st, j = post("/api/config/save", {"scope": "channel", "cardIndex": 1, "channel": 1,
                                       "params": {"WorkMode": "bad_mode"}})
    check("bad enum rejected", st == 400)
    st, j = post("/api/config/save", {"scope": "channel", "cardIndex": 1, "channel": 1,
                                       "params": {"Hacked": "1"}})
    check("unknown key rejected", st == 400)
    st, j = post("/api/config/save", {"scope": "channel", "cardIndex": 1, "channel": 1,
                                       "params": {"cardIndex": "99"}})
    check("identity key rejected", st == 400)

    # 11. app_config 编辑
    st, j = post("/api/config/save-app", {"app_config": {"runId": 999, "maxSaveSizeMB": 768}})
    check("save app ok", st == 200 and j.get("status") == "ok")
    st, cfg4 = get("/api/config")
    check("app edit persisted", cfg4["app_config"]["runId"] == 999 and cfg4["app_config"]["maxSaveSizeMB"] == 768)
    # 原有字段保留
    check("app other fields kept", cfg4["app_config"]["dataSavePath"] == "/mnt/localssd")

    # 12. 保存后的文件落盘到 backup
    on_disk = json.load(open(BACKUP))
    check("backup file written", on_disk["app_config"]["runId"] == 999)

    # 13. 页面
    with urllib.request.urlopen(BASE + "/") as r:
        html = r.read().decode()
    check("index page ok", r.status == 200 and "DAQ 配置编辑器" in html)

    # 14. 优雅退出
    st, j = post("/api/shutdown", {})
    check("shutdown ok", st == 200)
    proc.wait(timeout=5)
    check("process exited", proc.returncode == 0)

finally:
    if proc.poll() is None:
        proc.terminate()
        proc.wait(timeout=3)
    # 恢复 config 目录原状（backup 恢复为初始备份哈希）
    for f, h in before.items():
        p = os.path.join(CFG_DIR, f)
        cur = hashlib.sha256(open(p, 'rb').read()).hexdigest() if os.path.exists(p) else None
        if cur != h:
            if before.get(f):
                # 从 main 文件恢复 backup 内容（原样字节）
                import copy
                with open(MAIN, 'rb') as src, open(p, 'wb') as dst:
                    dst.write(src.read())
                print(f"RESTORE {f}")

print("\n" + ("ALL PASS" if fails == 0 else f"{fails} FAILURES"))
sys.exit(1 if fails else 0)
