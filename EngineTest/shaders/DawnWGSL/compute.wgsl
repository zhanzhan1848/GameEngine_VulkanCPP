// Compute shader for Dawn WebGPU backend testing
// Writes sequential values to a storage buffer

struct Data {
    values: array<u32>,
}

@group(0) @binding(0) var<storage, read_write> output: Data;

@compute @workgroup_size(64)
fn cs_main(@builtin(global_invocation_id) gid: vec3u) {
    let idx = gid.x;
    if (idx < arrayLength(&output.values)) {
        output.values[idx] = idx * 2u + 1u;
    }
}
