// VSM moments use the same vertex-pulling shadow pass as depth-only rendering.
@fragment
fn shadow_moments_fs(@builtin(position) position: vec4f) -> @location(0) vec2f {
    let z = position.z;
    return vec2f(z, z * z);
}
