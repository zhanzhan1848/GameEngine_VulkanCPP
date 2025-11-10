```mermaid
flowchart TD

    %% ========== Pre Pass ==========
    A[Start Frame] --> B[Depth Pre-Pass]
    B --> C[Shadow Map Pass]
    C --> D[Occlusion Culling / Hi-Z Buffer]

    %% ========== Forward / Deferred Split ==========
    D -->|Forward Path| F1[Forward Pass \n Geometry + Lighting]
    D -->|Deferred Path| G1[G-Buffer Pass \n Albedo, Normal, Roughness, Depth, etc.]

    %% ========== Lighting ==========
    G1 --> G2[Lighting Pass \n Deferred Shading]
    F1 --> H[Light Accumulation Buffer]
    G2 --> H[Light Accumulation Buffer]

    %% ========== Ray Tracing ==========
    D --> RT1[Ray Tracing Pass]
    RT1 --> RT2[RT Shadows]
    RT1 --> RT3[RT Reflections]
    RT1 --> RT4[RT Global Illumination]
    RT1 --> RT5[RT Ambient Occlusion]
    RT2 --> H
    RT3 --> H
    RT4 --> H
    RT5 --> H

    %% ========== Transparency / Special ==========
    H --> I1[Transparency Pass\nForward Only]
    I1 --> I2[Decals / Projectors]
    I2 --> I3[Volumetric Effects\nFog, Clouds, God Rays]
    I3 --> I4[Skybox / Atmosphere]

    %% ========== Post Processing ==========
    I4 --> J1[Tone Mapping (HDR → LDR)]
    J1 --> J2[Anti-Aliasing (TAA)]
    J2 --> J3[Bloom]
    J3 --> J4[Depth of Field]
    J4 --> J5[Motion Blur]
    J5 --> J6[Color Grading / LUT]
    J6 --> J7[Lens Effects\nGlare, Dirt, Vignette]

    %% ========== Compute / Async ==========
    D --> K1[Compute Pass Async]
    K1 --> K2[GPU Particles]
    K1 --> K3[Screen-Space Effects\nSSAO, SSR, SSGI, SSDO]
    K1 --> K4[Lighting Probe / GI Update]
    K2 --> H
    K3 --> H
    K4 --> H

    %% ========== Compose ==========
    J7 --> L1[UI / HUD Overlay]
    L1 --> M[Final Compose]
    M --> N[Present / Swapchain Output]
```