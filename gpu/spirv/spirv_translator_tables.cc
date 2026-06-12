/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified  high-cut path C, P-2b - const data tables the SDK declares in its
 *            headers (rex/graphics/format/ucode.h, rex/graphics/util/draw.h) but
 *            whose definitions live in .cc files not exported from rexruntime.
 *            The ported SpirvShaderTranslator references them, so we supply the
 *            definitions here, copied verbatim from Xenia 95a5c3e (ucode.cc /
 *            draw_util.cc). The struct/array types are byte-identical to the SDK
 *            headers (verified). See docs/highcut-c-plume-renderer-plan.md.
 */

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/util/draw.h>

namespace rex::graphics {
namespace ucode {

const AluVectorOpcodeInfo kAluVectorOpcodeInfos[32] = {
    {"add", {0b1111, 0b1111}, kAluOpChangedStateNone},
    {"mul", {0b1111, 0b1111}, kAluOpChangedStateNone},
    {"max", {0b1111, 0b1111}, kAluOpChangedStateNone},
    {"min", {0b1111, 0b1111}, kAluOpChangedStateNone},
    {"seq", {0b1111, 0b1111}, kAluOpChangedStateNone},
    {"sgt", {0b1111, 0b1111}, kAluOpChangedStateNone},
    {"sge", {0b1111, 0b1111}, kAluOpChangedStateNone},
    {"sne", {0b1111, 0b1111}, kAluOpChangedStateNone},
    {"frc", {0b1111}, kAluOpChangedStateNone},
    {"trunc", {0b1111}, kAluOpChangedStateNone},
    {"floor", {0b1111}, kAluOpChangedStateNone},
    {"mad", {0b1111, 0b1111, 0b1111}, kAluOpChangedStateNone},
    {"cndeq", {0b1111, 0b1111, 0b1111}, kAluOpChangedStateNone},
    {"cndge", {0b1111, 0b1111, 0b1111}, kAluOpChangedStateNone},
    {"cndgt", {0b1111, 0b1111, 0b1111}, kAluOpChangedStateNone},
    {"dp4", {0b1111, 0b1111}, kAluOpChangedStateNone},
    {"dp3", {0b0111, 0b0111}, kAluOpChangedStateNone},
    {"dp2add", {0b0011, 0b0011, 0b0001}, kAluOpChangedStateNone},
    {"cube", {0b1111, 0b1111}, kAluOpChangedStateNone},
    {"max4", {0b1111}, kAluOpChangedStateNone},
    {"setp_eq_push", {0b1001, 0b1001}, kAluOpChangedStatePredicate},
    {"setp_ne_push", {0b1001, 0b1001}, kAluOpChangedStatePredicate},
    {"setp_gt_push", {0b1001, 0b1001}, kAluOpChangedStatePredicate},
    {"setp_ge_push", {0b1001, 0b1001}, kAluOpChangedStatePredicate},
    {"kill_eq", {0b1111, 0b1111}, kAluOpChangedStatePixelKill},
    {"kill_gt", {0b1111, 0b1111}, kAluOpChangedStatePixelKill},
    {"kill_ge", {0b1111, 0b1111}, kAluOpChangedStatePixelKill},
    {"kill_ne", {0b1111, 0b1111}, kAluOpChangedStatePixelKill},
    {"dst", {0b0110, 0b1010}, kAluOpChangedStateNone},
    {"maxa", {0b1111, 0b1111}, kAluOpChangedStateAddressRegister},
    {"opcode_30", {}, kAluOpChangedStateNone},
    {"opcode_31", {}, kAluOpChangedStateNone},
};

}  // namespace ucode

namespace draw_util {

const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
const int8_t kD3D10StandardSamplePositions4x[4][2] = {
    {-2, -6}, {6, -2}, {-6, 2}, {2, 6}};

}  // namespace draw_util
}  // namespace rex::graphics
