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
