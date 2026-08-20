#version 450
// P4c-F3: sample set0 binding0 combined image sampler, passthrough.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(tex, fragUV);
}
