// High-cut C-3a: a solid-color pixel shader with NO inputs, paired with a TRANSLATED Xenos
// vertex shader to build a real plume graphics pipeline (defer textures / real PS translation).
// A zero-input PS is interface-compatible with any VS output set (Vulkan permits a fragment
// shader to consume fewer outputs than the VS produces), so it links against the Xenos VS that
// writes only gl_Position. Compiled to SPIR-V via dxc by plume's shader cmake -> solidFragBlobSPIRV.

float4 PSMain() : SV_Target {
    return float4(1.0f, 0.3f, 0.1f, 1.0f);  // unmistakable solid orange
}
