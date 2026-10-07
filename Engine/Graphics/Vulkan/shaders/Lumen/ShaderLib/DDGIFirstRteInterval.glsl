#ifndef LUMEN_DDGI_FIRST_RTE_INTERVAL_GLSL
#define LUMEN_DDGI_FIRST_RTE_INTERVAL_GLSL

// Private DDGI_FIRST_RECORD_INTERVAL proof only.  This file deliberately
// does not participate in DDGITraceSDF or any production HN write.  The
// interval evaluator re-reads the same first hit and resident source records
// as DDGIFirstNativeNormal.glsl, but never consumes its reconstructed normal.
//
// The RTE lane encloses the legal strict f32 path.  Its add/sub/mul endpoint
// operations are monotone binary32 operations and multiplication evaluates
// all four corners.  The real lane is a private conceptual source enclosure:
// each basic endpoint is expanded by an adjacent binary32 value and the
// packed 2/65535 constant is bracketed independently.  It is not a claim
// that f32 shader arithmetic is exact real arithmetic.  The host must join
// this tail with its independent source/point/UV/quad audit before any
// interpretation of the 2e-5 gate.

const float DDGI_FIRST_INTERVAL_F32_MIN_NORMAL = 1.1754943508222875e-38;
// The runtime contract used by this private proof does not certify an
// operation whose exact nonzero result can be flushed or rounded through the
// denormal interval.  Keep a one-binade safety margin around the minimum
// normal rather than treating a rounded zero as an exact cancellation.
const float DDGI_FIRST_INTERVAL_F32_MIN_SAFE = 2.350988701644575e-38;
const float DDGI_FIRST_INTERVAL_F32_MAX = 3.4028234663852886e+38;
const float DDGI_FIRST_INTERVAL_FDIV_MIN = 1.1754943508222875e-38;
const float DDGI_FIRST_INTERVAL_FDIV_MAX = 8.507059173023462e+37; // 2^126
const float DDGI_FIRST_INTERVAL_GATE = 2.0e-5;
const float DDGI_FIRST_INTERVAL_INV_F32 = 0.0000305180437862873077392578125;
const float DDGI_FIRST_INTERVAL_EPS = 1.1920928955078125e-7;

// Tail ABI: [0,41) is the existing proof record.  [41,69) is this tail.
const uint DDGI_FIRST_INTERVAL_TAIL_VEC4 = 28u;
const uint DDGI_FIRST_INTERVAL_STRIDE = 69u;
const uint DDGI_FIRST_INTERVAL_SCHEMA = 1u;

// Header status.  A complete status is still private arithmetic evidence;
// it is not a production or primary/raster-frame certificate.
const uint DDGI_FIRST_INTERVAL_STATUS_UNKNOWN = 0u;
const uint DDGI_FIRST_INTERVAL_STATUS_COMPLETE = 1u;

// Reason bits are intentionally independent from DDGI_FIRST_NATIVE status
// bits.  The host can retain overlapping domain/branch failures.
const uint DDGI_FIRST_INTERVAL_REASON_EARLY_PAYLOAD = 1u << 0u;
const uint DDGI_FIRST_INTERVAL_REASON_SOURCE = 1u << 1u;
const uint DDGI_FIRST_INTERVAL_REASON_MATERIAL = 1u << 2u;
const uint DDGI_FIRST_INTERVAL_REASON_NONFINITE = 1u << 3u;
const uint DDGI_FIRST_INTERVAL_REASON_SUBNORMAL = 1u << 4u;
const uint DDGI_FIRST_INTERVAL_REASON_SIGNED_ZERO = 1u << 5u;
const uint DDGI_FIRST_INTERVAL_REASON_OVERFLOW = 1u << 6u;
const uint DDGI_FIRST_INTERVAL_REASON_DIVISION = 1u << 7u;
const uint DDGI_FIRST_INTERVAL_REASON_INVERSE_SQRT = 1u << 8u;
const uint DDGI_FIRST_INTERVAL_REASON_THRESHOLD = 1u << 9u;
const uint DDGI_FIRST_INTERVAL_REASON_BRANCH_MISMATCH = 1u << 10u;
const uint DDGI_FIRST_INTERVAL_REASON_TEXTURE = 1u << 11u;
const uint DDGI_FIRST_INTERVAL_REASON_ZERO_LENGTH = 1u << 12u;
const uint DDGI_FIRST_INTERVAL_REASON_BOUND_REJECTED = 1u << 13u;
const uint DDGI_FIRST_INTERVAL_REASON_WORLD_MATRIX = 1u << 14u;

// Branch bits describe the private semantic path used for the interval
// payload.  They never select a production path.
const uint DDGI_FIRST_INTERVAL_BRANCH_PACKED_FALLBACK = 1u << 0u;
const uint DDGI_FIRST_INTERVAL_BRANCH_PACKED_REGULAR = 1u << 1u;
const uint DDGI_FIRST_INTERVAL_BRANCH_PACKED_EXACT_AXIS = 1u << 2u;
const uint DDGI_FIRST_INTERVAL_BRANCH_GRAM = 1u << 3u;
const uint DDGI_FIRST_INTERVAL_BRANCH_FALLBACK_UP = 1u << 4u;
const uint DDGI_FIRST_INTERVAL_BRANCH_PRIMARY_UP_X = 1u << 5u;
const uint DDGI_FIRST_INTERVAL_BRANCH_PRIMARY_UP_Y = 1u << 6u;
const uint DDGI_FIRST_INTERVAL_BRANCH_SAMPLE_PATH1 = 1u << 7u;
const uint DDGI_FIRST_INTERVAL_BRANCH_SAMPLE_PATH2 = 1u << 8u;
const uint DDGI_FIRST_INTERVAL_BRANCH_NATIVE_FLIP = 1u << 9u;
const uint DDGI_FIRST_INTERVAL_BRANCH_GEOM_FLIP = 1u << 10u;
const uint DDGI_FIRST_INTERVAL_BRANCH_ALIGN_FLIP = 1u << 11u;

struct DDGIFirstIntervalScalar {
    float lo;
    float hi;
    uint valid;
};

struct DDGIFirstIntervalVec3 {
    vec3 lo;
    vec3 hi;
    uint valid;
};

struct DDGIFirstIntervalPairScalar {
    DDGIFirstIntervalScalar rte;
    DDGIFirstIntervalScalar real;
};

struct DDGIFirstIntervalPairVec3 {
    DDGIFirstIntervalVec3 rte;
    DDGIFirstIntervalVec3 real;
};

struct DDGIFirstIntervalState {
    uint reason_bits;
    uint branch_bits;
    uint operation_count;
    uint rte_domain_valid;
    uint real_domain_valid;
};

struct DDGIFirstIntervalFrame {
    DDGIFirstIntervalPairVec3 normal;
    DDGIFirstIntervalPairVec3 tangent;
};

struct DDGIFirstRteIntervalResult {
    uint status;
    uint reason_bits;
    uint branch_bits;
    uint operation_count;
    uint rte_domain_valid;
    uint real_domain_valid;
    uint math_gate_pass;
    float bary_l2;
    float tangent_l2;
    float ray_l2;
    float geometry_l2;
    float bitangent_l2;
    float mapped_l2;
    float max_l2;
    DDGIFirstIntervalPairVec3 bary_native_n;
    DDGIFirstIntervalPairVec3 tangent;
    DDGIFirstIntervalPairVec3 ray_native_n;
    DDGIFirstIntervalPairVec3 geometry_n;
    DDGIFirstIntervalPairVec3 bitangent;
    DDGIFirstIntervalPairVec3 mapped_n;
};

bool ddgiFirstIntervalFiniteNormal(float value) {
    return ddgiFirstFinite(value) &&
           (value == 0.0 || abs(value) >= DDGI_FIRST_INTERVAL_F32_MIN_NORMAL) &&
           abs(value) <= DDGI_FIRST_INTERVAL_F32_MAX;
}

float ddgiFirstIntervalNextUp(float value) {
    if (value == 0.0) return uintBitsToFloat(1u);
    uint bits = floatBitsToUint(value);
    if ((bits & 0x80000000u) != 0u) return uintBitsToFloat(bits - 1u);
    return uintBitsToFloat(bits + 1u);
}

float ddgiFirstIntervalNextDown(float value) {
    if (value == 0.0) return uintBitsToFloat(0x80000001u);
    uint bits = floatBitsToUint(value);
    if ((bits & 0x80000000u) != 0u) return uintBitsToFloat(bits + 1u);
    return uintBitsToFloat(bits - 1u);
}

float ddgiFirstIntervalStepUp(float value, uint steps) {
    float result = value;
    for (uint i = 0u; i < steps; ++i) result = ddgiFirstIntervalNextUp(result);
    return result;
}

float ddgiFirstIntervalStepDown(float value, uint steps) {
    float result = value;
    for (uint i = 0u; i < steps; ++i) result = ddgiFirstIntervalNextDown(result);
    return result;
}

void ddgiFirstIntervalRteFailure(inout DDGIFirstIntervalState state, uint reason) {
    state.reason_bits |= reason;
    state.rte_domain_valid = 0u;
}

void ddgiFirstIntervalRealFailure(inout DDGIFirstIntervalState state, uint reason) {
    state.reason_bits |= reason;
    state.real_domain_valid = 0u;
}

void ddgiFirstIntervalBothFailure(inout DDGIFirstIntervalState state, uint reason) {
    state.reason_bits |= reason;
    state.rte_domain_valid = 0u;
    state.real_domain_valid = 0u;
}

bool ddgiFirstIntervalAssignScalar(out DDGIFirstIntervalScalar value,
                                   float lo, float hi,
                                   inout DDGIFirstIntervalState state,
                                   bool real_lane) {
    value.lo = lo;
    value.hi = hi;
    value.valid = 1u;
    if (!ddgiFirstFinite(lo) || !ddgiFirstFinite(hi) || lo > hi) {
        value.valid = 0u;
        if (real_lane) ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
        else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
        return false;
    }
    if ((lo != 0.0 && abs(lo) < DDGI_FIRST_INTERVAL_F32_MIN_NORMAL) ||
        (hi != 0.0 && abs(hi) < DDGI_FIRST_INTERVAL_F32_MIN_NORMAL)) {
        value.valid = 0u;
        if (real_lane) ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_SUBNORMAL);
        else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_SUBNORMAL);
        return false;
    }
    // Numeric singleton zero is the only signed-zero scope.  Any interval
    // that reaches zero from a nonzero side is UNKNOWN.
    if ((lo < 0.0 && hi >= 0.0) || (lo <= 0.0 && hi > 0.0)) {
        value.valid = 0u;
        if (real_lane) ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_SIGNED_ZERO);
        else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_SIGNED_ZERO);
        return false;
    }
    return true;
}

// A core add/sub/mul/div result is allowed to be zero only when the exact
// operation is known to produce zero.  This is deliberately stricter than the
// generic resident-value assignment: no-denorm-preserve devices may flush a
// nonzero result to zero, and an interval touching zero has an unknown sign.
bool ddgiFirstIntervalAssignCoreScalar(out DDGIFirstIntervalScalar value,
                                       float lo, float hi,
                                       inout DDGIFirstIntervalState state,
                                       bool real_lane, bool allow_zero) {
    value.lo = lo;
    value.hi = hi;
    value.valid = 1u;
    if (!ddgiFirstFinite(lo) || !ddgiFirstFinite(hi) || lo > hi) {
        value.valid = 0u;
        if (real_lane) ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
        else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
        return false;
    }
    if (lo == 0.0 && hi == 0.0) {
        if (allow_zero) return true;
        value.valid = 0u;
        if (real_lane) ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_SIGNED_ZERO);
        else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_SIGNED_ZERO);
        return false;
    }
    if ((lo <= 0.0 && hi >= 0.0) ||
        (lo != 0.0 && abs(lo) < DDGI_FIRST_INTERVAL_F32_MIN_SAFE) ||
        (hi != 0.0 && abs(hi) < DDGI_FIRST_INTERVAL_F32_MIN_SAFE)) {
        value.valid = 0u;
        if (real_lane) ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_SUBNORMAL);
        else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_SUBNORMAL);
        return false;
    }
    return true;
}

bool ddgiFirstIntervalAssignVec(out DDGIFirstIntervalVec3 value,
                                vec3 lo, vec3 hi,
                                inout DDGIFirstIntervalState state,
                                bool real_lane) {
    value.lo = lo;
    value.hi = hi;
    value.valid = 1u;
    DDGIFirstIntervalScalar x, y, z;
    bool ok = ddgiFirstIntervalAssignScalar(x, lo.x, hi.x, state, real_lane) &&
              ddgiFirstIntervalAssignScalar(y, lo.y, hi.y, state, real_lane) &&
              ddgiFirstIntervalAssignScalar(z, lo.z, hi.z, state, real_lane);
    if (!ok) value.valid = 0u;
    return ok;
}

DDGIFirstIntervalScalar ddgiFirstIntervalInvalidScalar() {
    DDGIFirstIntervalScalar value;
    value.lo = DDGI_FIRST_INTERVAL_F32_MAX;
    value.hi = DDGI_FIRST_INTERVAL_F32_MAX;
    value.valid = 0u;
    return value;
}

DDGIFirstIntervalVec3 ddgiFirstIntervalInvalidVec() {
    DDGIFirstIntervalVec3 value;
    value.lo = vec3(DDGI_FIRST_INTERVAL_F32_MAX);
    value.hi = vec3(DDGI_FIRST_INTERVAL_F32_MAX);
    value.valid = 0u;
    return value;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalInvalidPairScalar() {
    DDGIFirstIntervalPairScalar value;
    value.rte = ddgiFirstIntervalInvalidScalar();
    value.real = ddgiFirstIntervalInvalidScalar();
    return value;
}

DDGIFirstIntervalPairVec3 ddgiFirstIntervalInvalidPairVec() {
    DDGIFirstIntervalPairVec3 value;
    value.rte = ddgiFirstIntervalInvalidVec();
    value.real = ddgiFirstIntervalInvalidVec();
    return value;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalResidentScalar(
    float value, inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    ddgiFirstIntervalAssignScalar(result.rte, value, value, state, false);
    ddgiFirstIntervalAssignScalar(result.real, value, value, state, true);
    return result;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalExactConstant(
    float value, inout DDGIFirstIntervalState state) {
    // Exact binary32 constants are exact in the real lane as well.
    return ddgiFirstIntervalResidentScalar(value, state);
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalPackedInverse(
    inout DDGIFirstIntervalState state) {
    // The RTE lane starts from the resident f32 value used by the helper.  The
    // real lane brackets exact 2/65535 with adjacent f32 values, without
    // feeding a float64 ideal into the GPU path.
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    ddgiFirstIntervalAssignScalar(result.rte, DDGI_FIRST_INTERVAL_INV_F32,
                                  DDGI_FIRST_INTERVAL_INV_F32, state, false);
    float real_lo = ddgiFirstIntervalNextDown(DDGI_FIRST_INTERVAL_INV_F32);
    float real_hi = ddgiFirstIntervalNextUp(DDGI_FIRST_INTERVAL_INV_F32);
    ddgiFirstIntervalAssignScalar(result.real, real_lo, real_hi, state, true);
    return result;
}

bool ddgiFirstIntervalExpandReal(float value, bool lower, uint steps,
                                 bool allow_zero,
                                 out float expanded,
                                 inout DDGIFirstIntervalState state) {
    if (!ddgiFirstIntervalFiniteNormal(value)) {
        expanded = DDGI_FIRST_INTERVAL_F32_MAX;
        ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
        return false;
    }
    if (value == 0.0) {
        if (!allow_zero) {
            expanded = 0.0;
            ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_SIGNED_ZERO);
            return false;
        }
        expanded = 0.0;
        return true;
    }
    expanded = lower ? ddgiFirstIntervalStepDown(value, steps) :
                       ddgiFirstIntervalStepUp(value, steps);
    if (!ddgiFirstFinite(expanded) ||
        (expanded != 0.0 && abs(expanded) < DDGI_FIRST_INTERVAL_F32_MIN_SAFE)) {
        ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_SUBNORMAL);
        return false;
    }
    return true;
}

void ddgiFirstIntervalAddOperation(inout DDGIFirstIntervalState state) {
    state.operation_count += 1u;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalAdd(
    DDGIFirstIntervalPairScalar a, DDGIFirstIntervalPairScalar b,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    ddgiFirstIntervalAddOperation(state);
    if (a.rte.valid != 0u && b.rte.valid != 0u) {
        float lo = a.rte.lo + b.rte.lo;
        float hi = a.rte.hi + b.rte.hi;
        bool exact_zero = lo == 0.0 && hi == 0.0 &&
                          a.rte.lo == -b.rte.lo && a.rte.hi == -b.rte.hi;
        ddgiFirstIntervalAssignCoreScalar(result.rte, lo, hi, state, false, exact_zero);
    } else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
    if (a.real.valid != 0u && b.real.valid != 0u) {
        float lo, hi;
        float raw_lo = a.real.lo + b.real.lo;
        float raw_hi = a.real.hi + b.real.hi;
        bool exact_zero = raw_lo == 0.0 && raw_hi == 0.0 &&
                          a.real.lo == -b.real.lo && a.real.hi == -b.real.hi;
        bool ok = ddgiFirstIntervalExpandReal(raw_lo, true, 1u, exact_zero, lo, state) &&
                  ddgiFirstIntervalExpandReal(raw_hi, false, 1u, exact_zero, hi, state);
        if (ok) ddgiFirstIntervalAssignCoreScalar(result.real, lo, hi, state, true, exact_zero);
    } else ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
    return result;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalSub(
    DDGIFirstIntervalPairScalar a, DDGIFirstIntervalPairScalar b,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    ddgiFirstIntervalAddOperation(state);
    if (a.rte.valid != 0u && b.rte.valid != 0u) {
        float lo = a.rte.lo - b.rte.hi;
        float hi = a.rte.hi - b.rte.lo;
        bool exact_zero = lo == 0.0 && hi == 0.0 &&
                          a.rte.lo == b.rte.hi && a.rte.hi == b.rte.lo;
        ddgiFirstIntervalAssignCoreScalar(result.rte, lo, hi, state, false, exact_zero);
    } else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
    if (a.real.valid != 0u && b.real.valid != 0u) {
        float lo, hi;
        float raw_lo = a.real.lo - b.real.hi;
        float raw_hi = a.real.hi - b.real.lo;
        bool exact_zero = raw_lo == 0.0 && raw_hi == 0.0 &&
                          a.real.lo == b.real.hi && a.real.hi == b.real.lo;
        bool ok = ddgiFirstIntervalExpandReal(raw_lo, true, 1u, exact_zero, lo, state) &&
                  ddgiFirstIntervalExpandReal(raw_hi, false, 1u, exact_zero, hi, state);
        if (ok) ddgiFirstIntervalAssignCoreScalar(result.real, lo, hi, state, true, exact_zero);
    } else ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
    return result;
}

float ddgiFirstIntervalMin4(float a, float b, float c, float d) {
    return min(min(a, b), min(c, d));
}

float ddgiFirstIntervalMax4(float a, float b, float c, float d) {
    return max(max(a, b), max(c, d));
}

bool ddgiFirstIntervalDivisorDomain(DDGIFirstIntervalScalar value) {
    return value.valid != 0u &&
           !(value.lo <= 0.0 && value.hi >= 0.0) &&
           abs(value.lo) >= DDGI_FIRST_INTERVAL_FDIV_MIN &&
           abs(value.hi) >= DDGI_FIRST_INTERVAL_FDIV_MIN &&
           abs(value.lo) <= DDGI_FIRST_INTERVAL_FDIV_MAX &&
           abs(value.hi) <= DDGI_FIRST_INTERVAL_FDIV_MAX;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalMul(
    DDGIFirstIntervalPairScalar a, DDGIFirstIntervalPairScalar b,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    ddgiFirstIntervalAddOperation(state);
    if (a.rte.valid != 0u && b.rte.valid != 0u) {
        float r0 = a.rte.lo * b.rte.lo;
        float r1 = a.rte.lo * b.rte.hi;
        float r2 = a.rte.hi * b.rte.lo;
        float r3 = a.rte.hi * b.rte.hi;
        bool exact_zero = (a.rte.lo == 0.0 && a.rte.hi == 0.0) ||
                          (b.rte.lo == 0.0 && b.rte.hi == 0.0);
        ddgiFirstIntervalAssignCoreScalar(result.rte,
                                          ddgiFirstIntervalMin4(r0, r1, r2, r3),
                                          ddgiFirstIntervalMax4(r0, r1, r2, r3),
                                          state, false, exact_zero);
    } else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
    if (a.real.valid != 0u && b.real.valid != 0u) {
        float p0 = a.real.lo * b.real.lo;
        float p1 = a.real.lo * b.real.hi;
        float p2 = a.real.hi * b.real.lo;
        float p3 = a.real.hi * b.real.hi;
        float loValue = ddgiFirstIntervalMin4(p0, p1, p2, p3);
        float hiValue = ddgiFirstIntervalMax4(p0, p1, p2, p3);
        float lo, hi;
        bool exact_zero = (a.real.lo == 0.0 && a.real.hi == 0.0) ||
                          (b.real.lo == 0.0 && b.real.hi == 0.0);
        bool ok = ddgiFirstIntervalExpandReal(loValue, true, 1u, exact_zero, lo, state) &&
                  ddgiFirstIntervalExpandReal(hiValue, false, 1u, exact_zero, hi, state);
        if (ok) ddgiFirstIntervalAssignCoreScalar(result.real, lo, hi, state, true, exact_zero);
    } else ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
    return result;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalNeg(
    DDGIFirstIntervalPairScalar value, inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    if (value.rte.valid != 0u)
        ddgiFirstIntervalAssignScalar(result.rte, -value.rte.hi, -value.rte.lo, state, false);
    else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
    if (value.real.valid != 0u)
        ddgiFirstIntervalAssignScalar(result.real, -value.real.hi, -value.real.lo, state, true);
    else ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
    return result;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalAbs(
    DDGIFirstIntervalPairScalar value, inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    if (value.rte.valid != 0u) {
        float lo = (value.rte.lo <= 0.0 && value.rte.hi >= 0.0) ? 0.0 :
                   min(abs(value.rte.lo), abs(value.rte.hi));
        float hi = max(abs(value.rte.lo), abs(value.rte.hi));
        ddgiFirstIntervalAssignScalar(result.rte, lo, hi, state, false);
    } else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
    if (value.real.valid != 0u) {
        float lo = (value.real.lo <= 0.0 && value.real.hi >= 0.0) ? 0.0 :
                   min(abs(value.real.lo), abs(value.real.hi));
        float hi = max(abs(value.real.lo), abs(value.real.hi));
        // Absolute value intentionally permits a zero endpoint.  The branch
        // that consumes it must still reject an uncertain sign/threshold.
        result.real.lo = lo;
        result.real.hi = hi;
        result.real.valid = 1u;
    } else ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
    return result;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalMaxZero(
    DDGIFirstIntervalPairScalar value, inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = value;
    if (value.rte.valid != 0u) {
        if (value.rte.lo < 0.0 && value.rte.hi > 0.0) {
            state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_THRESHOLD;
            state.rte_domain_valid = 0u;
            result.rte.lo = 0.0;
        } else {
            result.rte.lo = max(0.0, value.rte.lo);
            result.rte.hi = max(0.0, value.rte.hi);
        }
    }
    if (value.real.valid != 0u) {
        if (value.real.lo < 0.0 && value.real.hi > 0.0) {
            state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_THRESHOLD;
            state.real_domain_valid = 0u;
            result.real.lo = 0.0;
        } else {
            result.real.lo = max(0.0, value.real.lo);
            result.real.hi = max(0.0, value.real.hi);
        }
    }
    return result;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalDiv(
    DDGIFirstIntervalPairScalar a, DDGIFirstIntervalPairScalar b,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    ddgiFirstIntervalAddOperation(state);
    bool rte_zero_numerator = a.rte.valid != 0u &&
                              a.rte.lo == 0.0 && a.rte.hi == 0.0;
    bool real_zero_numerator = a.real.valid != 0u &&
                               a.real.lo == 0.0 && a.real.hi == 0.0;
    if (rte_zero_numerator && ddgiFirstIntervalDivisorDomain(b.rte)) {
        ddgiFirstIntervalAssignCoreScalar(result.rte, 0.0, 0.0, state, false, true);
    } else if (a.rte.valid != 0u && ddgiFirstIntervalDivisorDomain(b.rte)) {
        float r0 = a.rte.lo / b.rte.lo;
        float r1 = a.rte.lo / b.rte.hi;
        float r2 = a.rte.hi / b.rte.lo;
        float r3 = a.rte.hi / b.rte.hi;
        float lo = ddgiFirstIntervalMin4(r0, r1, r2, r3);
        float hi = ddgiFirstIntervalMax4(r0, r1, r2, r3);
        // Each endpoint quotient has the 2.5-ULP FDiv allowance.  Ten
        // adjacent values cover both directions and the largest spacing at a
        // binade boundary; this is deliberately wider than a nominal 6-ULP
        // count because the proof must fail closed across binades.
        lo = ddgiFirstIntervalStepDown(lo, 10u);
        hi = ddgiFirstIntervalStepUp(hi, 10u);
        ddgiFirstIntervalAssignCoreScalar(result.rte, lo, hi, state, false, false);
    } else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_DIVISION);
    if (real_zero_numerator && ddgiFirstIntervalDivisorDomain(b.real)) {
        ddgiFirstIntervalAssignCoreScalar(result.real, 0.0, 0.0, state, true, true);
    } else if (a.real.valid != 0u && ddgiFirstIntervalDivisorDomain(b.real)) {
        float p0 = a.real.lo / b.real.lo;
        float p1 = a.real.lo / b.real.hi;
        float p2 = a.real.hi / b.real.lo;
        float p3 = a.real.hi / b.real.hi;
        float lo = ddgiFirstIntervalStepDown(ddgiFirstIntervalMin4(p0, p1, p2, p3), 6u);
        float hi = ddgiFirstIntervalStepUp(ddgiFirstIntervalMax4(p0, p1, p2, p3), 6u);
        ddgiFirstIntervalAssignCoreScalar(result.real, lo, hi, state, true, false);
    } else ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_DIVISION);
    return result;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalInvSqrt(
    DDGIFirstIntervalPairScalar value, inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    if (value.rte.valid != 0u && value.rte.lo > 0.0) {
        float lo = inversesqrt(value.rte.hi);
        float hi = inversesqrt(value.rte.lo);
        // The endpoint approximation may itself be two ULP from exact;
        // adding six steps is deliberately conservative for the legal RTE
        // result plus endpoint approximation and binade spacing.
        lo = ddgiFirstIntervalStepDown(lo, 6u);
        hi = ddgiFirstIntervalStepUp(hi, 6u);
        ddgiFirstIntervalAssignScalar(result.rte, lo, hi, state, false);
    } else ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_INVERSE_SQRT);
    if (value.real.valid != 0u && value.real.lo > 0.0) {
        float lo = inversesqrt(value.real.hi);
        float hi = inversesqrt(value.real.lo);
        // 4 steps cover the Vulkan 2-ULP inverse-sqrt error and one outward
        // f32 bracket step for the real endpoint evaluation.
        lo = ddgiFirstIntervalStepDown(lo, 4u);
        hi = ddgiFirstIntervalStepUp(hi, 4u);
        ddgiFirstIntervalAssignScalar(result.real, lo, hi, state, true);
    } else ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_INVERSE_SQRT);
    return result;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalSqrt(
    DDGIFirstIntervalPairScalar value, inout DDGIFirstIntervalState state) {
    if (value.rte.valid != 0u && value.rte.lo == 0.0 && value.rte.hi == 0.0 &&
        value.real.valid != 0u && value.real.lo == 0.0 && value.real.hi == 0.0)
        return value;
    if (value.rte.lo <= 0.0 || value.real.lo <= 0.0) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_THRESHOLD;
        ddgiFirstIntervalRteFailure(state, DDGI_FIRST_INTERVAL_REASON_INVERSE_SQRT);
        ddgiFirstIntervalRealFailure(state, DDGI_FIRST_INTERVAL_REASON_INVERSE_SQRT);
        return ddgiFirstIntervalInvalidPairScalar();
    }
    DDGIFirstIntervalPairScalar inv = ddgiFirstIntervalInvSqrt(value, state);
    return ddgiFirstIntervalMul(value, inv, state);
}

DDGIFirstIntervalPairVec3 ddgiFirstIntervalResidentVec3(
    vec3 value, inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairVec3 result = ddgiFirstIntervalInvalidPairVec();
    ddgiFirstIntervalAssignVec(result.rte, value, value, state, false);
    ddgiFirstIntervalAssignVec(result.real, value, value, state, true);
    return result;
}

DDGIFirstIntervalPairVec3 ddgiFirstIntervalAddVec(
    DDGIFirstIntervalPairVec3 a, DDGIFirstIntervalPairVec3 b,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairVec3 result;
    result.rte.valid = 1u;
    result.real.valid = 1u;
    result.rte.lo = vec3(0.0); result.rte.hi = vec3(0.0);
    result.real.lo = vec3(0.0); result.real.hi = vec3(0.0);
    for (uint i = 0u; i < 3u; ++i) {
        DDGIFirstIntervalPairScalar x;
        x.rte.lo = a.rte.lo[i]; x.rte.hi = a.rte.hi[i]; x.rte.valid = a.rte.valid;
        x.real.lo = a.real.lo[i]; x.real.hi = a.real.hi[i]; x.real.valid = a.real.valid;
        DDGIFirstIntervalPairScalar y;
        y.rte.lo = b.rte.lo[i]; y.rte.hi = b.rte.hi[i]; y.rte.valid = b.rte.valid;
        y.real.lo = b.real.lo[i]; y.real.hi = b.real.hi[i]; y.real.valid = b.real.valid;
        DDGIFirstIntervalPairScalar z = ddgiFirstIntervalAdd(x, y, state);
        result.rte.lo[i] = z.rte.lo; result.rte.hi[i] = z.rte.hi;
        result.real.lo[i] = z.real.lo; result.real.hi[i] = z.real.hi;
        result.rte.valid &= z.rte.valid; result.real.valid &= z.real.valid;
    }
    return result;
}

DDGIFirstIntervalPairVec3 ddgiFirstIntervalSubVec(
    DDGIFirstIntervalPairVec3 a, DDGIFirstIntervalPairVec3 b,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairVec3 result;
    result.rte.valid = 1u; result.real.valid = 1u;
    result.rte.lo = vec3(0.0); result.rte.hi = vec3(0.0);
    result.real.lo = vec3(0.0); result.real.hi = vec3(0.0);
    for (uint i = 0u; i < 3u; ++i) {
        DDGIFirstIntervalPairScalar x;
        x.rte.lo = a.rte.lo[i]; x.rte.hi = a.rte.hi[i]; x.rte.valid = a.rte.valid;
        x.real.lo = a.real.lo[i]; x.real.hi = a.real.hi[i]; x.real.valid = a.real.valid;
        DDGIFirstIntervalPairScalar y;
        y.rte.lo = b.rte.lo[i]; y.rte.hi = b.rte.hi[i]; y.rte.valid = b.rte.valid;
        y.real.lo = b.real.lo[i]; y.real.hi = b.real.hi[i]; y.real.valid = b.real.valid;
        DDGIFirstIntervalPairScalar z = ddgiFirstIntervalSub(x, y, state);
        result.rte.lo[i] = z.rte.lo; result.rte.hi[i] = z.rte.hi;
        result.real.lo[i] = z.real.lo; result.real.hi[i] = z.real.hi;
        result.rte.valid &= z.rte.valid; result.real.valid &= z.real.valid;
    }
    return result;
}

DDGIFirstIntervalPairVec3 ddgiFirstIntervalNegVec(
    DDGIFirstIntervalPairVec3 value, inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairVec3 result;
    result.rte.valid = 1u; result.real.valid = 1u;
    result.rte.lo = vec3(0.0); result.rte.hi = vec3(0.0);
    result.real.lo = vec3(0.0); result.real.hi = vec3(0.0);
    for (uint i = 0u; i < 3u; ++i) {
        DDGIFirstIntervalPairScalar x;
        x.rte.lo = value.rte.lo[i]; x.rte.hi = value.rte.hi[i]; x.rte.valid = value.rte.valid;
        x.real.lo = value.real.lo[i]; x.real.hi = value.real.hi[i]; x.real.valid = value.real.valid;
        DDGIFirstIntervalPairScalar y = ddgiFirstIntervalNeg(x, state);
        result.rte.lo[i] = y.rte.lo; result.rte.hi[i] = y.rte.hi;
        result.real.lo[i] = y.real.lo; result.real.hi[i] = y.real.hi;
        result.rte.valid &= y.rte.valid; result.real.valid &= y.real.valid;
    }
    return result;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalDot(
    DDGIFirstIntervalPairVec3 a, DDGIFirstIntervalPairVec3 b,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    DDGIFirstIntervalPairScalar x, y, z;
    x.rte.lo = a.rte.lo.x; x.rte.hi = a.rte.hi.x; x.rte.valid = a.rte.valid;
    x.real.lo = a.real.lo.x; x.real.hi = a.real.hi.x; x.real.valid = a.real.valid;
    y.rte.lo = b.rte.lo.x; y.rte.hi = b.rte.hi.x; y.rte.valid = b.rte.valid;
    y.real.lo = b.real.lo.x; y.real.hi = b.real.hi.x; y.real.valid = b.real.valid;
    result = ddgiFirstIntervalMul(x, y, state);
    for (uint i = 1u; i < 3u; ++i) {
        x.rte.lo = a.rte.lo[i]; x.rte.hi = a.rte.hi[i];
        x.real.lo = a.real.lo[i]; x.real.hi = a.real.hi[i];
        y.rte.lo = b.rte.lo[i]; y.rte.hi = b.rte.hi[i];
        y.real.lo = b.real.lo[i]; y.real.hi = b.real.hi[i];
        result = ddgiFirstIntervalAdd(result, ddgiFirstIntervalMul(x, y, state), state);
    }
    return result;
}

DDGIFirstIntervalPairVec3 ddgiFirstIntervalCross(
    DDGIFirstIntervalPairVec3 a, DDGIFirstIntervalPairVec3 b,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairVec3 result;
    result.rte.valid = 1u; result.real.valid = 1u;
    result.rte.lo = vec3(0.0); result.rte.hi = vec3(0.0);
    result.real.lo = vec3(0.0); result.real.hi = vec3(0.0);
    uint ia[3] = uint[3](1u, 2u, 0u);
    uint ib[3] = uint[3](2u, 0u, 1u);
    uint ja[3] = uint[3](2u, 0u, 1u);
    uint jb[3] = uint[3](1u, 2u, 0u);
    for (uint i = 0u; i < 3u; ++i) {
        DDGIFirstIntervalPairScalar ax, bx, ay, by;
        ax.rte.lo = a.rte.lo[ia[i]]; ax.rte.hi = a.rte.hi[ia[i]]; ax.rte.valid = a.rte.valid;
        ax.real.lo = a.real.lo[ia[i]]; ax.real.hi = a.real.hi[ia[i]]; ax.real.valid = a.real.valid;
        bx.rte.lo = b.rte.lo[ib[i]]; bx.rte.hi = b.rte.hi[ib[i]]; bx.rte.valid = b.rte.valid;
        bx.real.lo = b.real.lo[ib[i]]; bx.real.hi = b.real.hi[ib[i]]; bx.real.valid = b.real.valid;
        ay.rte.lo = a.rte.lo[ja[i]]; ay.rte.hi = a.rte.hi[ja[i]]; ay.rte.valid = a.rte.valid;
        ay.real.lo = a.real.lo[ja[i]]; ay.real.hi = a.real.hi[ja[i]]; ay.real.valid = a.real.valid;
        by.rte.lo = b.rte.lo[jb[i]]; by.rte.hi = b.rte.hi[jb[i]]; by.rte.valid = b.rte.valid;
        by.real.lo = b.real.lo[jb[i]]; by.real.hi = b.real.hi[jb[i]]; by.real.valid = b.real.valid;
        DDGIFirstIntervalPairScalar z = ddgiFirstIntervalSub(
            ddgiFirstIntervalMul(ax, bx, state),
            ddgiFirstIntervalMul(ay, by, state), state);
        result.rte.lo[i] = z.rte.lo; result.rte.hi[i] = z.rte.hi;
        result.real.lo[i] = z.real.lo; result.real.hi[i] = z.real.hi;
        result.rte.valid &= z.rte.valid; result.real.valid &= z.real.valid;
    }
    return result;
}

DDGIFirstIntervalPairVec3 ddgiFirstIntervalScale(
    DDGIFirstIntervalPairVec3 value, DDGIFirstIntervalPairScalar scalar,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairVec3 result;
    result.rte.valid = 1u; result.real.valid = 1u;
    result.rte.lo = vec3(0.0); result.rte.hi = vec3(0.0);
    result.real.lo = vec3(0.0); result.real.hi = vec3(0.0);
    for (uint i = 0u; i < 3u; ++i) {
        DDGIFirstIntervalPairScalar x;
        x.rte.lo = value.rte.lo[i]; x.rte.hi = value.rte.hi[i]; x.rte.valid = value.rte.valid;
        x.real.lo = value.real.lo[i]; x.real.hi = value.real.hi[i]; x.real.valid = value.real.valid;
        DDGIFirstIntervalPairScalar z = ddgiFirstIntervalMul(x, scalar, state);
        result.rte.lo[i] = z.rte.lo; result.rte.hi[i] = z.rte.hi;
        result.real.lo[i] = z.real.lo; result.real.hi[i] = z.real.hi;
        result.rte.valid &= z.rte.valid; result.real.valid &= z.real.valid;
    }
    return result;
}

DDGIFirstIntervalPairVec3 ddgiFirstIntervalWeighted3(
    DDGIFirstIntervalPairScalar w0, DDGIFirstIntervalPairScalar w1,
    DDGIFirstIntervalPairScalar w2, DDGIFirstIntervalPairVec3 a,
    DDGIFirstIntervalPairVec3 b, DDGIFirstIntervalPairVec3 c,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairVec3 result;
    result.rte.valid = 1u; result.real.valid = 1u;
    result.rte.lo = vec3(0.0); result.rte.hi = vec3(0.0);
    result.real.lo = vec3(0.0); result.real.hi = vec3(0.0);
    for (uint i = 0u; i < 3u; ++i) {
        DDGIFirstIntervalPairScalar xa, xb, xc;
        xa.rte.lo = a.rte.lo[i]; xa.rte.hi = a.rte.hi[i]; xa.rte.valid = a.rte.valid;
        xa.real.lo = a.real.lo[i]; xa.real.hi = a.real.hi[i]; xa.real.valid = a.real.valid;
        xb.rte.lo = b.rte.lo[i]; xb.rte.hi = b.rte.hi[i]; xb.rte.valid = b.rte.valid;
        xb.real.lo = b.real.lo[i]; xb.real.hi = b.real.hi[i]; xb.real.valid = b.real.valid;
        xc.rte.lo = c.rte.lo[i]; xc.rte.hi = c.rte.hi[i]; xc.rte.valid = c.rte.valid;
        xc.real.lo = c.real.lo[i]; xc.real.hi = c.real.hi[i]; xc.real.valid = c.real.valid;
        DDGIFirstIntervalPairScalar sum = ddgiFirstIntervalAdd(
            ddgiFirstIntervalAdd(ddgiFirstIntervalMul(w0, xa, state),
                                 ddgiFirstIntervalMul(w1, xb, state), state),
            ddgiFirstIntervalMul(w2, xc, state), state);
        result.rte.lo[i] = sum.rte.lo; result.rte.hi[i] = sum.rte.hi;
        result.real.lo[i] = sum.real.lo; result.real.hi[i] = sum.real.hi;
        result.rte.valid &= sum.rte.valid; result.real.valid &= sum.real.valid;
    }
    return result;
}

// Bound x_i/|x| with x_i shared by numerator and denominator.  For a
// positive endpoint, the lower extremum uses the largest other magnitudes;
// for a negative endpoint it uses the smallest.  The upper extremum reverses
// those choices.  Keep the original domain evaluator below authoritative:
// this optional intersection cannot revive any old UNKNOWN/threshold case.
DDGIFirstIntervalVec3 ddgiFirstIntervalNormalizeBox(
    DDGIFirstIntervalVec3 value, inout DDGIFirstIntervalState scratch) {
    DDGIFirstIntervalVec3 result = ddgiFirstIntervalInvalidVec();
    if (value.valid == 0u) return result;
    result.valid = 1u;
    for (uint axis = 0u; axis < 3u; ++axis) {
        if (value.lo[axis] == 0.0 && value.hi[axis] == 0.0) {
            result.lo[axis] = 0.0; result.hi[axis] = 0.0;
            continue;
        }
        float extremum[2];
        for (uint side = 0u; side < 2u; ++side) {
            float endpoint = side == 0u ? value.lo[axis] : value.hi[axis];
            DDGIFirstIntervalPairScalar x = ddgiFirstIntervalResidentScalar(endpoint, scratch);
            DDGIFirstIntervalPairScalar squared = ddgiFirstIntervalMul(x, x, scratch);
            bool use_max = side == 0u ? endpoint > 0.0 : endpoint < 0.0;
            for (uint other = 0u; other < 3u; ++other) {
                if (other == axis) continue;
                float smallest = value.lo[other] > 0.0 ? value.lo[other] :
                    (value.hi[other] < 0.0 ? -value.hi[other] : 0.0);
                float largest = max(abs(value.lo[other]), abs(value.hi[other]));
                float magnitude = use_max ? largest : smallest;
                DDGIFirstIntervalPairScalar y = ddgiFirstIntervalResidentScalar(magnitude, scratch);
                squared = ddgiFirstIntervalAdd(squared, ddgiFirstIntervalMul(y, y, scratch), scratch);
            }
            DDGIFirstIntervalPairScalar inv = ddgiFirstIntervalInvSqrt(squared, scratch);
            DDGIFirstIntervalPairScalar direction = ddgiFirstIntervalMul(x, inv, scratch);
            if (direction.real.valid == 0u) return ddgiFirstIntervalInvalidVec();
            // The real lane expands the approximate endpoint operations;
            // it therefore encloses the exact ideal extremum.
            extremum[side] = side == 0u ? direction.real.lo : direction.real.hi;
        }
        result.lo[axis] = extremum[0]; result.hi[axis] = extremum[1];
    }
    ddgiFirstIntervalAssignVec(result, result.lo, result.hi, scratch, true);
    return result;
}

DDGIFirstIntervalPairVec3 ddgiFirstIntervalNormalize(
    DDGIFirstIntervalPairVec3 value, inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar squared = ddgiFirstIntervalDot(value, value, state);
    DDGIFirstIntervalPairScalar length = ddgiFirstIntervalSqrt(squared, state);
    if ((length.rte.valid != 0u && length.rte.hi <= 1.0e-12) ||
        (length.real.valid != 0u && length.real.hi <= 1.0e-12)) {
        ddgiFirstIntervalBothFailure(state, DDGI_FIRST_INTERVAL_REASON_ZERO_LENGTH);
        return ddgiFirstIntervalInvalidPairVec();
    }
    if ((length.rte.valid != 0u && length.rte.lo <= 1.0e-12) ||
        (length.real.valid != 0u && length.real.lo <= 1.0e-12)) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_THRESHOLD;
    }
    DDGIFirstIntervalPairScalar inv = ddgiFirstIntervalInvSqrt(squared, state);
    DDGIFirstIntervalPairVec3 result = ddgiFirstIntervalScale(value, inv, state);
    // Preserve every legacy operation/domain/length guard before narrowing.
    if (state.rte_domain_valid == 0u || state.real_domain_valid == 0u ||
        state.reason_bits != 0u || result.rte.valid == 0u || result.real.valid == 0u)
        return result;
    DDGIFirstIntervalState scratch = state;
    DDGIFirstIntervalVec3 ideal_rte_box = ddgiFirstIntervalNormalizeBox(value.rte, scratch);
    DDGIFirstIntervalVec3 ideal_real_box = ddgiFirstIntervalNormalizeBox(value.real, scratch);
    if (scratch.rte_domain_valid == 0u || scratch.real_domain_valid == 0u ||
        scratch.reason_bits != 0u || ideal_rte_box.valid == 0u || ideal_real_box.valid == 0u)
        return result;
    // u=2^-24. Three products/two left-associated adds of nonnegative
    // squares give |dot/sum(x*x)-1| <= gamma_3=3u/(1-3u).
    // Two full ULP inverse-sqrt allowance is <=4u relative; final RTE
    // multiply contributes u. The common length scale is bounded by
    // (1 +/- 4u)*(1 +/- u)/sqrt(1 +/- gamma_3), strictly inside
    // 1 +/- 2^-20. This is an arithmetic enclosure, not a gate tolerance.
    // Legacy nonzero/normal/overflow guards above justify these relative
    // error bounds; no flushed intermediate is accepted by this shortcut.
    DDGIFirstIntervalPairScalar scale;
    scale.rte.lo = 0.99999904632568359375; scale.rte.hi = 1.00000095367431640625;
    scale.real.lo = scale.rte.lo; scale.real.hi = scale.rte.hi;
    scale.rte.valid = 1u; scale.real.valid = 1u;
    DDGIFirstIntervalPairVec3 ideal_pair;
    ideal_pair.rte = ideal_rte_box; ideal_pair.real = ideal_rte_box;
    DDGIFirstIntervalPairVec3 expanded = ddgiFirstIntervalScale(ideal_pair, scale, scratch);
    if (scratch.rte_domain_valid == 0u || scratch.real_domain_valid == 0u ||
        scratch.reason_bits != 0u || expanded.real.valid == 0u) return result;
    vec3 rte_lo = max(result.rte.lo, expanded.real.lo);
    vec3 rte_hi = min(result.rte.hi, expanded.real.hi);
    vec3 real_lo = max(result.real.lo, ideal_real_box.lo);
    vec3 real_hi = min(result.real.hi, ideal_real_box.hi);
    ddgiFirstIntervalAssignVec(result.rte, rte_lo, rte_hi, state, false);
    ddgiFirstIntervalAssignVec(result.real, real_lo, real_hi, state, true);
    state.operation_count = scratch.operation_count;
    return result;
}

void ddgiFirstIntervalRecordBranch(inout DDGIFirstIntervalState state,
                                   bool stable, uint branch_bit) {
    state.branch_bits |= branch_bit;
    if (!stable) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_THRESHOLD;
        state.rte_domain_valid = 0u;
        state.real_domain_valid = 0u;
    }
}

bool ddgiFirstIntervalStableGreater(DDGIFirstIntervalPairScalar value,
                                    float threshold) {
    return value.rte.valid != 0u && value.real.valid != 0u &&
           value.rte.lo > threshold && value.real.lo > threshold;
}

bool ddgiFirstIntervalStableLess(DDGIFirstIntervalPairScalar value,
                                 float threshold) {
    return value.rte.valid != 0u && value.real.valid != 0u &&
           value.rte.hi < threshold && value.real.hi < threshold;
}

bool ddgiFirstIntervalStableSign(DDGIFirstIntervalPairScalar value, bool positive) {
    if (value.rte.valid == 0u || value.real.valid == 0u) return false;
    if (positive) return value.rte.lo > 0.0 && value.real.lo > 0.0;
    return value.rte.hi < 0.0 && value.real.hi < 0.0;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalComponent(
    DDGIFirstIntervalPairVec3 value, uint component) {
    DDGIFirstIntervalPairScalar result;
    result.rte.lo = value.rte.lo[component]; result.rte.hi = value.rte.hi[component];
    result.rte.valid = value.rte.valid;
    result.real.lo = value.real.lo[component]; result.real.hi = value.real.hi[component];
    result.real.valid = value.real.valid;
    return result;
}

DDGIFirstIntervalPairVec3 ddgiFirstIntervalSourceUnpack(
    uint packed, uint color_sign, inout DDGIFirstIntervalState state,
    out uint path) {
    DDGIFirstIntervalPairVec3 result = ddgiFirstIntervalInvalidPairVec();
    uint hiBits = (packed >> 16u) & 0xffffu;
    uint loBits = packed & 0xffffu;
    bool exactAxis = (((hiBits == 0u || hiBits == 65535u) &&
                       (loBits == 32767u || loBits == 32768u)) ||
                      ((loBits == 0u || loBits == 65535u) &&
                       (hiBits == 32767u || hiBits == 32768u)));
    DDGIFirstIntervalPairScalar hi = ddgiFirstIntervalExactConstant(float(hiBits), state);
    DDGIFirstIntervalPairScalar lo = ddgiFirstIntervalExactConstant(float(loBits), state);
    DDGIFirstIntervalPairScalar inv = ddgiFirstIntervalPackedInverse(state);
    DDGIFirstIntervalPairScalar fx = ddgiFirstIntervalSub(
        ddgiFirstIntervalMul(hi, inv, state),
        ddgiFirstIntervalExactConstant(1.0, state), state);
    DDGIFirstIntervalPairScalar fy = ddgiFirstIntervalSub(
        ddgiFirstIntervalMul(lo, inv, state),
        ddgiFirstIntervalExactConstant(1.0, state), state);
    DDGIFirstIntervalPairVec3 xy;
    xy.rte.lo = vec3(fx.rte.lo, fy.rte.lo, 0.0);
    xy.rte.hi = vec3(fx.rte.hi, fy.rte.hi, 0.0);
    xy.rte.valid = fx.rte.valid & fy.rte.valid;
    if (exactAxis) {
        // The encoded axis component is an exact integer identity in the
        // conceptual source path.  Keep that identity instead of letting the
        // rounded 2/65535 resident value turn d=1+1/65535^2 into a boundary
        // crossing.  The off-axis component remains a tight adjacent-f32
        // bracket around the exact rational 1/65535.
        bool xAxis = (hiBits == 0u || hiBits == 65535u);
        float axisValue = (xAxis ? hiBits : loBits) == 0u ? -1.0 : 1.0;
        float small = DDGI_FIRST_INTERVAL_INV_F32 * 0.5;
        float smallLo = ddgiFirstIntervalNextDown(small);
        float smallHi = ddgiFirstIntervalNextUp(small);
        DDGIFirstIntervalPairScalar axis = ddgiFirstIntervalResidentScalar(axisValue, state);
        DDGIFirstIntervalPairScalar minor = ddgiFirstIntervalInvalidPairScalar();
        bool minorNegative = (xAxis ? loBits : hiBits) == 32767u;
        float signedLo = minorNegative ? -smallHi : smallLo;
        float signedHi = minorNegative ? -smallLo : smallHi;
        ddgiFirstIntervalAssignScalar(minor.real, signedLo, signedHi, state, true);
        if (xAxis) {
            fx.real = axis.real;
            fy.real = minor.real;
        } else {
            fx.real = minor.real;
            fy.real = axis.real;
        }
    }
    // Snapshot the conceptual source lane after the axis identities above.
    // Its dot product and returned components must consume the same values.
    xy.real.lo = vec3(fx.real.lo, fy.real.lo, 0.0);
    xy.real.hi = vec3(fx.real.hi, fy.real.hi, 0.0);
    xy.real.valid = fx.real.valid & fy.real.valid;
    DDGIFirstIntervalPairScalar d = ddgiFirstIntervalDot(xy, xy, state);
    DDGIFirstIntervalPairScalar gammaNNumerator = ddgiFirstIntervalMul(
        ddgiFirstIntervalExactConstant(8.0, state),
        ddgiFirstIntervalExactConstant(DDGI_FIRST_INTERVAL_EPS, state), state);
    DDGIFirstIntervalPairScalar gammaDenominator = ddgiFirstIntervalSub(
        ddgiFirstIntervalExactConstant(1.0, state), gammaNNumerator, state);
    bool gammaDenominatorStable = ddgiFirstIntervalStableGreater(gammaDenominator, 0.5);
    if (!gammaDenominatorStable) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_THRESHOLD;
        state.rte_domain_valid = 0u;
        state.real_domain_valid = 0u;
        gammaDenominator = ddgiFirstIntervalExactConstant(0.5, state);
    }
    DDGIFirstIntervalPairScalar gammaN = ddgiFirstIntervalDiv(
        gammaNNumerator, gammaDenominator, state);
    // The existing private decoder's d_error is retained as a branch guard.
    // The endpoint lanes still carry the full d interval rather than this
    // scalar shortcut.
    float maxAbsD = max(max(max(abs(d.rte.lo), abs(d.rte.hi)),
                             max(abs(d.real.lo), abs(d.real.hi))), 1.0);
    DDGIFirstIntervalPairScalar dError = ddgiFirstIntervalAdd(
        ddgiFirstIntervalMul(gammaN,
                             ddgiFirstIntervalExactConstant(maxAbsD, state), state),
        ddgiFirstIntervalExactConstant(4.0 * DDGI_FIRST_INTERVAL_EPS, state), state);
    DDGIFirstIntervalPairScalar upper = ddgiFirstIntervalAdd(
        ddgiFirstIntervalExactConstant(1.0, state), dError, state);
    DDGIFirstIntervalPairScalar lower = ddgiFirstIntervalSub(
        ddgiFirstIntervalExactConstant(1.0, state), dError, state);
    bool fallback = d.rte.valid != 0u && d.real.valid != 0u &&
                    d.rte.lo > upper.rte.hi && d.real.lo > upper.real.hi;
    bool regular = d.rte.valid != 0u && d.real.valid != 0u &&
                   d.rte.hi < lower.rte.lo && d.real.hi < lower.real.lo;
    if (fallback) {
        path = 2u; state.branch_bits |= DDGI_FIRST_INTERVAL_BRANCH_PACKED_FALLBACK;
        return ddgiFirstIntervalResidentVec3(vec3(0.0, 0.0, 1.0), state);
    }
    if (regular) {
        path = 1u; state.branch_bits |= DDGI_FIRST_INTERVAL_BRANCH_PACKED_REGULAR;
    } else if (exactAxis) {
        path = 3u; state.branch_bits |= DDGI_FIRST_INTERVAL_BRANCH_PACKED_EXACT_AXIS;
    } else {
        path = 0u;
        ddgiFirstIntervalBothFailure(state, DDGI_FIRST_INTERVAL_REASON_THRESHOLD);
        return result;
    }
    DDGIFirstIntervalPairScalar z2 = ddgiFirstIntervalMaxZero(
        ddgiFirstIntervalSub(ddgiFirstIntervalExactConstant(1.0, state), d, state), state);
    DDGIFirstIntervalPairScalar z = ddgiFirstIntervalSqrt(z2, state);
    float sign = (((color_sign >> 24u) & 0xffu) & 0x02u) != 0u ? 1.0 : -1.0;
    DDGIFirstIntervalPairScalar signedZ = ddgiFirstIntervalMul(
        z, ddgiFirstIntervalExactConstant(sign, state), state);
    result.rte.lo = vec3(fx.rte.lo, fy.rte.lo, signedZ.rte.lo);
    result.rte.hi = vec3(fx.rte.hi, fy.rte.hi, signedZ.rte.hi);
    result.rte.valid = fx.rte.valid & fy.rte.valid & signedZ.rte.valid;
    result.real.lo = vec3(fx.real.lo, fy.real.lo, signedZ.real.lo);
    result.real.hi = vec3(fx.real.hi, fy.real.hi, signedZ.real.hi);
    result.real.valid = fx.real.valid & fy.real.valid & signedZ.real.valid;
    return result;
}

DDGIFirstIntervalFrame ddgiFirstIntervalWorldFrame(
    DDGIFirstIntervalPairVec3 object_n, DDGIFirstIntervalPairVec3 object_t,
    inout DDGIFirstIntervalState state) {
    // Current native helper accepts only exact identity matrices.  Keeping
    // this private variant identity-only avoids silently modelling a matrix
    // FMA path that the actual source guard rejects.
    DDGIFirstIntervalFrame frame;
    DDGIFirstIntervalPairVec3 world_n = ddgiFirstIntervalNormalize(object_n, state);
    DDGIFirstIntervalPairVec3 world_t = ddgiFirstIntervalNormalize(object_t, state);
    DDGIFirstIntervalPairScalar d = ddgiFirstIntervalDot(world_t, world_n, state);
    DDGIFirstIntervalPairScalar absD = ddgiFirstIntervalAbs(d, state);
    bool fallback = ddgiFirstIntervalStableGreater(absD, 0.999);
    bool projection = ddgiFirstIntervalStableLess(absD, 0.999);
    ddgiFirstIntervalRecordBranch(state, fallback || projection, fallback ? DDGI_FIRST_INTERVAL_BRANCH_FALLBACK_UP : DDGI_FIRST_INTERVAL_BRANCH_GRAM);
    bool useFallback = abs(float((absD.rte.lo + absD.rte.hi) * 0.5)) > 0.999;
    if (useFallback) {
        DDGIFirstIntervalPairScalar y = ddgiFirstIntervalComponent(world_n, 1u);
        DDGIFirstIntervalPairScalar ay = ddgiFirstIntervalAbs(y, state);
        bool useX = ddgiFirstIntervalStableLess(ay, 0.999);
        ddgiFirstIntervalRecordBranch(state, useX, useX ? DDGI_FIRST_INTERVAL_BRANCH_PRIMARY_UP_X : DDGI_FIRST_INTERVAL_BRANCH_PRIMARY_UP_Y);
        DDGIFirstIntervalPairVec3 up = ddgiFirstIntervalResidentVec3(
            useX ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0), state);
        world_t = ddgiFirstIntervalNormalize(ddgiFirstIntervalCross(up, world_n, state), state);
    } else {
        world_t = ddgiFirstIntervalNormalize(
            ddgiFirstIntervalSubVec(world_t, ddgiFirstIntervalScale(world_n, d, state), state), state);
    }
    frame.normal = world_n;
    frame.tangent = world_t;
    return frame;
}

DDGIFirstIntervalPairScalar ddgiFirstIntervalGapComponent(
    DDGIFirstIntervalPairVec3 value, uint component,
    inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar result = ddgiFirstIntervalInvalidPairScalar();
    if (value.rte.valid == 0u || value.real.valid == 0u) {
        ddgiFirstIntervalBothFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
        return result;
    }
    // Every source endpoint is resident f32.  The subtraction itself is a
    // strict f32 operation; one adjacent value covers its correctly-rounded
    // result before max/abs.  The zero case is an exact endpoint identity and
    // is safe to retain as a metric component.
    float r0 = abs(value.rte.lo[component] - value.real.hi[component]);
    float r1 = abs(value.rte.hi[component] - value.real.lo[component]);
    float x0 = max(r0, r1);
    float q0 = x0 == 0.0 ? 0.0 : ddgiFirstIntervalStepUp(x0, 1u);
    float s0 = abs(value.real.lo[component] - value.rte.hi[component]);
    float s1 = abs(value.real.hi[component] - value.rte.lo[component]);
    float x1 = max(s0, s1);
    float q1 = x1 == 0.0 ? 0.0 : ddgiFirstIntervalStepUp(x1, 1u);
    if (!ddgiFirstFinite(q0) || !ddgiFirstFinite(q1)) {
        ddgiFirstIntervalBothFailure(state, DDGI_FIRST_INTERVAL_REASON_NONFINITE);
        return result;
    }
    if (q0 != 0.0 && abs(q0) < DDGI_FIRST_INTERVAL_F32_MIN_SAFE) {
        ddgiFirstIntervalBothFailure(state, DDGI_FIRST_INTERVAL_REASON_SUBNORMAL);
        return result;
    }
    if (q1 != 0.0 && abs(q1) < DDGI_FIRST_INTERVAL_F32_MIN_SAFE) {
        ddgiFirstIntervalBothFailure(state, DDGI_FIRST_INTERVAL_REASON_SUBNORMAL);
        return result;
    }
    ddgiFirstIntervalAssignScalar(result.rte, q0, q0, state, false);
    ddgiFirstIntervalAssignScalar(result.real, q1, q1, state, true);
    return result;
}

float ddgiFirstIntervalL2Gap(DDGIFirstIntervalPairVec3 value,
                             inout DDGIFirstIntervalState state) {
    DDGIFirstIntervalPairScalar g0 = ddgiFirstIntervalGapComponent(value, 0u, state);
    DDGIFirstIntervalPairScalar g1 = ddgiFirstIntervalGapComponent(value, 1u, state);
    DDGIFirstIntervalPairScalar g2 = ddgiFirstIntervalGapComponent(value, 2u, state);
    if (g0.rte.valid == 0u || g1.rte.valid == 0u || g2.rte.valid == 0u ||
        g0.real.valid == 0u || g1.real.valid == 0u || g2.real.valid == 0u)
        return DDGI_FIRST_INTERVAL_F32_MAX;
    // Evaluate the squared sum through the same pair operations as the rest
    // of the chain.  This records each add/mul domain and lets the final
    // inverse-sqrt allowance be checked independently for both lanes.
    DDGIFirstIntervalPairScalar sq = ddgiFirstIntervalAdd(
        ddgiFirstIntervalAdd(ddgiFirstIntervalMul(g0, g0, state),
                             ddgiFirstIntervalMul(g1, g1, state), state),
        ddgiFirstIntervalMul(g2, g2, state), state);
    DDGIFirstIntervalPairScalar length = ddgiFirstIntervalSqrt(sq, state);
    if (length.rte.valid == 0u || length.real.valid == 0u)
        return DDGI_FIRST_INTERVAL_F32_MAX;
    return max(length.rte.hi, length.real.hi);
}

void ddgiFirstIntervalInitResult(out DDGIFirstRteIntervalResult result) {
    result.status = DDGI_FIRST_INTERVAL_STATUS_UNKNOWN;
    result.reason_bits = DDGI_FIRST_INTERVAL_REASON_EARLY_PAYLOAD;
    result.branch_bits = 0u;
    result.operation_count = 0u;
    result.rte_domain_valid = 0u;
    result.real_domain_valid = 0u;
    result.math_gate_pass = 0u;
    result.bary_l2 = DDGI_FIRST_INTERVAL_F32_MAX;
    result.tangent_l2 = DDGI_FIRST_INTERVAL_F32_MAX;
    result.ray_l2 = DDGI_FIRST_INTERVAL_F32_MAX;
    result.geometry_l2 = DDGI_FIRST_INTERVAL_F32_MAX;
    result.bitangent_l2 = DDGI_FIRST_INTERVAL_F32_MAX;
    result.mapped_l2 = DDGI_FIRST_INTERVAL_F32_MAX;
    result.max_l2 = DDGI_FIRST_INTERVAL_F32_MAX;
    result.bary_native_n = ddgiFirstIntervalInvalidPairVec();
    result.tangent = ddgiFirstIntervalInvalidPairVec();
    result.ray_native_n = ddgiFirstIntervalInvalidPairVec();
    result.geometry_n = ddgiFirstIntervalInvalidPairVec();
    result.bitangent = ddgiFirstIntervalInvalidPairVec();
    result.mapped_n = ddgiFirstIntervalInvalidPairVec();
}

void ddgiFirstIntervalWriteVec(uint base, DDGIFirstIntervalVec3 value) {
    first_native_audit.audit_records[base + 0u] = vec4(value.lo, 0.0);
    first_native_audit.audit_records[base + 1u] = vec4(value.hi, 0.0);
}

void ddgiFirstIntervalWritePair(uint rte_base, uint real_base,
                                DDGIFirstIntervalPairVec3 value) {
    ddgiFirstIntervalWriteVec(rte_base, value.rte);
    ddgiFirstIntervalWriteVec(real_base, value.real);
}

void ddgiFirstIntervalWriteTail(uint base, DDGIFirstRteIntervalResult result) {
    first_native_audit.audit_records[base + 0u] = vec4(
        uintBitsToFloat(result.status), uintBitsToFloat(result.reason_bits),
        uintBitsToFloat(result.branch_bits), uintBitsToFloat(DDGI_FIRST_INTERVAL_SCHEMA));
    first_native_audit.audit_records[base + 1u] = vec4(
        result.bary_l2, result.tangent_l2, result.geometry_l2, result.mapped_l2);
    first_native_audit.audit_records[base + 2u] = vec4(
        result.ray_l2, result.bitangent_l2, result.max_l2,
        float(result.math_gate_pass));
    first_native_audit.audit_records[base + 3u] = vec4(
        uintBitsToFloat(result.operation_count),
        uintBitsToFloat(result.rte_domain_valid),
        uintBitsToFloat(result.real_domain_valid), 0.0);
    // ABI keeps every RTE vector contiguous before every real-source vector:
    // six values x (lo, hi) at tail4..15, then the same six at tail16..27.
    ddgiFirstIntervalWritePair(base + 4u,  base + 16u, result.bary_native_n);
    ddgiFirstIntervalWritePair(base + 6u,  base + 18u, result.tangent);
    ddgiFirstIntervalWritePair(base + 8u,  base + 20u, result.ray_native_n);
    ddgiFirstIntervalWritePair(base + 10u, base + 22u, result.geometry_n);
    ddgiFirstIntervalWritePair(base + 12u, base + 24u, result.bitangent);
    ddgiFirstIntervalWritePair(base + 14u, base + 26u, result.mapped_n);
}

void ddgiFirstIntervalClearTail(uint base) {
    for (uint i = 0u; i < DDGI_FIRST_INTERVAL_TAIL_VEC4; ++i)
        first_native_audit.audit_records[base + i] = vec4(DDGI_FIRST_INTERVAL_F32_MAX);
    first_native_audit.audit_records[base + 0u] = vec4(
        uintBitsToFloat(DDGI_FIRST_INTERVAL_STATUS_UNKNOWN), 0.0, 0.0,
        uintBitsToFloat(DDGI_FIRST_INTERVAL_SCHEMA));
    first_native_audit.audit_records[base + 3u] = vec4(
        uintBitsToFloat(0u), uintBitsToFloat(0u), uintBitsToFloat(0u), 0.0);
}

// Replays the private normal chain from first/source buffers.  The existing
// native result is used only for path/texture evidence and status context;
// no result normal or bound is consumed as an interval input.
DDGIFirstRteIntervalResult ddgiFirstRteEvaluate(
    DDGIFirstSurfaceHit first, vec3 ray_direction,
    DDGIFirstNativeNormalResult native_result) {
    DDGIFirstRteIntervalResult result;
    ddgiFirstIntervalInitResult(result);
    DDGIFirstIntervalState state;
    state.reason_bits = 0u;
    state.branch_bits = 0u;
    state.operation_count = 0u;
    state.rte_domain_valid = 1u;
    state.real_domain_valid = 1u;

    if (native_result.old_hn_valid > 0.5 || first.status != 1u ||
        first.flags != 0u || !ddgiFirstFinite(first.t) || first.t <= 2.0e-5) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_EARLY_PAYLOAD;
        result.reason_bits = state.reason_bits;
        return result;
    }
    if (!ddgiFirstFinite(first.triangle) || first.triangle < 0.0 ||
        first.triangle != floor(first.triangle) ||
        !ddgiFirstFiniteVec3(first.bary)) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_SOURCE;
        result.reason_bits = state.reason_bits;
        return result;
    }
    uint tri_id = uint(first.triangle);
    uint mappingBase = tri_id * 2u;
    if (mappingBase + 1u >= first_native_triangle_source.triangle_source.length()) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_SOURCE;
        result.reason_bits = state.reason_bits;
        return result;
    }
    uvec4 source = first_native_triangle_source.triangle_source[mappingBase];
    uvec4 mapping_meta = first_native_triangle_source.triangle_source[mappingBase + 1u];
    uint pos_count = first_native_positions.positions.length() / 3u;
    uint element_count = first_native_elements.elements.length() / 6u;
    uint instance_count = first_native_instances.instances.length();
    if (any(greaterThanEqual(source.xyz, uvec3(pos_count))) ||
        any(greaterThanEqual(source.xyz, uvec3(element_count))) ||
        source.w >= instance_count ||
        !ddgiFirstNativeIdentity(first_native_instances.instances[source.w].world_matrix)) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_SOURCE | DDGI_FIRST_INTERVAL_REASON_WORLD_MATRIX;
        result.reason_bits = state.reason_bits;
        return result;
    }
    DDGIFirstNativeInstance instance = first_native_instances.instances[source.w];
    if (!ddgiFirstFinite(first.source_mesh) || first.source_mesh < 0.0 ||
        first.source_mesh != floor(first.source_mesh) ||
        mapping_meta.y != uint(first.source_mesh)) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_SOURCE;
        result.reason_bits = state.reason_bits;
        return result;
    }
    uint c0, c1, c2, pn0, pn1, pn2, pt0, pt1, pt2;
    vec2 uv0, uv1, uv2;
    if (!ddgiFirstNativeLoadElement(source.x, c0, pn0, pt0, uv0) ||
        !ddgiFirstNativeLoadElement(source.y, c1, pn1, pt1, uv1) ||
        !ddgiFirstNativeLoadElement(source.z, c2, pn2, pt2, uv2)) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_SOURCE;
        result.reason_bits = state.reason_bits;
        return result;
    }
    uint material_count = first_native_materials.material_words.length() / 12u;
    uint material_id = instance.meta0.y;
    if (mapping_meta.x != material_id || mapping_meta.z != material_id ||
        mapping_meta.w != 0u || material_id >= material_count) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_MATERIAL;
        result.reason_bits = state.reason_bits;
        return result;
    }
    uint material_base = material_id * 12u;
    uint material_flags = first_native_materials.material_words[material_base + 11u];
    uint normal_layer = first_native_materials.material_words[material_base + 1u];
    vec2 uv_scale = vec2(uintBitsToFloat(first_native_materials.material_words[material_base + 9u]),
                         uintBitsToFloat(first_native_materials.material_words[material_base + 10u]));
    if (material_flags != 0u || !ddgiFirstNativeFinite4(vec4(uv_scale, 0.0, 0.0))) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_MATERIAL;
        result.reason_bits = state.reason_bits;
        return result;
    }

    uint nPath0, nPath1, nPath2, tPath0, tPath1, tPath2;
    DDGIFirstIntervalPairVec3 on0 = ddgiFirstIntervalSourceUnpack(pn0, c0, state, nPath0);
    DDGIFirstIntervalPairVec3 on1 = ddgiFirstIntervalSourceUnpack(pn1, c1, state, nPath1);
    DDGIFirstIntervalPairVec3 on2 = ddgiFirstIntervalSourceUnpack(pn2, c2, state, nPath2);
    DDGIFirstIntervalPairVec3 ot0 = ddgiFirstIntervalSourceUnpack(pt0, c0, state, tPath0);
    DDGIFirstIntervalPairVec3 ot1 = ddgiFirstIntervalSourceUnpack(pt1, c1, state, tPath1);
    DDGIFirstIntervalPairVec3 ot2 = ddgiFirstIntervalSourceUnpack(pt2, c2, state, tPath2);
    if (state.rte_domain_valid == 0u || state.real_domain_valid == 0u) {
        result.reason_bits = state.reason_bits;
        return result;
    }

    DDGIFirstIntervalFrame frame0 = ddgiFirstIntervalWorldFrame(on0, ot0, state);
    DDGIFirstIntervalFrame frame1 = ddgiFirstIntervalWorldFrame(on1, ot1, state);
    DDGIFirstIntervalFrame frame2 = ddgiFirstIntervalWorldFrame(on2, ot2, state);
    DDGIFirstIntervalPairScalar w0 = ddgiFirstIntervalResidentScalar(first.bary.x, state);
    DDGIFirstIntervalPairScalar w1 = ddgiFirstIntervalResidentScalar(first.bary.y, state);
    DDGIFirstIntervalPairScalar w2 = ddgiFirstIntervalResidentScalar(first.bary.z, state);
    DDGIFirstIntervalPairVec3 native_raw = ddgiFirstIntervalWeighted3(
        w0, w1, w2, frame0.normal, frame1.normal, frame2.normal, state);
    DDGIFirstIntervalPairVec3 tangent_raw = ddgiFirstIntervalWeighted3(
        w0, w1, w2, frame0.tangent, frame1.tangent, frame2.tangent, state);
    result.bary_native_n = ddgiFirstIntervalNormalize(native_raw, state);
    result.tangent = ddgiFirstIntervalNormalize(tangent_raw, state);

    DDGIFirstIntervalPairVec3 p0 = ddgiFirstIntervalResidentVec3(
        vec3(first_native_positions.positions[source.x * 3u + 0u],
             first_native_positions.positions[source.x * 3u + 1u],
             first_native_positions.positions[source.x * 3u + 2u]), state);
    DDGIFirstIntervalPairVec3 p1 = ddgiFirstIntervalResidentVec3(
        vec3(first_native_positions.positions[source.y * 3u + 0u],
             first_native_positions.positions[source.y * 3u + 1u],
             first_native_positions.positions[source.y * 3u + 2u]), state);
    DDGIFirstIntervalPairVec3 p2 = ddgiFirstIntervalResidentVec3(
        vec3(first_native_positions.positions[source.z * 3u + 0u],
             first_native_positions.positions[source.z * 3u + 1u],
             first_native_positions.positions[source.z * 3u + 2u]), state);
    DDGIFirstIntervalPairVec3 edge1 = ddgiFirstIntervalSubVec(p1, p0, state);
    DDGIFirstIntervalPairVec3 edge2 = ddgiFirstIntervalSubVec(p2, p0, state);
    DDGIFirstIntervalPairVec3 geom_raw = ddgiFirstIntervalCross(edge1, edge2, state);
    result.geometry_n = ddgiFirstIntervalNormalize(geom_raw, state);
    DDGIFirstIntervalPairVec3 direction = ddgiFirstIntervalResidentVec3(ray_direction, state);
    DDGIFirstIntervalPairScalar geom_dot = ddgiFirstIntervalDot(result.geometry_n, direction, state);
    DDGIFirstIntervalPairScalar native_dot = ddgiFirstIntervalDot(result.bary_native_n, direction, state);
    bool nativePositive = ddgiFirstIntervalStableSign(native_dot, true);
    bool nativeNegative = ddgiFirstIntervalStableSign(native_dot, false);
    bool geomPositive = ddgiFirstIntervalStableSign(geom_dot, true);
    bool geomNegative = ddgiFirstIntervalStableSign(geom_dot, false);
    bool signsStable = (nativePositive || nativeNegative) && (geomPositive || geomNegative);
    if (!signsStable) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_THRESHOLD;
        state.rte_domain_valid = 0u;
        state.real_domain_valid = 0u;
    }
    DDGIFirstIntervalPairVec3 ray_native = (nativePositive || nativeNegative) && nativePositive
        ? ddgiFirstIntervalNegVec(result.bary_native_n, state) : result.bary_native_n;
    DDGIFirstIntervalPairVec3 ray_geom = (geomPositive || geomNegative) && geomPositive
        ? ddgiFirstIntervalNegVec(result.geometry_n, state) : result.geometry_n;
    state.branch_bits |= nativePositive ? DDGI_FIRST_INTERVAL_BRANCH_NATIVE_FLIP : 0u;
    state.branch_bits |= geomPositive ? DDGI_FIRST_INTERVAL_BRANCH_GEOM_FLIP : 0u;
    DDGIFirstIntervalPairScalar align = ddgiFirstIntervalDot(ray_native, ray_geom, state);
    if (!ddgiFirstIntervalStableSign(align, true) && !ddgiFirstIntervalStableSign(align, false)) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_THRESHOLD;
        state.rte_domain_valid = 0u;
        state.real_domain_valid = 0u;
    }
    if (ddgiFirstIntervalStableSign(align, false)) {
        state.branch_bits |= DDGI_FIRST_INTERVAL_BRANCH_ALIGN_FLIP;
        ray_native = ddgiFirstIntervalNegVec(ray_native, state);
    }
    DDGIFirstIntervalPairVec3 bitangent_raw = ddgiFirstIntervalCross(ray_native, result.tangent, state);
    result.bitangent = ddgiFirstIntervalNormalize(bitangent_raw, state);
    result.ray_native_n = ray_native;

    DDGIFirstIntervalPairScalar b0 = ddgiFirstIntervalResidentScalar(first.bary.x, state);
    DDGIFirstIntervalPairScalar b1 = ddgiFirstIntervalResidentScalar(first.bary.y, state);
    DDGIFirstIntervalPairScalar b2 = ddgiFirstIntervalResidentScalar(first.bary.z, state);
    DDGIFirstIntervalPairVec3 uv;
    uv = ddgiFirstIntervalWeighted3(b0, b1, b2,
                                    ddgiFirstIntervalResidentVec3(vec3(uv0, 0.0), state),
                                    ddgiFirstIntervalResidentVec3(vec3(uv1, 0.0), state),
                                    ddgiFirstIntervalResidentVec3(vec3(uv2, 0.0), state), state);
    // The proof host independently certifies UV/quad identity.  Keep this
    // interval path on the private native sample already emitted for this ray,
    // and require the old helper to have a valid same-frame path/texture.
    if (native_result.path_code != 1u && native_result.path_code != 2u ||
        native_result.normal_layer != normal_layer ||
        !ddgiFirstNativeFinite4(native_result.normal_sample)) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_TEXTURE;
        result.reason_bits = state.reason_bits;
        return result;
    }
    DDGIFirstIntervalPairVec3 sample_vec = ddgiFirstIntervalResidentVec3(native_result.normal_sample.xyz, state);
    DDGIFirstIntervalPairScalar sample_len = ddgiFirstIntervalSqrt(
        ddgiFirstIntervalDot(sample_vec, sample_vec, state), state);
    bool path2 = ddgiFirstIntervalStableGreater(sample_len, 0.1);
    bool path1 = ddgiFirstIntervalStableLess(sample_len, 0.1);
    if (!path1 && !path2) {
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_THRESHOLD | DDGI_FIRST_INTERVAL_REASON_TEXTURE;
        state.rte_domain_valid = 0u;
        state.real_domain_valid = 0u;
    } else {
        state.branch_bits |= path2 ? DDGI_FIRST_INTERVAL_BRANCH_SAMPLE_PATH2 : DDGI_FIRST_INTERVAL_BRANCH_SAMPLE_PATH1;
    }
    DDGIFirstIntervalPairVec3 mapped = ray_native;
    if (path2) {
        DDGIFirstIntervalPairVec3 tangent_normal = ddgiFirstIntervalSubVec(
            ddgiFirstIntervalScale(sample_vec, ddgiFirstIntervalExactConstant(2.0, state), state),
            ddgiFirstIntervalResidentVec3(vec3(1.0), state), state);
        mapped = ddgiFirstIntervalNormalize(
            ddgiFirstIntervalAddVec(
                ddgiFirstIntervalAddVec(
                    ddgiFirstIntervalScale(result.tangent,
                        ddgiFirstIntervalComponent(tangent_normal, 0u), state),
                    ddgiFirstIntervalScale(result.bitangent,
                        ddgiFirstIntervalComponent(tangent_normal, 1u), state), state),
                ddgiFirstIntervalScale(ray_native,
                    ddgiFirstIntervalComponent(tangent_normal, 2u), state), state), state);
    }
    result.mapped_n = mapped;

    result.bary_l2 = ddgiFirstIntervalL2Gap(result.bary_native_n, state);
    result.tangent_l2 = ddgiFirstIntervalL2Gap(result.tangent, state);
    result.ray_l2 = ddgiFirstIntervalL2Gap(result.ray_native_n, state);
    result.geometry_l2 = ddgiFirstIntervalL2Gap(result.geometry_n, state);
    result.bitangent_l2 = ddgiFirstIntervalL2Gap(result.bitangent, state);
    result.mapped_l2 = ddgiFirstIntervalL2Gap(result.mapped_n, state);
    result.max_l2 = max(max(max(result.bary_l2, result.tangent_l2),
                            max(result.ray_l2, result.geometry_l2)),
                        max(result.bitangent_l2, result.mapped_l2));
    bool gate = state.rte_domain_valid != 0u && state.real_domain_valid != 0u &&
                state.reason_bits == 0u && result.max_l2 <= DDGI_FIRST_INTERVAL_GATE;
    result.status = gate ? DDGI_FIRST_INTERVAL_STATUS_COMPLETE : DDGI_FIRST_INTERVAL_STATUS_UNKNOWN;
    result.math_gate_pass = gate ? 1u : 0u;
    if (!gate && result.max_l2 > DDGI_FIRST_INTERVAL_GATE)
        state.reason_bits |= DDGI_FIRST_INTERVAL_REASON_BOUND_REJECTED;
    result.reason_bits = state.reason_bits;
    result.branch_bits = state.branch_bits;
    result.operation_count = state.operation_count;
    result.rte_domain_valid = state.rte_domain_valid;
    result.real_domain_valid = state.real_domain_valid;
    return result;
}

#endif // LUMEN_DDGI_FIRST_RTE_INTERVAL_GLSL
