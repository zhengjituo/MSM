#ifndef DAQ_CONFIG_COMPAT_H
#define DAQ_CONFIG_COMPAT_H

// Compatibility header: module_daq still references DaqConfig internally.
#include "config_impl.h"
using DaqConfig = ConfigImpl;

#endif
