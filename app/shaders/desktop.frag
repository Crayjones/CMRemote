#version 440
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uTex;
void main() {
    outColor = texture(uTex, vUv);
}
