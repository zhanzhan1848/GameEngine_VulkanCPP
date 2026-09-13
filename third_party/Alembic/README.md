# Alembic + Imath（vendored，PFX 离线转换器专用）

- 版本：Alembic 1.8.7 + Imath 3.1.11（dylib，macOS arm64，Release PIC）
- 重建配方：
    cmake -S Imath -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
          -DCMAKE_INSTALL_PREFIX=<install>
    cmake --build build -j8 && cmake --install build
    cmake -S Alembic -B build -DUSE_HDF5=OFF -DImath_DIR=<install>/lib/cmake/Imath \
          -DCMAKE_INSTALL_PREFIX=<install>
    cmake --build build -j8 && cmake --install build
- 消费方：ContentTools 的 pfx_converter（离线）；libEngine 不链接本目录。
- LICENSE：Alembic MPL-2.0、Imath BSD-3-Clause（见仓库根 LICENSE-THIRD-PARTY.md）。
