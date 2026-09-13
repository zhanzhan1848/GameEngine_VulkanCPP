// CacheConverters.cpp — 见 CacheConverters.h。

#include "CacheConverters.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::convert {

bool vdb_converter_available() { return false; }
bool alembic_converter_available() { return false; }

bool vdb_to_pgrid(const char* vdb_path, const char* grid_name, const char* out_pgrid, std::string& err) {
    (void)vdb_path;
    (void)grid_name;
    (void)out_pgrid;
    err = "vdb_converter not built: requires OpenVDB (read-only subset) in ContentTools; "
          "format contract: Docs/2026-09-12-particle-framework-architecture.md §5";
    return false;
}

bool alembic_to_pcache(const char* abc_path, const char* object_path, f32 fps, const char* out_pcache,
                       std::string& err) {
    (void)abc_path;
    (void)object_path;
    (void)fps;
    (void)out_pcache;
    err = "alembic_converter not built: requires Alembic (Ogawa read) in ContentTools; "
          "format contract: Docs/2026-09-12-particle-framework-architecture.md §5";
    return false;
}

} // namespace primal::pfx::convert

#endif // !DISABLE_PARTICLE_FX
