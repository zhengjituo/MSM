#!/bin/bash
# Start MSM DAQ with 3010 trigger subprocess management
# Usage: sudo ./run_msm_daq.sh
#
# Architecture:
#   msm_daq_display (main process)
#     ├── initializes 1073 cards, starts channels
#     ├── spawns trigger3010 as CHILD subprocess (separate process for 3010 card)
#     │     ├── CxDeviceSdkInit() → init 3010 → write reg 0x1C020=1
#     │     └── stays alive holding trigger open
#     ├── reads frames from 1073 → ring buffers
#     │     └── ModuleProcessFull (peek) + ModuleStorage (consume)
#     └── on stop: kill trigger3010 first, then cleanup 1073

DAQ_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${DAQ_DIR}/build"

if [ ! -f "${BUILD_DIR}/trigger3010" ]; then
    cp "${DAQ_DIR}/trigger3010" "${BUILD_DIR}/"
fi
if [ ! -d "${BUILD_DIR}/config" ]; then
    cp -r "${DAQ_DIR}/config" "${BUILD_DIR}/"
fi

export LD_LIBRARY_PATH="${DAQ_DIR}/sdk_include:${LD_LIBRARY_PATH}"
exec "${BUILD_DIR}/msm_daq_display"
