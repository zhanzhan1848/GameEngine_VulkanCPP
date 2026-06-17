// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once

namespace primal::graphics
{
struct platform_interface;

namespace vulkan
{
// === Phase 1 Sub-step 1.2.6': 旧 platform_interface 填充器，Phase 2 删除 ===
[[deprecated(
    "vulkan::get_platform_interface is deprecated. The RHI abstraction will host Vulkan "
    "via Engine/Graphics/RHI/Platforms/Vulkan/ once implemented; until then this is dead "
    "code. See Phase 1 Sub-step 1.2.6'."
)]]
void get_platform_interface(platform_interface& pi);
}
}