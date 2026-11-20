// Preserve the exact D32 value alongside the matching resolved color slot.
@group(0) @binding(0) var sourceDepth:texture_depth_2d;
@group(0) @binding(1) var targetDepth:texture_storage_2d<r32float,write>;
@compute @workgroup_size(8,8,1)
fn copy_taa_depth(@builtin(global_invocation_id) id:vec3<u32>){
    if(any(id.xy>=textureDimensions(sourceDepth))){return;}
    textureStore(targetDepth,vec2<i32>(id.xy),vec4<f32>(textureLoad(sourceDepth,vec2<i32>(id.xy),0),0,0,0));
}
