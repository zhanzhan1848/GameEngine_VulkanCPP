#pragma once

namespace primal::graphics
{
	struct platform_interface;

	namespace d3d12
	{
		// === Phase 1 Sub-step 1.2.6': 旧 platform_interface 填充器，Phase 2 删除 ===
		[[deprecated(
			"d3d12::get_platform_interface is deprecated. The RHI abstraction will host D3D12 "
			"via Engine/Graphics/RHI/Platforms/D3D12/ once implemented; until then this is dead "
			"code. See Phase 1 Sub-step 1.2.6'."
		)]]
		void get_platform_interface(platform_interface& pi);
	}
}