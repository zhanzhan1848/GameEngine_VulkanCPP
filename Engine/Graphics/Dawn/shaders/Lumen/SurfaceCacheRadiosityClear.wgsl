@group(0) @binding(0) var grid:texture_storage_2d<rgba16float,write>;
@group(0) @binding(1) var aux:texture_storage_2d<rgba16float,write>;
@group(0) @binding(2) var output:texture_storage_2d<rgba16float,write>;
@compute @workgroup_size(8,8) fn tile_clear(@builtin(global_invocation_id) id:vec3<u32>){if(all(id.xy<textureDimensions(grid))){textureStore(grid,vec2<i32>(id.xy),vec4<f32>(0));textureStore(aux,vec2<i32>(id.xy),vec4<f32>(0));}if(all(id.xy<textureDimensions(output))){textureStore(output,vec2<i32>(id.xy),vec4<f32>(0));}}
