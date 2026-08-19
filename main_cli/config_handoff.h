// config_handoff.h
#ifndef MSM_CONFIG_HANDOFF_H
#define MSM_CONFIG_HANDOFF_H
#include <string>

// 若 pending 存在则原子覆盖到 runtime；若 runtime 仍不存在则用 init 播种。
// 返回 0 成功，非 0 失败。
int ensureRuntimeConfig(const std::string& initPath,
                        const std::string& pendingPath,
                        const std::string& runtimePath);

#endif
