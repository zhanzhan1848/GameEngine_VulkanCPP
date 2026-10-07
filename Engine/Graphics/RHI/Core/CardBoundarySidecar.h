#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

// Shared offline/runtime codec. Separate from SCS1: samples carry real positions
// and have no atlas coordinates, evaluated radiance, or visibility certificate.
namespace primal::graphics::rhi::card_boundary {
struct Sample {
    uint32_t triangle_id{0xffffffffu};
    float position[3]{}, normal[3]{};
    uint8_t albedo[4]{}, params[4]{};
};
static_assert(sizeof(Sample)==36, "SCB1 sample layout");
static_assert(offsetof(Sample,position)==4 && offsetof(Sample,normal)==16 &&
              offsetof(Sample,albedo)==28 && offsetof(Sample,params)==32, "SCB1 sample offsets");
struct Mesh {
    uint64_t geometry_digest{}, layout_digest{}, material_digest{};
    uint32_t triangle_count{};
    std::vector<Sample> samples;
};
struct Header {
    uint32_t magic{0x31424353u}, version{1}, header_bytes{40}, mesh_count{};
    uint32_t record_bytes{48}, sample_bytes{36}, flags{}, reserved{};
    uint64_t body_hash{}; // FNV-1a corruption detection, not authentication.
};
struct Record {
    uint64_t geometry_digest, layout_digest, material_digest;
    uint32_t triangle_count, sample_count;
    uint64_t samples_offset, samples_bytes;
};
static_assert(sizeof(Header)==40 && sizeof(Record)==48, "SCB1 directory layout");
static_assert(offsetof(Header,body_hash)==32 && offsetof(Record,triangle_count)==24 &&
              offsetof(Record,samples_offset)==32 && offsetof(Record,samples_bytes)==40, "SCB1 directory offsets");
inline constexpr uint64_t MaxFileBytes=64ull*1024*1024;
inline constexpr uint32_t MaxMeshes=65536, MaxSamplesPerMesh=1u<<20;
inline uint64_t Hash(const void* bytes, size_t count) {
    uint64_t h=0xcbf29ce484222325ull; const auto* p=static_cast<const uint8_t*>(bytes);
    for (size_t i=0;i<count;++i) { h^=p[i]; h*=0x100000001b3ull; }
    return h;
}
// fast-math consumers must not optimize a by-value floating classification.
#if defined(_MSC_VER)
__declspec(noinline)
#else
[[gnu::noinline]]
#endif
inline bool Finite(const float& value) {
    uint32_t bits; std::memcpy(&bits,&value,4);
    return (bits&0x7f800000u)!=0x7f800000u;
}
inline bool LittleEndian() { const uint32_t one=1; return *reinterpret_cast<const uint8_t*>(&one)==1; }
inline auto Key(const Mesh& m) { return std::make_tuple(m.geometry_digest,m.layout_digest,m.material_digest); }
inline bool Valid(const Mesh& m) {
    if (!m.geometry_digest || !m.layout_digest || !m.material_digest || !m.triangle_count ||
        m.samples.empty() || m.samples.size()>MaxSamplesPerMesh) return false;
    uint32_t last=0;
    for (size_t i=0;i<m.samples.size();++i) {
        const auto& s=m.samples[i];
        if (s.triangle_id>=m.triangle_count || (i && s.triangle_id<last) || s.albedo[3]!=255 ||
            s.params[2] || s.params[3]) return false;
        last=s.triangle_id;
        double n2=0;
        for (unsigned k=0;k<3;++k) {
            if (!Finite(s.position[k]) || !Finite(s.normal[k])) return false;
            n2+=double(s.normal[k])*s.normal[k];
        }
        if (n2<.98 || n2>1.02) return false;
    }
    return true;
}
inline bool Encode(const std::vector<Mesh>& meshes, std::vector<uint8_t>& out) {
    out.clear();
    if (!LittleEndian() || meshes.empty() || meshes.size()>MaxMeshes) return false;
    std::vector<const Mesh*> ordered;
    uint64_t size=sizeof(Header)+meshes.size()*sizeof(Record);
    for (const auto& m:meshes) {
        if (!Valid(m)) return false;
        size+=m.samples.size()*sizeof(Sample);
        if (size>MaxFileBytes) return false;
        ordered.push_back(&m);
    }
    std::sort(ordered.begin(),ordered.end(),[](auto* a,auto* b){return Key(*a)<Key(*b);});
    for (size_t i=1;i<ordered.size();++i) if (Key(*ordered[i-1])==Key(*ordered[i])) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size),0);
    Header header; header.mesh_count=static_cast<uint32_t>(meshes.size());
    uint64_t offset=sizeof(Header)+ordered.size()*sizeof(Record);
    for (size_t i=0;i<ordered.size();++i) {
        const auto& m=*ordered[i]; const uint64_t n=m.samples.size()*sizeof(Sample);
        const Record r{m.geometry_digest,m.layout_digest,m.material_digest,m.triangle_count,
                       static_cast<uint32_t>(m.samples.size()),offset,n};
        std::memcpy(bytes.data()+sizeof(Header)+i*sizeof(Record),&r,sizeof(r));
        std::memcpy(bytes.data()+offset,m.samples.data(),static_cast<size_t>(n)); offset+=n;
    }
    header.body_hash=Hash(bytes.data()+sizeof(Header),bytes.size()-sizeof(Header));
    std::memcpy(bytes.data(),&header,sizeof(header)); out.swap(bytes); return true;
}
inline bool Decode(const std::vector<uint8_t>& bytes, std::vector<Mesh>& out) {
    out.clear();
    if (!LittleEndian() || bytes.size()<sizeof(Header) || bytes.size()>MaxFileBytes) return false;
    Header h; std::memcpy(&h,bytes.data(),sizeof(h));
    if (h.magic!=0x31424353u || h.version!=1 || h.header_bytes!=sizeof(Header) ||
        h.record_bytes!=sizeof(Record) || h.sample_bytes!=sizeof(Sample) || h.flags || h.reserved ||
        !h.mesh_count || h.mesh_count>MaxMeshes ||
        uint64_t(h.mesh_count)*sizeof(Record)>bytes.size()-sizeof(Header) ||
        h.body_hash!=Hash(bytes.data()+sizeof(Header),bytes.size()-sizeof(Header))) return false;
    uint64_t offset=sizeof(Header)+uint64_t(h.mesh_count)*sizeof(Record);
    std::vector<Mesh> meshes; meshes.reserve(h.mesh_count);
    for (uint32_t i=0;i<h.mesh_count;++i) {
        Record r; std::memcpy(&r,bytes.data()+sizeof(Header)+i*sizeof(Record),sizeof(r));
        const uint64_t n=uint64_t(r.sample_count)*sizeof(Sample);
        // Canonical contiguous payloads prohibit directory aliasing, overlap,
        // gaps and out-of-file reads before allocating any sample storage.
        if (!r.sample_count || r.sample_count>MaxSamplesPerMesh || r.samples_offset!=offset ||
            r.samples_bytes!=n || offset>bytes.size() || n>bytes.size()-offset) return false;
        Mesh m; m.geometry_digest=r.geometry_digest; m.layout_digest=r.layout_digest;
        m.material_digest=r.material_digest; m.triangle_count=r.triangle_count;
        m.samples.resize(r.sample_count); std::memcpy(m.samples.data(),bytes.data()+offset,static_cast<size_t>(n));
        if (!Valid(m) || (!meshes.empty() && !(Key(meshes.back())<Key(m)))) return false;
        meshes.push_back(std::move(m)); offset+=n;
    }
    if (offset!=bytes.size()) return false;
    out.swap(meshes); return true;
}
inline bool Read(const char* path, std::vector<Mesh>& out) {
    out.clear(); if (!path || !*path) return false;
    FILE* f=std::fopen(path,"rb"); if (!f) return false;
    if (std::fseek(f,0,SEEK_END)!=0) { std::fclose(f); return false; }
    const long n=std::ftell(f);
    if (n<static_cast<long>(sizeof(Header)) || uint64_t(n)>MaxFileBytes || std::fseek(f,0,SEEK_SET)!=0) {
        std::fclose(f); return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    const bool ok=std::fread(bytes.data(),1,bytes.size(),f)==bytes.size(); std::fclose(f);
    return ok && Decode(bytes,out);
}
inline bool Write(const char* path, const std::vector<Mesh>& meshes) {
    if (!path || !*path) return false;
    std::vector<uint8_t> bytes; if (!Encode(meshes,bytes)) return false;
    static std::atomic<uint64_t> serial{0};
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary=std::string(path)+".tmp-"+std::to_string(stamp)+"-"+std::to_string(serial++);
    FILE* f=std::fopen(temporary.c_str(),"wbx"); if (!f) return false;
    bool ok=std::fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();
    if (std::fflush(f)!=0) ok=false;
    if (std::fclose(f)!=0) ok=false;
    if (!ok || std::rename(temporary.c_str(),path)!=0) { std::remove(temporary.c_str()); return false; }
    return true;
}
// Search the canonical sorted directory returned by Decode/Read.
inline uint32_t FindMesh(const std::vector<Mesh>& meshes, uint64_t geometry, uint64_t layout, uint64_t material) {
    if (!geometry || !layout || !material) return 0xffffffffu;
    const auto key=std::make_tuple(geometry,layout,material);
    const auto it=std::lower_bound(meshes.begin(),meshes.end(),key,[](const Mesh& m,const auto& k){return Key(m)<k;});
    return it!=meshes.end() && Key(*it)==key ? static_cast<uint32_t>(it-meshes.begin()) : 0xffffffffu;
}
} // namespace primal::graphics::rhi::card_boundary
