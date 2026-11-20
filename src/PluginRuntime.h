#pragma once

#include "PluginConfig.h"
#include <atomic>

namespace gakufsr {
extern GakuRuntimeService runtime;
extern GakuHookService hooks;
extern std::atomic<bool> enabled;
extern bool frameGeneration;
extern PluginConfig options;

void Log(const std::string& text, GakuLogLevel level=GAKU_LOG_INFO);
void Tick(void*,float);
void Shutdown(void*);
}
