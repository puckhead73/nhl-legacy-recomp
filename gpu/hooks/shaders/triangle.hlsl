// High-cut C-1: minimal geometry shader to prove the plume render path (vertex buffer +
// pipeline + draw) beyond the H-2 clears. Vertex format: POSITION (float3) + COLOR (float4),
// matching third_party/plume/examples/triangle. Compiled to SPIR-V via dxc (-spirv) into
// shaders/triangle{Vert,Frag}.hlsl.spirv.h by plume's shader cmake (CMakeLists.txt:
// plume_compile_{vertex,pixel}_shader); plume-Vulkan is the required in-process backend
// (a 2nd D3D12 device TDRs rexglue). Blobs: triangle{Vert,Frag}BlobSPIRV / ...BlobDXIL.

struct VSInput {
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct VSOutput {
    float4 position : SV_Position;
    float4 color    : COLOR;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 1.0f);
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    return input.color;
}
