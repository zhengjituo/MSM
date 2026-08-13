#!/bin/bash
# 启动独立 config 编辑器（以 backup json 为中介读入）
# 用法: ./tools/run_config_editor.sh [配置文件路径]
# 默认: config/app_initial_config_backup.json
set -e
cd "$(dirname "$0")/.."

CONFIG="${1:-config/app_initial_config_backup.json}"
echo "[run_config_editor] loading: $CONFIG"
./main_config_editor/build/msm_config_editor --config "$CONFIG"
