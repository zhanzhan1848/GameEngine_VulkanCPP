#version 460 core
// Depth-only fragment shader for CameraDepth prepass.
// Empty body — depth is written via depth attachment through gl_FragCoord.z
// when depthWriteEnable=true in the pipeline.
void main() {}
