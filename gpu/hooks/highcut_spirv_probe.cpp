// High-cut path C, P-1: glslang link/compile probe.
//
// Proves two things needed before porting the Xenos->SPIR-V translator (P-2):
//   1. glslang's SPIR-V backend (spv::Builder) BUILDS + LINKS in the recomp's clang/MSVC
//      toolchain (the vendored FetchContent glslang, target SPIRV).
//   2. The SDK's spirv_builder.h (Xenia's, which `class SpirvBuilder : public spv::Builder`)
//      COMPILES against that vendored glslang — i.e. no fatal API drift in the spv::Builder
//      surface the translator relies on.
//
// Self-gated on NHL_HIGHCUT_SPIRV_PROBE; emits a trivial empty SPIR-V module and logs the word
// count + magic. No effect on any normal run.

#include <cstdint>
#include <cstdlib>
#include <vector>

#include <rex/logging.h>

// Including the SDK header (Xenia's) is itself the compile test — it pulls in
// <SPIRV/SpvBuilder.h> from the vendored glslang via the include path added in CMakeLists.
#include <rex/graphics/pipeline/shader/spirv_builder.h>
// P-2b: the ported translator. Constructing one + calling a method below forces the
// linker to resolve SpirvShaderTranslator (ctor, Features(bool), the virtual override,
// and transitively the base ShaderTranslator helpers) — the P-2b linkage proof.
#include <rex/graphics/pipeline/shader/spirv_translator.h>

extern "C" void HighcutSpirvProbe() {
    if (!std::getenv("NHL_HIGHCUT_SPIRV_PROBE")) {
        return;
    }
    spv::SpvBuildLogger logger;
    // SPIR-V version 1.0 = 0x0001'0000; user/generator number is arbitrary.
    spv::Builder builder(0x00010000u, 0u, &logger);
    builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    builder.addCapability(spv::CapabilityShader);
    std::vector<unsigned int> words;
    builder.dump(words);
    const uint32_t magic = words.empty() ? 0u : words[0];
    REXLOG_INFO("[highcut-P1] glslang spv::Builder OK: dumped {} SPIR-V words, magic=0x{:08X} "
                "(expect 0x07230203); SDK spirv_builder.h compiled against vendored glslang 14.3.0",
                uint32_t(words.size()), magic);

    // P-2b: prove the ported SpirvShaderTranslator constructs + links. Features(true)
    // selects the all-features (no Vulkan device) path; the ctor's later args default.
    namespace rg = rex::graphics;
    rg::SpirvShaderTranslator::Features features(/*all=*/true);
    rg::SpirvShaderTranslator translator(features,
                                         /*native_2x_msaa_with_attachments=*/false,
                                         /*native_2x_msaa_no_attachments=*/false,
                                         /*edram_fragment_shader_interlock=*/false);
    const uint64_t default_vs_mod = translator.GetDefaultVertexShaderModification(
        /*dynamic_addressable_register_count=*/0,
        rg::Shader::HostVertexShaderType::kVertex);
    REXLOG_INFO("[highcut-P2b] SpirvShaderTranslator linked: constructed OK, "
                "GetDefaultVertexShaderModification(0)=0x{:016X}",
                default_vs_mod);
}
