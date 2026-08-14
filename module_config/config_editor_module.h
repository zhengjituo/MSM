#ifndef CONFIG_EDITOR_MODULE_H
#define CONFIG_EDITOR_MODULE_H

#include "Module.h"
#include "MessageBus.h"
#include <nlohmann/json.hpp>
#include <string>
#include <mutex>
#include <atomic>
#include <map>
#include <vector>
#include <functional>

namespace msm {

// ── ConfigEditorModule ─────────────────────────────────────────
// 独立配置编辑器模块（独立 MSM 状态机）。
// 职责：import json 配置文件 → 解析 → 编辑 → export json 配置文件。
// 与原 DAQ 程序无耦合；交集仅是配置文件读写 + 运行状态监视启动调用。
// 行为约定：
//   - 读取：ordered_json 解析，保留键序、未知字段、值类型
//   - 写入：原子写（tmp.<pid> + fsync + rename）；迁移前 .bak 备份
//   - 版本：ConfigVersion 字段（当前=1，旧文件无键 → 视为 v1），增量补默认
//   - 保存语义：channel / card / chassis 三种 scope；身份键不覆盖；
//     params 键必须属于已知可编辑键集合，否则拒绝
class ConfigEditorModule : public msm::Module {
public:
    explicit ConfigEditorModule();
    ~ConfigEditorModule() override;

    void start() override;
    void stop() override;
    void handleState(const msm::Message& msg) override;

    // ── 生命周期（import / export）──
    // import：读取文件并解析，执行版本迁移，更新内存状态
    bool importConfig(const std::string& path, std::string* err = nullptr);
    // export：将内存状态原子写回文件
    bool exportConfig(const std::string& path = "", std::string* err = nullptr);

    // ── 查询 ──
    std::string getConfigJson() const;   // 内存配置（ordered_json dump）
    std::string currentVersion() const;  // "v1" 等
    const std::string& loadedPath() const { return loadedPath_; }

    // ── 编辑（三种 scope + app）──
    // scope: "channel" | "card" | "chassis"
    // params: key->value 覆盖写入目标通道/卡/机箱所有通道
    // 返回: "ok" | "locked" | "error:..."（错误信息见 kError 前缀）
    std::string applyEdit(const std::string& scope, int cardIndex, int channel,
                          const std::map<std::string, std::string>& params);

    // app_config 子对象整体替换（合并到现有 app_config，含类型保护）
    std::string applyAppEdit(const std::string& appConfigJson);

    // app_config 合并到指定目标文件（保存到启动配置），返回 "ok"/"locked"/error
    // target 文件不存在或用默认内容时以 app_config 为主重建 {app_config:{...}}
    std::string applyAppEditToFile(const std::string& appConfigJson,
                                   const std::string& targetFile);

    // ── 只读编辑元数据（供前端下拉）──
    // 已知可编辑通道参数键（对齐 Qt ChannelConfig + DownSample）
    static const std::vector<std::string>& editableChannelKeys();
    // 枚举键 → 合法取值（文件值）
    static const std::map<std::string, std::vector<std::string>>& enumOptions();
    // 卡片类型 → 中文标签
    static const std::map<std::string, std::string>& cardTypeLabels();

private:
    void registerVars();
    bool saveToFileAtomic(const std::string& path, const std::string& content);
    bool migrateIfNeeded();   // 版本迁移 + .bak 备份

    // 编辑定位
    nlohmann::ordered_json* findCard(int cardIndex);
    nlohmann::ordered_json& configJson();   // 顶层
    nlohmann::ordered_json& cardsJson();
    nlohmann::ordered_json& appConfigJson();

    // 合并 app_config（含类型保护）；失败返回 error 状态，成功返回空串
    std::string mergeAppConfig(nlohmann::ordered_json& app, nlohmann::ordered_json& ac);

    std::string loadedPath_;
    nlohmann::ordered_json cfg_;           // 内存中持有完整配置
    mutable std::mutex mtx_;
};

} // namespace msm

#endif
