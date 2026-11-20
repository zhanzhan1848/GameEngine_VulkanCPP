// Native frozen-profile reprojection. Noise does not increase EMA alpha.
struct TemporalData {
    previousVP:mat4x4<f32>,totalProbes:u32,alpha:f32,positionThreshold:f32,normalThreshold:f32,
    clampScale:f32,maxGradient:f32,gridWidth:u32,gridHeight:u32,mode:f32,pad0:f32,pad1:f32,pad2:f32,
}
@group(0) @binding(0) var<storage,read> currentSH:array<vec4<f32>>;
@group(0) @binding(1) var<storage,read> historySH:array<vec4<f32>>;
@group(0) @binding(2) var<storage,read> currentPositions:array<vec4<f32>>;
@group(0) @binding(3) var<storage,read> historyPositions:array<vec4<f32>>;
@group(0) @binding(4) var<storage,read_write> outputSH:array<vec4<f32>>;
@group(0) @binding(5) var<uniform> temporal:TemporalData;
@group(0) @binding(6) var<storage,read> currentNormals:array<vec4<f32>>;
@group(0) @binding(7) var<storage,read> historyNormals:array<vec4<f32>>;
@compute @workgroup_size(64)
fn measured_screen_temporal(@builtin(global_invocation_id) gid:vec3<u32>){
    let probe=gid.x;if(probe>=temporal.totalProbes){return;}let base=probe*4u;
    let position=currentPositions[probe];
    // Average can contain NaN L1 for inactive zero normals. Reject by position
    // before touching those SH values, exactly as the frozen consumer does.
    if(position.w<=0.0){for(var b=0u;b<4u;b++){outputSH[base+b]=vec4<f32>(0);}return;}
    let current0=currentSH[base];let confidence=current0.w;
    if(confidence<=0.0){for(var b=0u;b<4u;b++){outputSH[base+b]=vec4<f32>(0);}return;}
    var history:array<vec4<f32>,4>;var total=0.0;
    let clip=temporal.previousVP*vec4<f32>(position.xyz,1);
    if(clip.w>0.0){
        let ndc=clip.xy/clip.w;let uv=vec2<f32>(ndc.x*.5+.5,1.0-(ndc.y*.5+.5));
        if(all(uv>=vec2<f32>(0))&&all(uv<=vec2<f32>(1))){
            let cell=uv*vec2<f32>(f32(temporal.gridWidth),f32(temporal.gridHeight))-.5;
            let corner=vec2<i32>(floor(cell));let fraction=fract(cell);let normal=currentNormals[probe];
            for(var y=0;y<=1;y++){for(var x=0;x<=1;x++){
                let c=corner+vec2<i32>(x,y);if(any(c<vec2<i32>(0))||c.x>=i32(temporal.gridWidth)||c.y>=i32(temporal.gridHeight)){continue;}
                let index=u32(c.y)*temporal.gridWidth+u32(c.x);let oldPosition=historyPositions[index];
                if(oldPosition.w<=0.0||distance(position.xyz,oldPosition.xyz)>temporal.positionThreshold){continue;}
                let oldNormal=historyNormals[index];
                if(oldNormal.w>0.0&&normal.w>0.0&&dot(normal.xyz,normal.xyz)>1e-8&&dot(oldNormal.xyz,oldNormal.xyz)>1e-8&&dot(normalize(normal.xyz),normalize(oldNormal.xyz))<temporal.normalThreshold){continue;}
                let oldBase=index*4u;if(historySH[oldBase].w<=0.0){continue;}
                let weight=select(fraction.x,1.0-fraction.x,x==0)*select(fraction.y,1.0-fraction.y,y==0);
                for(var b=0u;b<4u;b++){history[b]+=historySH[oldBase+b]*weight;}total+=weight;
            }}
        }
    }
    if(total<=.001||confidence<.05){for(var b=0u;b<4u;b++){outputSH[base+b]=currentSH[base+b];}return;}
    for(var b=0u;b<4u;b++){history[b]/=total;}
    if(temporal.mode>.5&&temporal.mode<1.5){
        var sum=current0.xyz;var sumSq=sum*sum;var count=1.0;let x=probe%temporal.gridWidth;
        if(x>0u){let value=currentSH[(probe-1u)*4u];if(value.w>0.0){sum+=value.xyz;sumSq+=value.xyz*value.xyz;count+=1.0;}}
        if(x+1u<temporal.gridWidth){let value=currentSH[(probe+1u)*4u];if(value.w>0.0){sum+=value.xyz;sumSq+=value.xyz*value.xyz;count+=1.0;}}
        let mean=sum/count;let sigma=sqrt(max(sumSq/count-mean*mean,vec3<f32>(0)));let floorValue=max(abs(mean)*.1,vec3<f32>(.01));
        let clamped=clamp(history[0].xyz,mean-sigma*temporal.clampScale-floorValue,mean+sigma*temporal.clampScale+floorValue);
        let shrink=clamp(abs(clamped)/max(abs(history[0].xyz),vec3<f32>(1e-4)),vec3<f32>(0),vec3<f32>(1));
        history[0]=vec4<f32>(clamped,history[0].w);for(var b=1u;b<4u;b++){history[b]=vec4<f32>(history[b].xyz*shrink,history[b].w);}
    }
    let alpha=clamp(temporal.alpha,0.0,1.0);
    for(var b=0u;b<4u;b++){let current=currentSH[base+b];outputSH[base+b]=vec4<f32>(mix(history[b].xyz,current.xyz,alpha),current.w);}
}
