# Third-Party License Notice

The source code in this repository (excluding the `third_party/` directory)
is licensed under the **GNU General Public License v3 (GPL-3.0)** — see
[LICENSE](./LICENSE).

The `third_party/` directory contains third-party software that is **not**
covered by the project's GPL-3.0 license. Each subdirectory retains its
original license and copyright notices, as required by the respective
licenses. These components are distributed under their own terms.

| Component                         | License                  |
| --------------------------------- | ------------------------ |
| third_party/JoltPhysics           | MIT                      |
| third_party/PhysX                 | BSD-3-Clause             |
| third_party/VHACD                 | Apache-2.0               |
| third_party/WebGPU (Dawn)         | BSD-3-Clause             |
| third_party/astc-encoder          | Apache-2.0               |
| third_party/eigen                 | MPL-2.0                  |
| third_party/libigl                | MPL-2.0                  |
| third_party/lua                   | MIT                      |
| third_party/meshoptimizer         | MIT                      |
| third_party/moodycamel-ConcurrentQueue | Boost Software License v1.0 |
| third_party/pmp-library           | MIT                      |
| third_party/stb                   | MIT / Public Domain      |
| third_party/xatlas                | MIT                      |

This table is provided for convenience. If a `third_party/` subdirectory
contains its own `LICENSE`, `COPYING`, or `LICENSE.txt` file, that file is
authoritative and takes precedence over this notice.

The bundled platform headers under `OSAPI/MAC/Metal` and `OSAPI/MAC/MetalFX`
are Apple SDK headers distributed under Apple's own terms
(see `OSAPI/MAC/LICENSE-metalcpp.txt`).

## Alembic 1.8.7（third_party/Alembic/Mac）
- 许可：MPL-2.0（含 Ogawa 子集）；来源：github.com/Alembic/Alembic v1.8.7
- 用途：PFX 离线点缓存转换器（pfx_converter，Alembic → .pcache）
- 部署：dylib 随 ContentTools 测试/工具目录部署（沿 FBX_SDK 先例）；运行时（libEngine）不链接

## Imath 3.1.11（third_party/Alembic/Mac/include/Imath + lib）
- 许可：BSD-3-Clause；来源：github.com/AcademySoftwareFoundation/Imath v3.1.11
- 用途：Alembic 依赖（V3f/half 类型）

## TBB 2023.1.0（brew，OpenVDB 构建依赖）
- 许可：Apache-2.0；用途：vdb_converter 构建依赖（未接入运行时）

## OpenVDB 11.0.0（third_party/OpenVDB/Mac）
- 许可：MPL-2.0；来源：github.com/AcademySoftwareFoundation/openvdb v11.0.0
- 用途：vdb_converter（离线 .vdb → .pgrid）；构建依赖 oneTBB 2021.13.0（third_party/TBB/Mac）
- 部署：静态链接进 pfx_converter 离线工具；运行时（libEngine）不链接

## oneTBB 2021.13.0（third_party/TBB/Mac）
- 许可：Apache-2.0；来源：github.com/oneapi-src/oneTBB v2021.13.0
- 用途：OpenVDB 构建依赖（静态链接进 pfx_converter）
