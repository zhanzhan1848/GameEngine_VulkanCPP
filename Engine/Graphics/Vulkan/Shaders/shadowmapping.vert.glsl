#version 450

layout(location = 0) in vec3 inPos;

layout(location = 3) in vec3 transform;
layout(location = 4) in vec3 rotate;
layout(location = 5) in vec3 scale;

layout(binding = 0) uniform UniformBufferObject
{
	mat4	model;
	mat4	view;
	mat4	projection;
} ubo;

out gl_PerVertex
{
    vec4 gl_Position;
};

void main()
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
    mat4 gRotMat;
    s = sin(rotate.y);
    c = cos(rotate.y);
    gRotMat[0] = vec4(c, 0.0, s, 0.0);
    gRotMat[1] = vec4(0.0, 1.0, 0.0, 0.0);
    gRotMat[2] = vec4(-s, 0.0, c, 0.0);
    gRotMat[3] = vec4(0.0, 0.0, 0.0, 1.0);

    vec4 locPos = vec4(rotMat * inPos.xyz, 1.0);
    vec4 pos = vec4((locPos.xyz * scale) + transform, 1.0);
    //vec4 pos = ubo.model * vec4(inPos, 1.0);

    gl_Position = ubo.projection * ubo.view * gRotMat * pos;
}