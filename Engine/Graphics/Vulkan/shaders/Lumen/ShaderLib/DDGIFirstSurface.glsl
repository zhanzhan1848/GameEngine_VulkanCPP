#ifndef LUMEN_DDGI_FIRST_SURFACE_GLSL
#define LUMEN_DDGI_FIRST_SURFACE_GLSL

// DDGI_FIRST_SURFACE=1 geometry candidate.
//
// The node/triangle byte layout is shared with SCGeometrySupportProof:
//   node  = vec4(lo), vec4(hi), uvec4(links)
//   tri   = vec4(a),  vec4(e1), vec4(e2)
// Triangle metadata is intentionally carried in the padding lanes:
//   a.w  = float triangle id
//   e1.w = uintBitsToFloat(material flags)
//   e2.w = float mesh id
//
// This helper certifies only a finite geometric first intersection. Material
// acceptance, the incumbent-normal guard, and the distance replacement stay in
// DDGITraceSDF.comp so a failed/unsupported query cannot turn into sky.

struct DDGIFirstSurfaceNode {
    vec3 lo;
    vec3 hi;
    uvec4 links;
};

struct DDGIFirstSurfaceTriangle {
    vec4 a;
    vec4 e1;
    vec4 e2;
};

struct DDGIFirstSurfaceHit {
    uint status;       // 0 unknown, 1 geometric hit, 2 segment miss,
                       // 4 endpoint, 5 barycentric residual, 6 overflow
    float t;
    float triangle;    // numeric triangle id from a.w
    uint flags;
    float residual;
    vec3 bary;
#if defined(DDGI_FIRST_NATIVE_NORMAL_VARIANT) && DDGI_FIRST_NATIVE_NORMAL_VARIANT
    // Child-only source payload.  The parent first-surface shader never sees
    // these fields, so its preprocessed source and binary stay unchanged.
    vec3 source_a;
    vec3 source_e1;
    vec3 source_e2;
    float source_mesh;
#endif
};

bool ddgiFirstFinite(float v) {
    return !isnan(v) && !isinf(v);
}

bool ddgiFirstFiniteVec3(vec3 v) {
    return !any(isnan(v)) && !any(isinf(v));
}

DDGIFirstSurfaceNode ddgiFirstLoadNode(uint at) {
    DDGIFirstSurfaceNode node;
    node.lo = uintBitsToFloat(first_surface_nodes.node_words[at * 3u + 0u]).xyz;
    node.hi = uintBitsToFloat(first_surface_nodes.node_words[at * 3u + 1u]).xyz;
    node.links = first_surface_nodes.node_words[at * 3u + 2u];
    return node;
}

DDGIFirstSurfaceTriangle ddgiFirstLoadTriangle(uint at) {
    DDGIFirstSurfaceTriangle tri;
    tri.a = first_surface_triangles.triangle_words[at * 3u + 0u];
    tri.e1 = first_surface_triangles.triangle_words[at * 3u + 1u];
    tri.e2 = first_surface_triangles.triangle_words[at * 3u + 2u];
    return tri;
}

bool ddgiFirstRayBox(vec3 origin, vec3 direction,
                     DDGIFirstSurfaceNode node,
                     float tMin, float tMax, out float nearT) {
    float lo = tMin;
    float hi = tMax;
    for (uint k = 0u; k < 3u; ++k) {
        float d = direction[k];
        if (abs(d) <= 1.0e-20) {
            if (origin[k] < node.lo[k] || origin[k] > node.hi[k]) return false;
            continue;
        }
        float ta = (node.lo[k] - origin[k]) / d;
        float tb = (node.hi[k] - origin[k]) / d;
        lo = max(lo, min(ta, tb));
        hi = min(hi, max(ta, tb));
        if (lo > hi) return false;
    }
    nearT = lo;
    return ddgiFirstFinite(nearT);
}

DDGIFirstSurfaceHit ddgiFirstSurfaceTrace(vec3 origin, vec3 direction,
                                           float tMin, float tMax) {
    DDGIFirstSurfaceHit result;
    result.status = 0u;
    result.t = -1.0;
    result.triangle = -1.0;
    result.flags = 0u;
    result.residual = 0.0;
    result.bary = vec3(0.0);
#if defined(DDGI_FIRST_NATIVE_NORMAL_VARIANT) && DDGI_FIRST_NATIVE_NORMAL_VARIANT
    result.source_a = vec3(0.0);
    result.source_e1 = vec3(0.0);
    result.source_e2 = vec3(0.0);
    result.source_mesh = -1.0;
#endif

    const uint nodeWordCount = first_surface_nodes.node_words.length();
    const uint triangleWordCount = first_surface_triangles.triangle_words.length();
    if (nodeWordCount == 0u || triangleWordCount == 0u ||
        (nodeWordCount % 3u) != 0u || (triangleWordCount % 3u) != 0u ||
        !ddgiFirstFiniteVec3(origin) || !ddgiFirstFiniteVec3(direction) ||
        !ddgiFirstFinite(tMin) || !ddgiFirstFinite(tMax) ||
        tMin < 0.0 || tMax <= tMin) {
        return result;
    }
    const uint nodeCount = nodeWordCount / 3u;
    const uint triangleCount = triangleWordCount / 3u;
    if (nodeCount == 0u || triangleCount == 0u ||
        abs(dot(direction, direction) - 1.0) > 2.0e-5) {
        return result;
    }

    uint stack[64];
    uint stackSize = 1u;
    stack[0] = 0u;
    uint visited = 0u;
    float bestT = tMax;
    uint bestTriangleIndex = 0xffffffffu;
    float bestTriangleId = 0.0;
    uint bestFlags = 0u;
    vec3 bestBary = vec3(0.0);

    while (stackSize > 0u) {
        if (stackSize > 64u || visited >= nodeCount) {
            result.status = 6u;
            return result;
        }
        const uint nodeIndex = stack[--stackSize];
        if (nodeIndex >= nodeCount) return result;
        DDGIFirstSurfaceNode node = ddgiFirstLoadNode(nodeIndex);
        if (!ddgiFirstFiniteVec3(node.lo) || !ddgiFirstFiniteVec3(node.hi) ||
            any(greaterThan(node.lo, node.hi))) {
            return result;
        }
        ++visited;
        float nodeNear = 0.0;
        if (!ddgiFirstRayBox(origin, direction, node, tMin, bestT + 2.0e-5, nodeNear)) {
            continue;
        }

        if (node.links.w > 0u) {
            if (node.links.z >= triangleCount ||
                node.links.w > triangleCount - node.links.z) {
                return result;
            }
            for (uint i = 0u; i < node.links.w; ++i) {
                DDGIFirstSurfaceTriangle tri = ddgiFirstLoadTriangle(node.links.z + i);
                if (!ddgiFirstFiniteVec3(tri.a.xyz) ||
                    !ddgiFirstFiniteVec3(tri.e1.xyz) ||
                    !ddgiFirstFiniteVec3(tri.e2.xyz) ||
                    !ddgiFirstFinite(tri.a.w) ||
                    !ddgiFirstFinite(tri.e2.w)) {
                    return result;
                }
                vec3 p = cross(direction, tri.e2.xyz);
                float det = dot(tri.e1.xyz, p);
                if (!ddgiFirstFinite(det) || abs(det) <= 1.0e-12) continue;
                vec3 delta = origin - tri.a.xyz;
                float u = dot(delta, p) / det;
                vec3 q = cross(delta, tri.e1.xyz);
                float v = dot(direction, q) / det;
                float t = dot(tri.e2.xyz, q) / det;
                if (!ddgiFirstFinite(u) || !ddgiFirstFinite(v) ||
                    !ddgiFirstFinite(t)) return result;
                if (u < 0.0 || v < 0.0 || u + v > 1.0 ||
                    t < tMin - 2.0e-5 || t > bestT + 2.0e-5) continue;
                // a.w is a numeric float triangle id by contract.
                const float triangleId = tri.a.w;
                if (t < bestT ||
                    (t == bestT &&
                     (bestTriangleIndex == 0xffffffffu || triangleId < bestTriangleId))) {
                    bestT = t;
                    bestTriangleIndex = node.links.z + i;
                    bestTriangleId = triangleId;
                    bestFlags = floatBitsToUint(tri.e1.w);
                    bestBary = vec3(1.0 - u - v, u, v);
#if defined(DDGI_FIRST_NATIVE_NORMAL_VARIANT) && DDGI_FIRST_NATIVE_NORMAL_VARIANT
                    result.source_a = tri.a.xyz;
                    result.source_e1 = tri.e1.xyz;
                    result.source_e2 = tri.e2.xyz;
                    result.source_mesh = tri.e2.w;
#endif
                }
            }
        } else {
            const uint left = node.links.x;
            const uint right = node.links.y;
            if (left >= nodeCount || right >= nodeCount) return result;
            DDGIFirstSurfaceNode leftNode = ddgiFirstLoadNode(left);
            DDGIFirstSurfaceNode rightNode = ddgiFirstLoadNode(right);
            if (!ddgiFirstFiniteVec3(leftNode.lo) || !ddgiFirstFiniteVec3(leftNode.hi) ||
                !ddgiFirstFiniteVec3(rightNode.lo) || !ddgiFirstFiniteVec3(rightNode.hi) ||
                any(greaterThan(leftNode.lo, leftNode.hi)) ||
                any(greaterThan(rightNode.lo, rightNode.hi))) return result;
            float leftNear = 0.0;
            float rightNear = 0.0;
            const bool hitLeft = ddgiFirstRayBox(origin, direction, leftNode,
                                                 tMin, bestT + 2.0e-5, leftNear);
            const bool hitRight = ddgiFirstRayBox(origin, direction, rightNode,
                                                  tMin, bestT + 2.0e-5, rightNear);
            const uint needed = (hitLeft ? 1u : 0u) + (hitRight ? 1u : 0u);
            if (stackSize + needed > 64u) {
                result.status = 6u;
                return result;
            }
            // Push farther first so the nearer child is visited first.  This
            // keeps the bestT pruning deterministic without changing the
            // first-intersection semantics.
            if (hitLeft && hitRight) {
                if (leftNear <= rightNear) {
                    stack[stackSize++] = right;
                    stack[stackSize++] = left;
                } else {
                    stack[stackSize++] = left;
                    stack[stackSize++] = right;
                }
            } else if (hitLeft) {
                stack[stackSize++] = left;
            } else if (hitRight) {
                stack[stackSize++] = right;
            }
        }
    }

    if (bestTriangleIndex == 0xffffffffu || !ddgiFirstFinite(bestT)) {
        result.status = 2u; // geometric segment miss, never a sky decision
        return result;
    }
    if (bestT <= tMin + 2.0e-5 || bestT >= tMax - 2.0e-5) {
        result.status = 4u;
        result.t = bestT;
        result.triangle = bestTriangleId;
        result.flags = bestFlags;
        result.bary = bestBary;
        return result;
    }

    // Keep the reconstruction in the same float32 arithmetic domain as the
    // runtime candidate. The host independently recomputes this residual in
    // float64 and applies the same strict 2e-5 m acceptance guard.
    DDGIFirstSurfaceTriangle best = ddgiFirstLoadTriangle(bestTriangleIndex);
    vec3 reconstructed = best.a.xyz + bestBary.y * best.e1.xyz +
                                 bestBary.z * best.e2.xyz;
    vec3 rayPoint = origin + direction * bestT;
    // A `precise` qualifier here makes glslang propagate NoContraction back
    // through the entire nested SDF/BVH graph (unbounded compiler memory in
    // the prototype). Instead bound float32 reconstruction roundoff locally.
    // Four machine epsilons over all absolute terms conservatively cover the
    // two products/two sums, ray product/sum, subtraction and norm rounding.
    // Eligibility still requires the residual upper bound <= the fixed 2e-5.
    vec3 absoluteTerms = abs(best.a.xyz) + abs(bestBary.y * best.e1.xyz) +
                         abs(bestBary.z * best.e2.xyz) + abs(origin) + abs(direction * bestT);
    float roundoffBudget = 4.0 * 1.1920928955078125e-7 * length(absoluteTerms);
    float residual = length(reconstructed - rayPoint) + roundoffBudget;
    if (!ddgiFirstFinite(residual)) return result;
    result.t = bestT;
    result.triangle = bestTriangleId;
    result.flags = bestFlags;
    result.residual = residual;
    result.bary = bestBary;
    if (residual > 2.0e-5) {
        result.status = 5u;
        return result;
    }
    result.status = 1u;
    return result;
}

#endif // LUMEN_DDGI_FIRST_SURFACE_GLSL
