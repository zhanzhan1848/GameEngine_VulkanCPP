#include "Common.h"
#include "BuildinMaterial.metal"
#include "SDFBasicShape.metal"

struct VertexOutput
{
    float4 position [[position]];
    float2 uv;
};

////////////////////////////////////////// Function ///////////////////////////////////////
// 静态相机变换矩阵生成函数
float3x3 setCamera(float3 ro, float3 ta, float cr) {
    float3 cw = normalize(ta - ro);           // 前向量
    float3 cp = float3(sin(cr), cos(cr), 0.0); // 临时上向量
    float3 cu = normalize(cross(cw, cp));     // 右向量
    float3 cv = cross(cu, cw);                // 上向量
    return float3x3(cu, cv, cw);
}

// map 函数, 定义了一个场景的签名距离场 (SDF)，返回距离 (res.x) 和材质 ID (res.y)
// 输入：pos（vec3），表示世界空间中的点。
// 输出：vec2，x 是到最近表面的距离，y 是材质 ID。
float2 map(float3 pos) {
    float2 res = float2(pos.y, 0.0);  // 地面 (y=0)

    // 第一个包围盒
    if (SDBox(pos - float3(-2.0, 0.3, 0.25), float3(0.3, 0.3, 1.0)) < res.x) {
        res = OPU(res, float2(SDSphere(pos - float3(-2.0, 0.25, 0.0), 0.25), 26.9));
        res = OPU(res, float2(SDRhombus((pos - float3(-2.0, 0.25, 1.0)).xzy, 0.15, 0.25, 0.04, 0.08), 17.0));
    }

    // 第二个包围盒
    if (SDBox(pos - float3(0.0, 0.3, -1.0), float3(0.35, 0.3, 2.5)) < res.x) {
        res = OPU(res, float2(SDCappedTorus((pos - float3(0.0, 0.30, 1.0)) * float3(1, -1, 1), float2(0.866025, -0.5), 0.25, 0.05), 25.0));
        res = OPU(res, float2(SDBoxFrame(pos - float3(0.0, 0.25, 0.0), float3(0.3, 0.25, 0.2), 0.025), 16.9));
        res = OPU(res, float2(SDCone(pos - float3(0.0, 0.45, -1.0), float2(0.6, 0.8), 0.45), 55.0));
        res = OPU(res, float2(SDCappedCone(pos - float3(0.0, 0.25, -2.0), 0.25, 0.25, 0.1), 13.67));
        res = OPU(res, float2(SDSolidAngle(pos - float3(0.0, 0.00, -3.0), float2(3, 4) / 5.0, 0.4), 49.13));
    }

    // 第三个包围盒
    if (SDBox(pos - float3(1.0, 0.3, -1.0), float3(0.35, 0.3, 2.5)) < res.x) {
        res = OPU(res, float2(SDTorus((pos - float3(1.0, 0.30, 1.0)).xzy, float2(0.25, 0.05)), 7.1));
        res = OPU(res, float2(SDBox(pos - float3(1.0, 0.25, 0.0), float3(0.3, 0.25, 0.1)), 3.0));
        res = OPU(res, float2(SDCapsule(pos - float3(1.0, 0.00, -1.0), float3(-0.1, 0.1, -0.1), float3(0.2, 0.4, 0.2), 0.1), 31.9));
        res = OPU(res, float2(SDCylinder(pos - float3(1.0, 0.25, -2.0), float2(0.15, 0.25)), 8.0));
        res = OPU(res, float2(SDHexPrism(pos - float3(1.0, 0.2, -3.0), float2(0.2, 0.05)), 18.4));
    }

    // 第四个包围盒
    if (SDBox(pos - float3(-1.0, 0.35, -1.0), float3(0.35, 0.35, 2.5)) < res.x) {
        res = OPU(res, float2(SDPyramid(pos - float3(-1.0, -0.6, -3.0), 1.0), 13.56));
        res = OPU(res, float2(SDOctahedron(pos - float3(-1.0, 0.15, -2.0), 0.35), 23.56));
        res = OPU(res, float2(SDTriPrism(pos - float3(-1.0, 0.15, -1.0), float2(0.3, 0.05)), 43.5));
        res = OPU(res, float2(SDEllipsoid(pos - float3(-1.0, 0.25, 0.0), float3(0.2, 0.25, 0.05)), 43.17));
        res = OPU(res, float2(SDHorseshoe(pos - float3(-1.0, 0.25, 1.0), float2(cos(1.3), sin(1.3)), 0.2, 0.3, float2(0.03, 0.08)), 11.5));
    }

    // 第五个包围盒
    if (SDBox(pos - float3(2.0, 0.3, -1.0), float3(0.35, 0.3, 2.5)) < res.x) {
        res = OPU(res, float2(SDOctogonPrism(pos - float3(2.0, 0.2, -3.0), 0.2, 0.05), 51.8));
        res = OPU(res, float2(SDCylinder(pos - float3(2.0, 0.14, -2.0), float3(0.1, -0.1, 0.0), float3(-0.2, 0.35, 0.1), 0.08), 31.2));
        res = OPU(res, float2(SDCappedCone(pos - float3(2.0, 0.09, -1.0), float3(0.1, 0.0, 0.0), float3(-0.2, 0.40, 0.1), 0.15, 0.05), 46.1));
        res = OPU(res, float2(SDRoundCone(pos - float3(2.0, 0.15, 0.0), float3(0.1, 0.0, 0.0), float3(-0.1, 0.35, 0.1), 0.15, 0.05), 51.7));
        res = OPU(res, float2(SDRoundCone(pos - float3(2.0, 0.20, 1.0), 0.2, 0.1, 0.3), 37.0));
    }

    return res;
}

// https://iquilezles.org/articles/boxfunctions
float2 iBox( float3 ro, float3 rd, float3 rad ) 
{
    float3 m = 1.0 / rd;
    float3 n = m * ro;
    float3 k = abs(m) * rad;
    float3 t1 = -n - k;
    float3 t2 = -n + k;
	return float2( max( max( t1.x, t1.y ), t1.z ),
	             min( min( t2.x, t2.y ), t2.z ) );
}

float2 raycast( float3 ro, float3 rd )
{
    float2 res = float2(-1.0,-1.0);

    float tmin = 1.0;
    float tmax = 20.0;

    // raytrace floor plane
    float tp1 = (0.0 - ro.y) / rd.y;
    if( tp1 > 0.0 )
    {
        tmax = min( tmax, tp1 );
        res = float2( tp1, 1.0 );
    }
    //else return res;
    
    // raymarch primitives   
    float2 tb = iBox( ro - float3(0.0, 0.4, -0.5), rd, float3(2.5, 0.41, 3.0) );
    if( tb.x<tb.y && tb.y>0.0 && tb.x<tmax)
    {
        //return vec2(tb.x, 2.0);
        tmin = max(tb.x, tmin);
        tmax = min(tb.y, tmax);

        float t = tmin;
        for( int i = 0; i < 70 && t < tmax; i++ )
        {
            float2 h = map( ro + rd * t );
            if( abs(h.x) < (0.0001 * t) )
            { 
                res = float2(t, h.y); 
                break;
            }
            t += h.x;
        }
    }
    
    return res;
}

////////////////////////////////////////// Calculate Function ///////////////////////////////////////
// https://iquilezles.org/articles/rmshadows
float CalcSoftshadow( float3 ro, float3 rd, float mint, float tmax )
{
    // bounding volume
    float tp = (0.8 - ro.y) / rd.y; if( tp > 0.0 ) tmax = min( tmax, tp );

    float res = 1.0;
    float t = mint;
    for( int i = 0; i < 24; i++ )
    {
		float h = map( ro + rd * t ).x;
        float s = clamp(8.0 * h / t, 0.0, 1.0);
        res = min( res, s );
        t += clamp( h, 0.01, 0.2 );
        if( res < 0.004 || t > tmax ) break;
    }
    res = clamp( res, 0.0, 1.0 );
    return res * res * (3.0 - 2.0 * res);
}

// https://iquilezles.org/articles/normalsSDF
float3 CalcNormal( float3 pos )
{
#if 0
    float2 e = float2(1.0, -1.0) * 0.5773 * 0.0005;
    return normalize( e.xyy * map( pos + e.xyy ).x + 
					  e.yyx * map( pos + e.yyx ).x + 
					  e.yxy * map( pos + e.yxy ).x + 
					  e.xxx * map( pos + e.xxx ).x );
#else
    // inspired by tdhooper and klems - a way to prevent the compiler from inlining map() 4 times
    float3 n = float3(0.0);
    for( int i = 0; i < 4; i++ )
    {
        float3 e = 0.5773 * (2.0 * float3((((i + 3) >> 1) & 1),((i >> 1) & 1),(i & 1)) - 1.0);
        n += e * map(pos + 0.0005 * e).x;
      //if( n.x + n.y + n.z > 100.0 ) break;
    }
    return normalize(n);
#endif    
}

// https://iquilezles.org/articles/nvscene2008/rwwtt.pdf
float CalcAO( float3 pos, float3 nor )
{
	float occ = 0.0;
    float sca = 1.0;
    for( int i = 0; i < 5; i++ )
    {
        float h = 0.01 + 0.12 * float(i) / 4.0;
        float d = map( pos + h * nor ).x;
        occ += (h - d) * sca;
        sca *= 0.95;
        if( occ > 0.35 ) break;
    }
    return clamp( 1.0 - 3.0 * occ, 0.0, 1.0 ) * (0.5 + 0.5 * nor.y);
}

////////////////////////////////////////// Calculate Function ///////////////////////////////////////

float3 render(float3 ro, float3 rd, float3 rdx, float3 rdy) {
    // background
    float3 col = float3(0.7, 0.7, 0.9) - max(rd.y, 0.0) * 0.3;
    
    // raycast scene
    float2 res = raycast(ro, rd);  // 假设 raycast 已定义
    float t = res.x;
    float m = res.y;
    
    if (m > -0.5) {
        float3 pos = ro + t * rd;
        float3 nor = (m < 1.5) ? float3(0.0, 1.0, 0.0) : CalcNormal(pos);  // 假设 calcNormal 已定义
        float3 ref = reflect(rd, nor);
        
        // material
        col = 0.2 + 0.2 * sin(m * 2.0 + float3(0.0, 1.0, 2.0));
        float ks = 1.0;
        
        if (m < 1.5) {
            // project pixel footprint into the plane
            float3 dpdx = ro.y * (rd / rd.y - rdx / rdx.y);
            float3 dpdy = ro.y * (rd / rd.y - rdy / rdy.y);
            
            float f = checkersGradBox(3.0 * pos.xz, 3.0 * dpdx.xz, 3.0 * dpdy.xz);  // 假设已定义
            col = 0.15 + f * float3(0.05);
            ks = 0.4;
        }
        
        // lighting
        float occ = CalcAO(pos, nor);  // 假设 calcAO 已定义
        float3 lin = float3(0.0);
        
        // sun
        {
            float3 lig = normalize(float3(-0.5, 0.4, -0.6));
            float3 hal = normalize(lig - rd);
            float dif = clamp(dot(nor, lig), 0.0, 1.0);
            if (dif > 0.0001) {
                dif *= CalcSoftshadow(pos, lig, 0.02, 2.5);  // 假设已定义
            }
            float spe = pow(clamp(dot(nor, hal), 0.0, 1.0), 16.0);
            spe *= dif;
            spe *= 0.04 + 0.96 * pow(clamp(1.0 - dot(hal, lig), 0.0, 1.0), 5.0);
            lin += col * 2.20 * dif * float3(1.30, 1.00, 0.70);
            lin += 5.00 * spe * float3(1.30, 1.00, 0.70) * ks;
        }
        
        // sky
        {
            float dif = sqrt(clamp(0.5 + 0.5 * nor.y, 0.0, 1.0));
            dif *= occ;
            float spe = smoothstep(-0.2, 0.2, ref.y);
            spe *= dif;
            spe *= 0.04 + 0.96 * pow(clamp(1.0 + dot(nor, rd), 0.0, 1.0), 5.0);
            if (spe > 0.001) {
                spe *= CalcSoftshadow(pos, ref, 0.02, 2.5);
            }
            lin += col * 0.60 * dif * float3(0.40, 0.60, 1.15);
            lin += 2.00 * spe * float3(0.40, 0.60, 1.30) * ks;
        }
        
        // back
        {
            float dif = clamp(dot(nor, normalize(float3(0.5, 0.0, 0.6))), 0.0, 1.0) * 
                       clamp(1.0 - pos.y, 0.0, 1.0);
            dif *= occ;
            lin += col * 0.55 * dif * float3(0.25, 0.25, 0.25);
        }
        
        // sss
        {
            float dif = pow(clamp(1.0 + dot(nor, rd), 0.0, 1.0), 2.0);
            dif *= occ;
            lin += col * 0.25 * dif * float3(1.00, 1.00, 1.00);
        }
        
        col = lin;
        col = mix(col, float3(0.7, 0.7, 0.9), 1.0 - exp(-0.0001 * t * t * t));
    }
    
    return clamp(col, 0.0, 1.0);
}

////////////////////////////////////////// Function ///////////////////////////////////////

// 常量定义
// constant float3 cameraTarget = float3(0.25f, -0.75f, 0.25f);  // 相机目标点
// constant float3 cameraPosition = cameraTarget + float3(2.5f, 2.2f, 2.5f);     // 相机位置（静态）
// focal length
constant float focal_length = 2.5f;

fragment float4 post_process_ps(VertexOutput fragInput [[stage_in]],
                                constant GlobalShaderData* globalData [[buffer(0)]],
                                texture2d<float, access::sample> depth_tex [[texture(0)]],
                                texture2d<float, access::sample> gpass_tex [[texture(1)]],
                                texture2d<float, access::sample> ssao_tex [[texture(2)]],
                                sampler s [[sampler(0)]])
{
    // // 翻转y坐标
    // float2 p = float2(
    //     (2.0f * fragInput.position.x - globalData->CameraPositionAndViewWidth.w) / globalData->CameraDirectionAndViewHeight.w,
    //     -(2.0f * fragInput.position.y - globalData->CameraDirectionAndViewHeight.w) / globalData->CameraDirectionAndViewHeight.w
    // );
    // float3 final_color = float3(0.0f, 0.f, 0.f);
    // float3 cPos = globalData->CameraPositionAndViewWidth.xyz;
    // // float3x3 cameraMatrix = setCamera(cPos, cameraTarget, 0.0); // 相机变换矩阵
    // float3 rd = (globalData->View * normalize(float4(p.x, p.y, focal_length, 1.0))).xyz; // 光线方向
    // // float3 rd = cameraMatrix * normalize(float3(p.x, p.y, focal_length));
    
    // // 同样需要翻转这两个坐标的y值
    // float2 px = float2(
    //     (2.0f * (fragInput.position.x + 1.0f) - globalData->CameraPositionAndViewWidth.w) / globalData->CameraDirectionAndViewHeight.w,
    //     -(2.0f * (fragInput.position.y + 0.0f) - globalData->CameraDirectionAndViewHeight.w) / globalData->CameraDirectionAndViewHeight.w
    // );
    // float2 py = float2(
    //     (2.0f * (fragInput.position.x + 0.0f) - globalData->CameraPositionAndViewWidth.w) / globalData->CameraDirectionAndViewHeight.w,
    //     -(2.0f * (fragInput.position.y + 1.0f) - globalData->CameraDirectionAndViewHeight.w) / globalData->CameraDirectionAndViewHeight.w
    // );
    // float3 rdx = (globalData->View * normalize(float4(px.x, px.y, focal_length, 1.0))).xyz;
    // float3 rdy = (globalData->View * normalize(float4(py.x, py.y, focal_length, 1.0))).xyz;
    // // float3 rdx = cameraMatrix * normalize(float3(px.x, px.y, focal_length));
    // // float3 rdy = cameraMatrix * normalize(float3(py.x, py.y, focal_length));

    // // render	
    // float3 color = render( cPos, rd, rdx, rdy );
    
    // // gamma
    // color = pow( color, float3(0.4545) );
    // final_color += color;
    // return float4(final_color, 1.0f);
    float2 uv = fragInput.uv;
    uv = float2(uv.x, 1.f - uv.y);
    float3 color = gpass_tex.sample(s, uv).rgb;
    float4 ssdo = ssao_tex.sample(s, uv);
    float3 indirect = ssdo.rgb;
    float ao = ssdo.a;

    color = color * ao; // + indirect;
    
    // 应用gamma校正以获得正确的颜色显示
    // color = pow(color, float3(0.4545)); // 1/2.2 ≈ 0.4545
    
    return float4(color, 1.0);
}