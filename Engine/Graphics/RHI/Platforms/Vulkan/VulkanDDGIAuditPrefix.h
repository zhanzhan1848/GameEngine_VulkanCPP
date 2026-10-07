#pragma once
#include <cstdlib>
#include <cstring>

namespace primal::graphics::rhi {
// Exact opt-in for saved native candidate evidence only. No rendering writes.
inline const char* ddgiRecordedComputeAuditPrefix() {
    const char* dump = std::getenv("DDGI_DUMP");
    const char* zero = std::getenv("DDGI_FIRST_ZERO_CROSS");
    const char* prefix = std::getenv("DDGI_FIRST_ZERO_CROSS_AUDIT_PREFIX");
    if (dump && std::strcmp(dump, "2") == 0 && zero &&
        std::strcmp(zero, "1") == 0 && prefix && *prefix) return prefix;
    return std::getenv("DDGI_ANALYTIC_FIRST_RHO_PREFIX");
}
}
