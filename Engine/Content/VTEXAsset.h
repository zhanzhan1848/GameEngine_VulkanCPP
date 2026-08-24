/**
 * @file VTEXAsset.h
 * @brief VT M1 — VTEX 容器运行时读取器(Engine 侧)
 * @details 只做结构校验与索引访问,不解码(M2 的 StreamingManager 负责
 *          zlib 解压与 GPU 上传)。容器结构单一事实源在
 *          ContentTools/common/VTBaker.h,本文件直接 include,两侧永不漂移。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-08-21
 */

#pragma once

#include "../../ContentTools/common/VTBaker.h"

#include <cstdint>

namespace primal::content {

class vtex_asset {
public:
    /// 校验并绑定容器数据(不拷贝;调用方保证生命周期)
    bool initialize(const uint8_t* data, uint64_t size) {
        _data = nullptr;
        if (!data || size < sizeof(vt::vtex_header)) return false;
        const vt::vtex_header* h = reinterpret_cast<const vt::vtex_header*>(data);
        if (std::memcmp(h->magic, "VTEX", 4) != 0) return false;
        if (h->version != vt::kVTEXVersion) return false;
        if (h->mip_count == 0 || h->page_count == 0) return false;
        const uint64_t index_end = uint64_t(h->index_offset) +
            uint64_t(h->page_count) * sizeof(vt::vtex_page_index);
        if (index_end > size) return false;
        if (h->bcn_size > 0 &&
            uint64_t(h->bcn_offset) + h->bcn_size > size) return false;
        if (h->astc_size > 0 &&
            uint64_t(h->astc_offset) + h->astc_size > size) return false;
        // M5:空数据段合法(运行时 VT — 页由 producer 生产,不走 tile_data)。
        // 判定改为:索引区存在即可。
        _data = data;
        _header = h;
        return true;
    }

    const vt::vtex_header& header() const { return *_header; }
    uint32_t page_count() const { return _header->page_count; }

    const vt::vtex_page_index* page(uint32_t i) const {
        if (i >= _header->page_count) return nullptr;
        const uint8_t* p = _data + _header->index_offset +
                           uint64_t(i) * sizeof(vt::vtex_page_index);
        return reinterpret_cast<const vt::vtex_page_index*>(p);
    }

    /// 多层容器:层计数(reserved[0];单层=1)
    uint32_t layer_count() const {
        return _header ? std::max(1u, _header->reserved[0]) : 1u;
    }
    /// 多层容器:层 l 的页索引(索引区 [l*page_count, (l+1)*page_count))
    const vt::vtex_page_index* page_layer(uint32_t i, uint32_t layer) const {
        if (!_header || layer >= layer_count() || i >= _header->page_count) return nullptr;
        const uint8_t* p = _data + _header->index_offset +
            (uint64_t(layer) * _header->page_count + i) * sizeof(vt::vtex_page_index);
        return reinterpret_cast<const vt::vtex_page_index*>(p);
    }

    /// 指定页的段数据指针与字节数(astc=false → BCn 段)
    const uint8_t* tile_data(const vt::vtex_page_index& p, bool astc,
                             uint64_t& out_size) const {
        const uint32_t off = astc ? p.astc_offset : p.bcn_offset;
        const uint32_t sz = astc ? p.astc_size : p.bcn_size;
        const uint32_t secOff = astc ? _header->astc_offset : _header->bcn_offset;
        const uint32_t secSize = astc ? _header->astc_size : _header->bcn_size;
        if (sz == 0) { out_size = 0; return nullptr; }
        if (uint64_t(off) + sz > secSize) { out_size = 0; return nullptr; }
        out_size = sz;
        return _data + secOff + off;
    }

    bool uses_zlib() const { return (_header->flags & vt::kVTEXFlagZlibTiles) != 0; }

private:
    const uint8_t* _data = nullptr;
    const vt::vtex_header* _header = nullptr;
};

} // namespace primal::content
