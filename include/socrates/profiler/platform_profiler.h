#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "socrates/types.h"

namespace socrates::profiler::platform {

std::string cpu_model();
std::uint32_t cpu_count();
std::uint64_t total_ram();
std::uint64_t available_ram();
std::vector<std::string> accelerators();
std::string os_name();

BackendCapability make_platform_primary_backend();

}  // namespace socrates::profiler::platform
