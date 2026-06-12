/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified  high-cut path C, P-3 - ported to ReXGlue/rex:: (namespace + include
 *            remap; bodies unchanged from Xenia 95a5c3e). Implements the SDK's
 *            rex::graphics::SpirvShader (header ships in the SDK, .cc does not),
 *            the Shader subclass the SpirvShaderTranslator translates into. See
 *            docs/highcut-c-plume-renderer-plan.md.
 */

#include <rex/graphics/pipeline/shader/spirv.h>

#include <cstring>

namespace rex::graphics {

SpirvShader::SpirvShader(xenos::ShaderType shader_type,
                         uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
                         size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : Shader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
             ucode_source_endian) {}

Shader::Translation* SpirvShader::CreateTranslationInstance(
    uint64_t modification) {
  return new SpirvTranslation(*this, modification);
}

}  // namespace rex::graphics
