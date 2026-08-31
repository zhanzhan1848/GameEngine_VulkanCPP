#include "EngineDLL/Common.h"
#include "CommonHeaders.h"
#include "EngineDLL/AssetAPI.h"

#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: TestCookedMeshImport <mesh-file>\n";
        return 2;
    }
    if (!InitializeAssetSystem()) return 3;

    const u64 request = ImportMeshAsync(argv[1]);
    if (request == 0)
    {
        ShutdownAssetSystem();
        return 4;
    }

    u64 mesh_id = 0;
    u32 status = 0;
    for (int attempt = 0; attempt < 500 && status == 0; ++attempt)
    {
        TickMainThreadJobs();
        TryGetMeshImportResult(request, &mesh_id, &status);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "request=" << request << " status=" << status
              << " mesh_id=" << mesh_id << '\n';
    ShutdownAssetSystem();
	// Zero is the first valid native content id; invalid_id is UINT32_MAX.
	return status == 1 && mesh_id != static_cast<u64>(u32_invalid_id) ? 0 : 5;
}
