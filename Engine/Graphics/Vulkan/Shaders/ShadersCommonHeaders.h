#define BLOCKER_SEARCH_NUM_SAMPLES NUM_SAMPLES
#define PCF_NUM_SAMPLES NUM_SAMPLES
#define LIGHTSIZE 1.0
#define SAMPLESTRDIE 50
#define SHADOWMAPSIZE 2048

#define EPS 1e-3
#define PI 3.141592653589793
#define PI2 6.283185307179586

vec2 poissonDisk[20];

vec4 trans_scale(vec3 transform, vec3 rotate, vec3 scale, vec3 position)
{
    mat3 mx, my, mz;
    float s = sin(rotate.x);
    float c = cos(rotate.x);

    mx[0] = vec3(c, s, 0.0);
    mx[1] = vec3(-s, c, 0.0);
    mx[2] = vec3(0.0, 0.0, 1.0);

    s = sin(rotate.y);
    c = cos(rotate.y);

    my[0] = vec3(c, 0.0, s);
    my[1] = vec3(0.0, 1.0, 0.0);
    my[2] = vec3(-s, 0.0, c);

    s = sin(rotate.z);
    c = cos(rotate.z);

    mz[0] = vec3(1.0, 0.0, 0.0);
    mz[1] = vec3(0.0, c, s);
    mz[2] = vec3(0.0, -s, c);

    mat3 rotMat = mz * my * mx;

    vec4 locPos = vec4(rotMat * position.xyz, 1.0);
    return vec4((locPos.xyz * scale) + transform, 1.0);
}

mat4 rotateMatrix(in vec3 rotate)
{
    float s = sin(rotate.y);
    float c = cos(rotate.y);
    mat4 gRotMat;
    gRotMat[0] = vec4(c, 0.0, s, 0.0);
    gRotMat[1] = vec4(0.0, 1.0, 0.0, 0.0);
    gRotMat[2] = vec4(-s, 0.0, c, 0.0);
    gRotMat[3] = vec4(0.0, 0.0, 0.0, 1.0);
    return gRotMat;
}

float linearizeDepth(float depth)
{
    float n = 0.001;
    float f = 10.0;
    float z = depth;
    return (2.0 * n) / (f + n - z * (f - n));
}

float rand_1_to_1(float x)
{
    return fract(sin(x) * 10000.0);
}

float rand_2_to_1(vec2 uv)
{
    float a = 12.9898, b = 78.233, c = 43758.5453;
    float dt = dot(uv.xy, vec2(a, b)), sn = mod(dt, PI);
    return fract(sin(sn) * c);
}

float sampleShadowMap(sampler2D shadowMap, vec4 coord)
{
    float shadow = 1.0;
    if (coord.z > -1.0 && coord.z < 1.0)
    {
        float dist = texture(shadowMap, coord.st).r;
        if (coord.w > 0.0 && dist < coord.z)
        {
            shadow = 0.1;
        }
    }
    return shadow;
}

void poissonDiskSamples(const in vec2 randomSeed, in int num_samples, in int num_rings)
{
    float angle_step = PI2 * float(num_rings) / float(num_samples);
    float inv_num_samples = 1.0 / float(num_samples);

    float angle = rand_2_to_1(randomSeed) * PI2;
    float radius = inv_num_samples;
    float radiusStep = radius;

    for (int i = 0; i < num_samples; i++)
    {
        poissonDisk[i] = vec2(cos(angle), sin(angle)) * pow(radius, 0.75);
        radius += radiusStep;
        angle += angle_step;
    }
}

void uniformDiskSamples(const in vec2 randomSeed, in int num_samples)
{
    float randNum = rand_2_to_1(randomSeed);
    float sampleX = rand_1_to_1(randNum);
    float sampleY = rand_1_to_1(sampleX);

    float angle = sampleX * PI2;
    float radius = sqrt(sampleY);

    for (int i = 0; i < num_samples; i++)
    {
        poissonDisk[i] = vec2(radius * cos(angle), radius * sin(angle));
        sampleX = rand_1_to_1(sampleY);
        sampleY = rand_1_to_1(sampleX);

        angle = sampleX * PI2;
        radius = sqrt(sampleY);
    }
}

float PCF(sampler2D shadowMap, vec4 coords)
{
    int num_samples = 20;

    uniformDiskSamples(coords.xy, num_samples);

    float depth = 0.0;
    int per = 0;
    for (int i = 0; i < num_samples; i++)
    {
        vec4 uv = vec4(poissonDisk[i] * float(SAMPLESTRDIE) / float(SHADOWMAPSIZE) + coords.xy, coords.zw);
        depth = sampleShadowMap(shadowMap, uv);
        if (depth == 1.0) per++;
    }
    return ((float(per)) / float(num_samples));
}

float findBlocker(sampler2D shadowMap, vec4 uv, float zReceiver, int num_samples)
{
    float findSize = float(LIGHTSIZE) * zReceiver;
    float depthSum = 0.0;
    int depthNum = 0;

    uniformDiskSamples(uv.xy, num_samples);
    for (int i = 0; i < num_samples; i++)
    {
        vec4 uv1 = vec4(poissonDisk[i] * float(SAMPLESTRDIE) / float(SHADOWMAPSIZE) + uv.xy, uv.zw);
        float depth = sampleShadowMap(shadowMap, uv1);
        if (depth == 0.1)
        {
            depthSum += rand_1_to_1(i);
            depthNum++;
        }
    }
    if (depthNum == 0) return 1.0;
    return depthSum / float(depthNum);
}

float PCSS(sampler2D shadowMap, vec4 coords)
{
    int num_samples = 20;

    float blocker = findBlocker(shadowMap, coords, coords.z, num_samples);

    float penumbraSize = float(LIGHTSIZE) * (coords.z - blocker) / blocker;

    uniformDiskSamples(coords.xy, num_samples);

    float depth = 0.0;
    int per = 0;
    for (int i = 0; i < num_samples; i++)
    {
        vec4 uv = vec4(poissonDisk[i] * penumbraSize * float(SAMPLESTRDIE) / float(SHADOWMAPSIZE) + coords.xy, coords.zw);
        depth = sampleShadowMap(shadowMap, uv);
        if (depth == 1.0) per++;
    }
    return float(per) / float(num_samples);
}

vec3 blinnPhong(sampler2D colorMap, vec2 uv, vec3 inPos, vec3 inNormal, vec3 camPos, vec3 lightPos, float lightIntensity, float Ks)
{
    vec3 color = texture(colorMap, uv).rgb;
    color = pow(color, vec3(2.2));

    vec3 ambient = 0.05 * color;

    vec3 lightDir = normalize(lightPos);
    vec3 normal = normalize(inNormal);
    float diff = max(dot(lightDir, normal), 0.0);
    vec3 light_atten_coff = lightIntensity / pow((lightPos - inPos), vec3(2.0));
    vec3 diffuse = diff * light_atten_coff * color;

    vec3 viewDir = normalize(camPos - inPos);
    vec3 halfDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(halfDir, normal), 0.0), 32.0);
    vec3 specular = Ks * light_atten_coff * spec;

    vec3 radiance = (ambient + diffuse + specular);
    vec3 phongColor = pow(radiance, vec3(1.0 / 2.2));
    return phongColor;
}