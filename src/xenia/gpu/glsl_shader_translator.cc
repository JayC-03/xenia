/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/glsl_shader_translator.h"

namespace xe {
namespace gpu {

using namespace xe::gpu::ucode;

constexpr int kMaxInterpolators = 16;
constexpr int kMaxTemporaryRegisters = 64;

#define EmitSource(...) source_.AppendFormat(__VA_ARGS__)
#define EmitSourceDepth(...)     \
  source_.Append("  ");          \
  source_.Append(depth_prefix_); \
  source_.AppendFormat(__VA_ARGS__)

const char* GetVertexFormatTypeName(VertexFormat format) {
  switch (format) {
    case VertexFormat::k_32:
    case VertexFormat::k_32_FLOAT:
      return "float";
    case VertexFormat::k_16_16:
    case VertexFormat::k_32_32:
    case VertexFormat::k_16_16_FLOAT:
    case VertexFormat::k_32_32_FLOAT:
      return "vec2";
    case VertexFormat::k_10_11_11:
    case VertexFormat::k_11_11_10:
    case VertexFormat::k_32_32_32_FLOAT:
      return "vec3";
    case VertexFormat::k_8_8_8_8:
    case VertexFormat::k_2_10_10_10:
    case VertexFormat::k_16_16_16_16:
    case VertexFormat::k_32_32_32_32:
    case VertexFormat::k_16_16_16_16_FLOAT:
    case VertexFormat::k_32_32_32_32_FLOAT:
      return "vec4";
    default:
      assert_always();
      return "vec4";
  }
}

GlslShaderTranslator::GlslShaderTranslator(Dialect dialect)
    : dialect_(dialect) {}

GlslShaderTranslator::~GlslShaderTranslator() = default;

void GlslShaderTranslator::Reset() {
  ShaderTranslator::Reset();
  depth_ = 0;
  depth_prefix_[0] = 0;
  source_.Reset();
}

void GlslShaderTranslator::EmitTranslationError(const char* message) {
  ShaderTranslator::EmitTranslationError(message);
  EmitSourceDepth("// TRANSLATION ERROR: %s\n", message);
}

void GlslShaderTranslator::EmitUnimplementedTranslationError() {
  ShaderTranslator::EmitUnimplementedTranslationError();
  EmitSourceDepth("// UNIMPLEMENTED TRANSLATION\n");
}

void GlslShaderTranslator::Indent() {
  depth_prefix_[depth_] = ' ';
  depth_prefix_[depth_ + 1] = ' ';
  depth_prefix_[depth_ + 2] = 0;
  depth_ += 2;
}

void GlslShaderTranslator::Unindent() {
  depth_prefix_[depth_] = 0;
  depth_prefix_[depth_ - 1] = 0;
  depth_prefix_[depth_ - 2] = 0;
  depth_ -= 2;
}

void GlslShaderTranslator::StartTranslation() {
  // Tons of boilerplate for shaders, here.
  // We have a large amount of shared state defining uniforms and some common
  // utility functions used in both vertex and pixel shaders.
  EmitSource(R"(
#version 450
#extension all : warn
#extension GL_ARB_bindless_texture : require
#extension GL_ARB_explicit_uniform_location : require
#extension GL_ARB_shader_draw_parameters : require
#extension GL_ARB_shader_storage_buffer_object : require
#extension GL_ARB_shading_language_420pack : require
#extension GL_ARB_fragment_coord_conventions : require
#define FLT_MAX 3.402823466e+38
precision highp float;
precision highp int;
layout(std140, column_major) uniform;
layout(std430, column_major) buffer;

// This must match DrawBatcher::CommonHeader.
struct StateData {
  vec4 window_scale;
  vec4 vtx_fmt;
  vec4 alpha_test;
  // TODO(benvanik): variable length.
  uvec2 texture_samplers[32];
  vec4 float_consts[512];
  int bool_consts[8];
  int loop_consts[32];
};
layout(binding = 0) buffer State {
  StateData states[];
};

struct VertexData {
  vec4 o[16];
};
)");

  // http://www.nvidia.com/object/cube_map_ogl_tutorial.html
  // http://developer.amd.com/wordpress/media/2012/10/R600_Instruction_Set_Architecture.pdf
  // src0 = Rn.zzxy, src1 = Rn.yxzz
  // dst.W = FaceId;
  // dst.Z = 2.0f * MajorAxis;
  // dst.Y = S cube coordinate;
  // dst.X = T cube coordinate;
  /*
  major axis
  direction     target                                sc     tc    ma
  ----------   ------------------------------------   ---    ---   ---
  +rx          GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT=0   -rz    -ry   rx
  -rx          GL_TEXTURE_CUBE_MAP_NEGATIVE_X_EXT=1   +rz    -ry   rx
  +ry          GL_TEXTURE_CUBE_MAP_POSITIVE_Y_EXT=2   +rx    +rz   ry
  -ry          GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_EXT=3   +rx    -rz   ry
  +rz          GL_TEXTURE_CUBE_MAP_POSITIVE_Z_EXT=4   +rx    -ry   rz
  -rz          GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_EXT=5   -rx    -ry   rz
  */
  EmitSource(R"(
vec4 cube(vec4 src0, vec4 src1) {
  vec3 src = vec3(src1.y, src1.x, src1.z);
  vec3 abs_src = abs(src);
  int face_id;
  float sc;
  float tc;
  float ma;
  if (abs_src.x > abs_src.y && abs_src.x > abs_src.z) {
    if (src.x > 0.0) {
      face_id = 0; sc = -abs_src.z; tc = -abs_src.y; ma = abs_src.x;
    } else {
      face_id = 1; sc =  abs_src.z; tc = -abs_src.y; ma = abs_src.x;
    }
  } else if (abs_src.y > abs_src.x && abs_src.y > abs_src.z) {
    if (src.y > 0.0) {
      face_id = 2; sc =  abs_src.x; tc =  abs_src.z; ma = abs_src.y;
    } else {
      face_id = 3; sc =  abs_src.x; tc = -abs_src.z; ma = abs_src.y;
    }
  } else {
    if (src.z > 0.0) {
      face_id = 4; sc =  abs_src.x; tc = -abs_src.y; ma = abs_src.z;
    } else {
      face_id = 5; sc = -abs_src.x; tc = -abs_src.y; ma = abs_src.z;
    }
  }
  float s = (sc / ma + 1.0) / 2.0;
  float t = (tc / ma + 1.0) / 2.0;
  return vec4(t, s, 2.0 * ma, float(face_id));
};
)");

  if (is_vertex_shader()) {
    EmitSource(R"(
out gl_PerVertex {
  vec4 gl_Position;
  float gl_PointSize;
  float gl_ClipDistance[];
};
layout(location = 0) flat out uint draw_id;
layout(location = 1) out VertexData vtx;
vec4 applyTransform(const in StateData state, vec4 pos) {
  if (state.vtx_fmt.w == 0.0) {
    // w is 1/W0, so fix it.
    pos.w = 1.0 / pos.w;
  }
  if (state.vtx_fmt.x != 0.0) {
    // Already multiplied by 1/W0, so pull it out.
    pos.xy /= pos.w;
  }
  if (state.vtx_fmt.z != 0.0) {
    // Already multiplied by 1/W0, so pull it out.
    pos.z /= pos.w;
  }
  pos.xy *= state.window_scale.xy;
  return pos;
};
void processVertex(const in StateData state);
void main() {
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
  gl_PointSize = 1.0;
  for (int i = 0; i < vtx.o.length(); ++i) {
    vtx.o[i] = vec4(0.0, 0.0, 0.0, 0.0);
  }
  const StateData state = states[gl_DrawIDARB];
  processVertex(state);
  gl_Position = applyTransform(state, gl_Position);
  draw_id = gl_DrawIDARB;
}
)");
  } else {
    EmitSource(R"(
layout(origin_upper_left, pixel_center_integer) in vec4 gl_FragCoord;
layout(location = 0) flat in uint draw_id;
layout(location = 1) in VertexData vtx;
layout(location = 0) out vec4 oC[4];
void applyAlphaTest(int alpha_func, float alpha_ref) {
  bool passes = false;
  switch (alpha_func) {
  case 0:                                          break;
  case 1: if (oC[0].a <  alpha_ref) passes = true; break;
  case 2: if (oC[0].a == alpha_ref) passes = true; break;
  case 3: if (oC[0].a <= alpha_ref) passes = true; break;
  case 4: if (oC[0].a >  alpha_ref) passes = true; break;
  case 5: if (oC[0].a != alpha_ref) passes = true; break;
  case 6: if (oC[0].a >= alpha_ref) passes = true; break;
  case 7:                           passes = true; break;
  };
  if (!passes) discard;
}
void processFragment(const in StateData state);
void main() {
  const StateData state = states[draw_id];
  processFragment(state);
  if (state.alpha_test.x != 0.0) {
    applyAlphaTest(int(state.alpha_test.y), state.alpha_test.z);
  }
}
)");
  }

  // Add vertex shader input declarations.
  if (is_vertex_shader()) {
    for (auto& binding : vertex_bindings()) {
      for (auto& attrib : binding.attributes) {
        const char* type_name =
            GetVertexFormatTypeName(attrib.fetch_instr.attributes.data_format);
        EmitSource("layout(location = %d) in %s vf%u_%d;\n",
                   attrib.attrib_index, type_name, binding.fetch_constant,
                   attrib.fetch_instr.attributes.offset);
      }
    }
  }

  // Enter the main function, where all of our shader lives.
  if (is_vertex_shader()) {
    EmitSource("void processVertex(const in StateData state) {\n");

    // Temporary registers.
    EmitSource("  vec4 r[64];\n");
  } else {
    EmitSource("void processFragment(const in StateData state) {\n");

    // Bring interpolators from vertex shader into temporary registers.
    EmitSource("  vec4 r[64];\n");
    for (int i = 0; i < kMaxInterpolators; ++i) {
      EmitSource("  r[%d] = vtx.o[%d];\n", i, i);
    }
  }

  // Previous Vector result (used as a scratch).
  EmitSource("  vec4 pv;\n");
  // Previous Scalar result (used for RETAIN_PREV).
  EmitSource("  float ps;\n");
  // Predicate temp, clause-local.
  EmitSource("  bool p0 = false;\n");
  // Address register when using absolute addressing.
  EmitSource("  int a0 = 0;\n");
  // Temps for source register values.
  EmitSource("  vec4 src0;\n");
  EmitSource("  vec4 src1;\n");
  EmitSource("  vec4 src2;\n");
}

std::vector<uint8_t> GlslShaderTranslator::CompleteTranslation() {
  // End of process*() function.
  EmitSource("}\n");

  return source_.ToBytes();
}

void GlslShaderTranslator::ProcessLabel(uint32_t cf_index) {
  EmitUnimplementedTranslationError();
}

void GlslShaderTranslator::ProcessControlFlowNopInstruction() {
  EmitSource("//        cnop\n");
}

void GlslShaderTranslator::ProcessExecInstructionBegin(
    const ParsedExecInstruction& instr) {
  EmitSource("// ");
  instr.Disassemble(&source_);

  switch (instr.type) {
    case ParsedExecInstruction::Type::kUnconditional:
      EmitSourceDepth("{\n");
      break;
    case ParsedExecInstruction::Type::kConditional:
      EmitSourceDepth("if ((state.bool_consts[%d] & (1 << %d)) == %c) {\n",
                      instr.bool_constant_index / 32,
                      instr.bool_constant_index % 32,
                      instr.condition ? '1' : '0');
      break;
    case ParsedExecInstruction::Type::kPredicated:
      EmitSourceDepth("if (%cp0) {\n", instr.condition ? ' ' : '!');
      break;
  }
  Indent();
}

void GlslShaderTranslator::ProcessExecInstructionEnd(
    const ParsedExecInstruction& instr) {
  Unindent();
  EmitSourceDepth("}\n");
}

void GlslShaderTranslator::ProcessLoopStartInstruction(
    const ParsedLoopStartInstruction& instr) {
  EmitSource("// ");
  instr.Disassemble(&source_);

  EmitUnimplementedTranslationError();
}

void GlslShaderTranslator::ProcessLoopEndInstruction(
    const ParsedLoopEndInstruction& instr) {
  EmitSource("// ");
  instr.Disassemble(&source_);

  EmitUnimplementedTranslationError();
}

void GlslShaderTranslator::ProcessCallInstruction(
    const ParsedCallInstruction& instr) {
  EmitSource("// ");
  instr.Disassemble(&source_);

  EmitUnimplementedTranslationError();
}

void GlslShaderTranslator::ProcessReturnInstruction(
    const ParsedReturnInstruction& instr) {
  EmitSource("// ");
  instr.Disassemble(&source_);

  EmitUnimplementedTranslationError();
}

void GlslShaderTranslator::ProcessJumpInstruction(
    const ParsedJumpInstruction& instr) {
  EmitSource("// ");
  instr.Disassemble(&source_);

  EmitUnimplementedTranslationError();
}

void GlslShaderTranslator::ProcessAllocInstruction(
    const ParsedAllocInstruction& instr) {
  EmitSource("// ");
  instr.Disassemble(&source_);
}

void GlslShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  EmitSource("// ");
  instr.Disassemble(&source_);

  if (instr.is_predicated) {
    EmitSourceDepth("if (%cp0) {\n", instr.predicate_condition ? ' ' : '!');
    Indent();
  }

  if (instr.result.stores_non_constants()) {
    for (size_t i = 0; i < instr.operand_count; ++i) {
      if (instr.operands[i].storage_source !=
          InstructionStorageSource::kVertexFetchConstant) {
        EmitLoadOperand(i, instr.operands[i]);
      }
    }

    switch (instr.opcode) {
      case FetchOpcode::kVertexFetch:
        EmitSourceDepth("pv.");
        for (int i = 0;
             i < GetVertexFormatComponentCount(instr.attributes.data_format);
             ++i) {
          EmitSource("%c", GetCharForComponentIndex(i));
        }
        EmitSource(" = vf%u_%d;\n", instr.operands[1].storage_index,
                   instr.attributes.offset);
        break;
    }
  }

  EmitStoreVectorResult(instr.result);

  if (instr.is_predicated) {
    Unindent();
    EmitSourceDepth("}\n");
  }
}

void GlslShaderTranslator::ProcessTextureFetchInstruction(
    const ParsedTextureFetchInstruction& instr) {
  EmitSource("// ");
  instr.Disassemble(&source_);

  if (instr.is_predicated) {
    EmitSourceDepth("if (%cp0) {\n", instr.predicate_condition ? ' ' : '!');
    Indent();
  }

  for (size_t i = 0; i < instr.operand_count; ++i) {
    if (instr.operands[i].storage_source !=
        InstructionStorageSource::kTextureFetchConstant) {
      EmitLoadOperand(i, instr.operands[i]);
    }
  }

  switch (instr.opcode) {
    case FetchOpcode::kTextureFetch:
      switch (instr.dimension) {
        case TextureDimension::k1D:
          EmitSourceDepth("if (state.texture_samplers[%d] != 0) {\n",
                          instr.operands[1].storage_index);
          EmitSourceDepth(
              "  pv = texture(sampler1D(state.texture_samplers[%d]), "
              "src0.x);\n",
              instr.operands[1].storage_index);
          EmitSourceDepth("} else {\n");
          EmitSourceDepth("  pv = vec4(src0.x, 0.0, 0.0, 1.0);\n");
          EmitSourceDepth("}\n");
          break;
        case TextureDimension::k2D:
          EmitSourceDepth("if (state.texture_samplers[%d] != 0) {\n",
                          instr.operands[1].storage_index);
          EmitSourceDepth(
              "  pv = texture(sampler2D(state.texture_samplers[%d]), "
              "src0.xy);\n",
              instr.operands[1].storage_index);
          EmitSourceDepth("} else {\n");
          EmitSourceDepth("  pv = vec4(src0.x, src0.y, 0.0, 1.0);\n");
          EmitSourceDepth("}\n");
          break;
        case TextureDimension::k3D:
          EmitSourceDepth("if (state.texture_samplers[%d] != 0) {\n",
                          instr.operands[1].storage_index);
          EmitSourceDepth(
              "  pv = texture(sampler3D(state.texture_samplers[%d]), "
              "src0.xyz);\n",
              instr.operands[1].storage_index);
          EmitSourceDepth("} else {\n");
          EmitSourceDepth("  pv = vec4(src0.x, src0.y, src0.z, 1.0);\n");
          EmitSourceDepth("}\n");
          break;
        case TextureDimension::kCube:
          // TODO(benvanik): undo CUBEv logic on t? (s,t,faceid)
          EmitSourceDepth("if (state.texture_samplers[%d] != 0) {\n",
                          instr.operands[1].storage_index);
          EmitSourceDepth(
              "  pv = texture(samplerCube(state.texture_samplers[%d]), "
              "src0.xyz);\n",
              instr.operands[1].storage_index);
          EmitSourceDepth("} else {\n");
          EmitSourceDepth("  pv = vec4(src0.x, src0.y, src0.z, 1.0);\n");
          EmitSourceDepth("}\n");
          break;
      }
      break;
    case FetchOpcode::kGetTextureBorderColorFrac:
      EmitUnimplementedTranslationError();
      EmitSourceDepth("pv = vec4(0.0);\n");
      break;
    case FetchOpcode::kGetTextureComputedLod:
      EmitUnimplementedTranslationError();
      EmitSourceDepth("pv = vec4(0.0);\n");
      break;
    case FetchOpcode::kGetTextureGradients:
      EmitUnimplementedTranslationError();
      EmitSourceDepth("pv = vec4(0.0);\n");
      break;
    case FetchOpcode::kGetTextureWeights:
      EmitUnimplementedTranslationError();
      EmitSourceDepth("pv = vec4(0.0);\n");
      break;
    case FetchOpcode::kSetTextureLod:
      EmitUnimplementedTranslationError();
      break;
    case FetchOpcode::kSetTextureGradientsHorz:
      EmitUnimplementedTranslationError();
      break;
    case FetchOpcode::kSetTextureGradientsVert:
      EmitUnimplementedTranslationError();
      break;
    case FetchOpcode::kUnknownTextureOp:
      EmitUnimplementedTranslationError();
      EmitSourceDepth("pv = vec4(0.0);\n");
      break;
  }

  EmitStoreVectorResult(instr.result);

  if (instr.is_predicated) {
    Unindent();
    EmitSourceDepth("}\n");
  }
}

void GlslShaderTranslator::ProcessAluInstruction(
    const ParsedAluInstruction& instr) {
  EmitSource("// ");
  instr.Disassemble(&source_);

  switch (instr.type) {
    case ParsedAluInstruction::Type::kNop:
      break;
    case ParsedAluInstruction::Type::kVector:
      ProcessVectorAluInstruction(instr);
      break;
    case ParsedAluInstruction::Type::kScalar:
      ProcessScalarAluInstruction(instr);
      break;
  }
}

void GlslShaderTranslator::EmitLoadOperand(size_t i,
                                           const InstructionOperand& op) {
  EmitSourceDepth("src%d = ", i);
  if (op.is_negated) {
    EmitSource("-");
  }
  if (op.is_absolute_value) {
    EmitSource("abs(");
  }
  int storage_index_offset = 0;
  bool has_components = true;
  switch (op.storage_source) {
    case InstructionStorageSource::kRegister:
      EmitSource("r");
      break;
    case InstructionStorageSource::kConstantFloat:
      storage_index_offset = is_pixel_shader() ? 256 : 0;
      EmitSource("state.float_consts");
      break;
    case InstructionStorageSource::kConstantInt:
      EmitSource("state.loop_consts");
      break;
    case InstructionStorageSource::kConstantBool:
      EmitSource("state.bool_consts");
      break;
  }
  switch (op.storage_addressing_mode) {
    case InstructionStorageAddressingMode::kStatic:
      if (storage_index_offset) {
        EmitSource("[%d+%d]", storage_index_offset, op.storage_index);
      } else {
        EmitSource("[%d]", op.storage_index);
      }
      break;
    case InstructionStorageAddressingMode::kAddressAbsolute:
      if (storage_index_offset) {
        EmitSource("[%d+%d+a0]", storage_index_offset, op.storage_index);
      } else {
        EmitSource("[%d+a0]", op.storage_index);
      }
      break;
    case InstructionStorageAddressingMode::kAddressRelative:
      if (storage_index_offset) {
        EmitSource("[%d+%d+aL]", storage_index_offset, op.storage_index);
      } else {
        EmitSource("[%d+aL]", op.storage_index);
      }
      break;
  }
  if (op.is_absolute_value) {
    EmitSource(")");
  }
  if (!op.is_standard_swizzle()) {
    EmitSource(".");
    if (op.component_count == 1) {
      char a = GetCharForSwizzle(op.components[0]);
      EmitSource("%c%c%c%c", a, a, a, a);
    } else if (op.component_count == 2) {
      char a = GetCharForSwizzle(op.components[0]);
      char b = GetCharForSwizzle(op.components[1]);
      EmitSource("%c%c%c%c", a, b, b, b);
    } else {
      for (int j = 0; j < op.component_count; ++j) {
        EmitSource("%c", GetCharForSwizzle(op.components[j]));
      }
      for (int j = op.component_count; j < 4; ++j) {
        EmitSource("%c",
                   GetCharForSwizzle(op.components[op.component_count - 1]));
      }
    }
  }
  EmitSource(";\n");
}

void GlslShaderTranslator::EmitStoreVectorResult(
    const InstructionResult& result) {
  EmitStoreResult(result, "pv");
}

void GlslShaderTranslator::EmitStoreScalarResult(
    const InstructionResult& result) {
  EmitStoreResult(result, "vec4(ps)");
}

void GlslShaderTranslator::EmitStoreResult(const InstructionResult& result,
                                           const char* temp) {
  if (!result.has_any_writes()) {
    return;
  }
  bool uses_storage_index = false;
  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      EmitSourceDepth("r");
      uses_storage_index = true;
      break;
    case InstructionStorageTarget::kInterpolant:
      EmitSourceDepth("vtx.o");
      uses_storage_index = true;
      break;
    case InstructionStorageTarget::kPosition:
      EmitSourceDepth("gl_Position");
      break;
    case InstructionStorageTarget::kPointSize:
      EmitSourceDepth("gl_PointSize");
      break;
    case InstructionStorageTarget::kColorTarget:
      EmitSourceDepth("oC");
      uses_storage_index = true;
      break;
    case InstructionStorageTarget::kDepth:
      EmitSourceDepth("gl_FragDepth");
      break;
  }
  if (uses_storage_index) {
    switch (result.storage_addressing_mode) {
      case InstructionStorageAddressingMode::kStatic:
        EmitSource("[%d]", result.storage_index);
        break;
      case InstructionStorageAddressingMode::kAddressAbsolute:
        EmitSource("[%d+a0]", result.storage_index);
        break;
      case InstructionStorageAddressingMode::kAddressRelative:
        EmitSource("[%d+aL]", result.storage_index);
        break;
    }
  }
  bool has_const_writes = false;
  int component_write_count = 0;
  if (!result.is_standard_swizzle()) {
    EmitSource(".");
    for (int j = 0; j < 4; ++j) {
      if (result.write_mask[j]) {
        if (result.components[j] == SwizzleSource::k0 ||
            result.components[j] == SwizzleSource::k1) {
          has_const_writes = true;
        }
        ++component_write_count;
        EmitSource("%c", GetCharForSwizzle(GetSwizzleFromComponentIndex(j)));
      }
    }
  }
  EmitSource(" = ");
  if (result.is_clamped) {
    EmitSource("clamp(");
  }
  if (has_const_writes) {
    EmitSource("vec%d(", component_write_count);
    bool has_written = false;
    for (int j = 0; j < 4; ++j) {
      if (result.write_mask[j]) {
        if (has_written) {
          EmitSource(", ");
        }
        has_written = true;
        switch (result.components[j]) {
          case SwizzleSource::k0:
            EmitSource("0.0");
            break;
          case SwizzleSource::k1:
            EmitSource("1.0");
            break;
          default:
            EmitSource("%s.%c", temp, GetCharForSwizzle(result.components[j]));
            break;
        }
      }
    }
    EmitSource(")");
  } else {
    EmitSource(temp);
    if (!result.is_standard_swizzle()) {
      EmitSource(".");
      for (int j = 0; j < 4; ++j) {
        if (result.write_mask[j]) {
          EmitSource("%c", GetCharForSwizzle(result.components[j]));
        }
      }
    }
  }
  if (result.is_clamped) {
    EmitSource(", 0.0, 1.0)");
  }
  EmitSource(";\n");
}

void GlslShaderTranslator::ProcessVectorAluInstruction(
    const ParsedAluInstruction& instr) {
  if (instr.is_predicated) {
    EmitSourceDepth("if (%cp0) {\n", instr.predicate_condition ? ' ' : '!');
    Indent();
  }

  for (size_t i = 0; i < instr.operand_count; ++i) {
    EmitLoadOperand(i, instr.operands[i]);
  }

  switch (instr.vector_opcode) {
    // add dest, src0, src1
    case AluVectorOpcode::kAdd:
      EmitSourceDepth("pv = src0 + src1;\n");
      break;

    // mul dest, src0, src1
    case AluVectorOpcode::kMul:
      EmitSourceDepth("pv = src0 * src1;\n");
      break;

    // max dest, src0, src1
    case AluVectorOpcode::kMax:
      EmitSourceDepth("pv = max(src0, src1);\n");
      break;

    // min dest, src0, src1
    case AluVectorOpcode::kMin:
      EmitSourceDepth("pv = min(src0, src1);\n");
      break;

    // seq dest, src0, src1
    case AluVectorOpcode::kSeq:
      EmitSourceDepth("pv.x = src0.x == src1.x ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.y = src0.y == src1.y ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.z = src0.z == src1.z ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.w = src0.w == src1.w ? 1.0 : 0.0;\n");
      break;

    // sgt dest, src0, src1
    case AluVectorOpcode::kSgt:
      EmitSourceDepth("pv.x = src0.x > src1.x ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.y = src0.y > src1.y ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.z = src0.z > src1.z ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.w = src0.w > src1.w ? 1.0 : 0.0;\n");
      break;

    // sge dest, src0, src1
    case AluVectorOpcode::kSge:
      EmitSourceDepth("pv.x = src0.x >= src1.x ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.y = src0.y >= src1.y ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.z = src0.z >= src1.z ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.w = src0.w >= src1.w ? 1.0 : 0.0;\n");
      break;

    // sne dest, src0, src1
    case AluVectorOpcode::kSne:
      EmitSourceDepth("pv.x = src0.x != src1.x ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.y = src0.y != src1.y ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.z = src0.z != src1.z ? 1.0 : 0.0;\n");
      EmitSourceDepth("pv.w = src0.w != src1.w ? 1.0 : 0.0;\n");
      break;

    // frc dest, src0
    case AluVectorOpcode::kFrc:
      EmitSourceDepth("pv = fract(src0);\n");
      break;

    // trunc dest, src0
    case AluVectorOpcode::kTrunc:
      EmitSourceDepth("pv = trunc(src0);\n");
      break;

    // floor dest, src0
    case AluVectorOpcode::kFloor:
      EmitSourceDepth("pv = floor(src0);\n");
      break;

    // mad dest, src0, src1, src2
    case AluVectorOpcode::kMad:
      EmitSourceDepth("pv = (src0 * src1) + src2;\n");
      break;

    // cndeq dest, src0, src1, src2
    case AluVectorOpcode::kCndEq:
      EmitSourceDepth("pv.x = src0.x == 0.0 ? src1.x : src2.x;\n");
      EmitSourceDepth("pv.y = src0.y == 0.0 ? src1.y : src2.y;\n");
      EmitSourceDepth("pv.z = src0.z == 0.0 ? src1.z : src2.z;\n");
      EmitSourceDepth("pv.w = src0.w == 0.0 ? src1.w : src2.w;\n");
      break;

    // cndge dest, src0, src1, src2
    case AluVectorOpcode::kCndGe:
      EmitSourceDepth("pv.x = src0.x >= 0.0 ? src1.x : src2.x;\n");
      EmitSourceDepth("pv.y = src0.y >= 0.0 ? src1.y : src2.y;\n");
      EmitSourceDepth("pv.z = src0.z >= 0.0 ? src1.z : src2.z;\n");
      EmitSourceDepth("pv.w = src0.w >= 0.0 ? src1.w : src2.w;\n");
      break;

    // cndgt dest, src0, src1, src2
    case AluVectorOpcode::kCndGt:
      EmitSourceDepth("pv.x = src0.x > 0.0 ? src1.x : src2.x;\n");
      EmitSourceDepth("pv.y = src0.y > 0.0 ? src1.y : src2.y;\n");
      EmitSourceDepth("pv.z = src0.z > 0.0 ? src1.z : src2.z;\n");
      EmitSourceDepth("pv.w = src0.w > 0.0 ? src1.w : src2.w;\n");
      break;

    // dp4 dest, src0, src1
    case AluVectorOpcode::kDp4:
      EmitSourceDepth("pv = dot(src0, src1).xxxx;\n");
      break;

    // dp3 dest, src0, src1
    case AluVectorOpcode::kDp3:
      EmitSourceDepth("pv = dot(vec4(src0).xyz, vec4(src1).xyz).xxxx;\n");
      break;

    // dp2add dest, src0, src1, src2
    case AluVectorOpcode::kDp2Add:
      EmitSourceDepth(
          "pv = vec4(src0.x * src1.x + src0.y * src1.y + src2.x).xxxx;\n");
      break;

    // cube dest, src0, src1
    case AluVectorOpcode::kCube:
      EmitSourceDepth("pv = cube(src0, src1);\n");
      break;

    // max4 dest, src0
    case AluVectorOpcode::kMax4:
      EmitSourceDepth(
          "pv = max(src0.x, max(src0.y, max(src0.z, src0.w))).xxxx;\n");
      break;

    // setp_eq_push dest, src0, src1
    case AluVectorOpcode::kSetpEqPush:
      EmitSourceDepth("p0 = src0.w == 0.0 && src1.w == 0.0 ? true : false;\n");
      EmitSourceDepth(
          "pv = vec4(src0.x == 0.0 && src1.x == 0.0 ? 0.0 : src0.x + 1.0);\n");
      break;

    // setp_ne_push dest, src0, src1
    case AluVectorOpcode::kSetpNePush:
      EmitSourceDepth("p0 = src0.w == 0.0 && src1.w != 0.0 ? true : false;\n");
      EmitSourceDepth(
          "pv = vec4(src0.x == 0.0 && src1.x != 0.0 ? 0.0 : src0.x + 1.0);\n");
      break;

    // setp_gt_push dest, src0, src1
    case AluVectorOpcode::kSetpGtPush:
      EmitSourceDepth("p0 = src0.w == 0.0 && src1.w > 0.0 ? true : false;\n");
      EmitSourceDepth(
          "pv = vec4(src0.x == 0.0 && src1.x > 0.0 ? 0.0 : src0.x + 1.0);\n");
      break;

    // setp_ge_push dest, src0, src1
    case AluVectorOpcode::kSetpGePush:
      EmitSourceDepth("p0 = src0.w == 0.0 && src1.w >= 0.0 ? true : false;\n");
      EmitSourceDepth(
          "pv = vec4(src0.x == 0.0 && src1.x >= 0.0 ? 0.0 : src0.x + 1.0);\n");
      break;

    // kill_eq dest, src0, src1
    case AluVectorOpcode::kKillEq:
      EmitSourceDepth(
          "if (src0.x == src1.x || src0.y == src1.y || src0.z == src1.z || "
          "src0.w == src1.w) {\n");
      EmitSourceDepth("  pv = vec4(1.0);\n");
      EmitSourceDepth("  discard;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  pv = vec4(0.0);\n");
      EmitSourceDepth("}\n");
      break;

    // kill_gt dest, src0, src1
    case AluVectorOpcode::kKillGt:
      EmitSourceDepth(
          "if (src0.x > src1.x || src0.y > src1.y || src0.z > src1.z || "
          "src0.w > src1.w) {\n");
      EmitSourceDepth("  pv = vec4(1.0);\n");
      EmitSourceDepth("  discard;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  pv = vec4(0.0);\n");
      EmitSourceDepth("}\n");
      break;

    // kill_ge dest, src0, src1
    case AluVectorOpcode::kKillGe:
      EmitSourceDepth(
          "if (src0.x >= src1.x || src0.y >= src1.y || src0.z >= src1.z || "
          "src0.w >= src1.w) {\n");
      EmitSourceDepth("  pv = vec4(1.0);\n");
      EmitSourceDepth("  discard;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  pv = vec4(0.0);\n");
      EmitSourceDepth("}\n");
      break;

    // kill_ne dest, src0, src1
    case AluVectorOpcode::kKillNe:
      EmitSourceDepth(
          "if (src0.x != src1.x || src0.y != src1.y || src0.z != src1.z || "
          "src0.w != src1.w) {\n");
      EmitSourceDepth("  pv = vec4(1.0);\n");
      EmitSourceDepth("  discard;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  pv = vec4(0.0);\n");
      EmitSourceDepth("}\n");
      break;

    // dst dest, src0, src1
    case AluVectorOpcode::kDst:
      EmitSourceDepth("pv.x = 1.0;\n");
      EmitSourceDepth("pv.y = src0.y * src1.y;\n");
      EmitSourceDepth("pv.z = src0.z;\n");
      EmitSourceDepth("pv.w = src1.w;\n");
      break;

    // maxa dest, src0, src1
    case AluVectorOpcode::kMaxA:
      EmitSourceDepth("a0 = clamp(int(floor(src0.w + 0.5)), -256, 255);\n");
      EmitSourceDepth("pv = max(src0, src1);\n");
      break;
  }

  EmitStoreVectorResult(instr.result);

  if (instr.is_predicated) {
    Unindent();
    EmitSourceDepth("}\n");
  }
}

void GlslShaderTranslator::ProcessScalarAluInstruction(
    const ParsedAluInstruction& instr) {
  if (instr.is_predicated) {
    EmitSourceDepth("if (%cp0) {\n", instr.predicate_condition ? ' ' : '!');
    Indent();
  }

  for (size_t i = 0; i < instr.operand_count; ++i) {
    EmitLoadOperand(i, instr.operands[i]);
  }

  switch (instr.scalar_opcode) {
    // adds dest, src0.ab
    case AluScalarOpcode::kAdds:
      EmitSourceDepth("ps = src0.x + src0.y;\n");
      break;

    // adds_prev dest, src0.a
    case AluScalarOpcode::kAddsPrev:
      EmitSourceDepth("ps = src0.x + ps;\n");
      break;

    // muls dest, src0.ab
    case AluScalarOpcode::kMuls:
      EmitSourceDepth("ps = src0.x * src0.y;\n");
      break;

    // muls_prev dest, src0.a
    case AluScalarOpcode::kMulsPrev:
      EmitSourceDepth("ps = src0.x * ps;\n");
      break;

    // muls_prev2 dest, src0.ab
    case AluScalarOpcode::kMulsPrev2:
      EmitSourceDepth(
          "ps = ps == -FLT_MAX || isinf(ps) || isnan(ps) || isnan(src0.y) || "
          "src0.y <= 0.0 ? -FLT_MAX : src0.x * ps;\n");
      break;

    // maxs dest, src0.ab
    case AluScalarOpcode::kMaxs:
      EmitSourceDepth("ps = max(src0.x, src0.y);\n");
      break;

    // mins dest, src0.ab
    case AluScalarOpcode::kMins:
      EmitSourceDepth("ps = min(src0.x, src0.y);\n");
      break;

    // seqs dest, src0.a
    case AluScalarOpcode::kSeqs:
      EmitSourceDepth("ps = src0.x == 0.0 ? 1.0 : 0.0;\n");
      break;

    // sgts dest, src0.a
    case AluScalarOpcode::kSgts:
      EmitSourceDepth("ps = src0.x > 0.0 ? 1.0 : 0.0;\n");
      break;

    // sges dest, src0.a
    case AluScalarOpcode::kSges:
      EmitSourceDepth("ps = src0.x >= 0.0 ? 1.0 : 0.0;\n");
      break;

    // snes dest, src0.a
    case AluScalarOpcode::kSnes:
      EmitSourceDepth("ps = src0.x != 0.0 ? 1.0 : 0.0;\n");
      break;

    // frcs dest, src0.a
    case AluScalarOpcode::kFrcs:
      EmitSourceDepth("ps = fract(src0.x);\n");
      break;

    // truncs dest, src0.a
    case AluScalarOpcode::kTruncs:
      EmitSourceDepth("ps = trunc(src0.x);\n");
      break;

    // floors dest, src0.a
    case AluScalarOpcode::kFloors:
      EmitSourceDepth("ps = floor(src0.x);\n");
      break;

    // exp dest, src0.a
    case AluScalarOpcode::kExp:
      EmitSourceDepth("ps = exp2(src0.x);\n");
      break;

    // logc dest, src0.a
    case AluScalarOpcode::kLogc:
      EmitSourceDepth("ps = log2(src0.x);\n");
      EmitSourceDepth("ps = isinf(ps) ? -FLT_MAX : ps;\n");
      break;

    // log dest, src0.a
    case AluScalarOpcode::kLog:
      EmitSourceDepth("ps = log2(src0.x);\n");
      break;

    // rcpc dest, src0.a
    case AluScalarOpcode::kRcpc:
      EmitSourceDepth("ps = 1.0 / src0.x;\n");
      EmitSourceDepth("if (isinf(ps)) ps = FLT_MAX;\n");
      break;

    // rcpf dest, src0.a
    case AluScalarOpcode::kRcpf:
      EmitSourceDepth("ps = 1.0 / src0.x;\n");
      EmitSourceDepth("if (isinf(ps)) ps = 0.0;\n");
      break;

    // rcp dest, src0.a
    case AluScalarOpcode::kRcp:
      EmitSourceDepth("ps = 1.0 / src0.x;\n");
      break;

    // rsqc dest, src0.a
    case AluScalarOpcode::kRsqc:
      EmitSourceDepth("ps = inversesqrt(src0.x);\n");
      EmitSourceDepth("if (isinf(ps)) ps = FLT_MAX;\n");
      break;

    // rsqc dest, src0.a
    case AluScalarOpcode::kRsqf:
      EmitSourceDepth("ps = inversesqrt(src0.x);\n");
      EmitSourceDepth("if (isinf(ps)) ps = 0.0;\n");
      break;

    // rsq dest, src0.a
    case AluScalarOpcode::kRsq:
      EmitSourceDepth("ps = inversesqrt(src0.x);\n");
      break;

    // maxas dest, src0.ab
    // movas dest, src0.aa
    case AluScalarOpcode::kMaxAs:
      EmitSourceDepth("a0 = clamp(int(floor(src0.x + 0.5)), -256, 255);\n");
      EmitSourceDepth("ps = max(src0.x, src0.y);\n");
      break;

    // maxasf dest, src0.ab
    // movasf dest, src0.aa
    case AluScalarOpcode::kMaxAsf:
      EmitSourceDepth("a0 = clamp(int(floor(src0.x)), -256, 255);\n");
      EmitSourceDepth("ps = max(src0.x, src0.y);\n");
      break;

    // subs dest, src0.ab
    case AluScalarOpcode::kSubs:
      EmitSourceDepth("ps = src0.x - src0.y;\n");
      break;

    // subs_prev dest, src0.a
    case AluScalarOpcode::kSubsPrev:
      EmitSourceDepth("ps = src0.x - ps;\n");
      break;

    // setp_eq dest, src0.a
    case AluScalarOpcode::kSetpEq:
      EmitSourceDepth("if (src0.x == 0.0) {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("  p0 = true;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = 1.0;\n");
      EmitSourceDepth("  p0 = false;\n");
      EmitSourceDepth("}\n");
      break;

    // setp_ne dest, src0.a
    case AluScalarOpcode::kSetpNe:
      EmitSourceDepth("if (src0.x != 0.0) {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("  p0 = true;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = 1.0;\n");
      EmitSourceDepth("  p0 = false;\n");
      EmitSourceDepth("}\n");
      break;

    // setp_gt dest, src0.a
    case AluScalarOpcode::kSetpGt:
      EmitSourceDepth("if (src0.x > 0.0) {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("  p0 = true;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = 1.0;\n");
      EmitSourceDepth("  p0 = false;\n");
      EmitSourceDepth("}\n");
      break;

    // setp_ge dest, src0.a
    case AluScalarOpcode::kSetpGe:
      EmitSourceDepth("if (src0.x >= 0.0) {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("  p0 = true;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = 1.0;\n");
      EmitSourceDepth("  p0 = false;\n");
      EmitSourceDepth("}\n");
      break;

    // setp_inv dest, src0.a
    case AluScalarOpcode::kSetpInv:
      EmitSourceDepth("if (src0.x == 1.0) {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("  p0 = true;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = src0.x == 0.0 ? 1.0 : src0.x;\n");
      EmitSourceDepth("  p0 = false;\n");
      EmitSourceDepth("}\n");
      break;

    // setp_pop dest, src0.a
    case AluScalarOpcode::kSetpPop:
      EmitSourceDepth("if (src0.x - 1.0 <= 0.0) {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("  p0 = true;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = src0.x - 1.0;\n");
      EmitSourceDepth("  p0 = false;\n");
      EmitSourceDepth("}\n");
      break;

    // setp_clr dest
    case AluScalarOpcode::kSetpClr:
      EmitSourceDepth("ps = FLT_MAX;\n");
      EmitSourceDepth("p0 = false;\n");
      break;

    // setp_rstr dest, src0.a
    case AluScalarOpcode::kSetpRstr:
      EmitSourceDepth("ps = src0.x;\n");
      EmitSourceDepth("p0 = src0.x == 0.0 ? true : false;\n");
      break;

    // kills_eq dest, src0.a
    case AluScalarOpcode::kKillsEq:
      EmitSourceDepth("if (src0.x == 0.0) {\n");
      EmitSourceDepth("  ps = 1.0;\n");
      EmitSourceDepth("  discard;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("}\n");
      break;

    // kills_gt dest, src0.a
    case AluScalarOpcode::kKillsGt:
      EmitSourceDepth("if (src0.x > 0.0) {\n");
      EmitSourceDepth("  ps = 1.0;\n");
      EmitSourceDepth("  discard;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("}\n");
      break;

    // kills_ge dest, src0.a
    case AluScalarOpcode::kKillsGe:
      EmitSourceDepth("if (src0.x >= 0.0) {\n");
      EmitSourceDepth("  ps = 1.0;\n");
      EmitSourceDepth("  discard;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("}\n");
      break;

    // kills_ne dest, src0.a
    case AluScalarOpcode::kKillsNe:
      EmitSourceDepth("if (src0.x != 0.0) {\n");
      EmitSourceDepth("  ps = 1.0;\n");
      EmitSourceDepth("  discard;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("}\n");
      break;

    // kills_one dest, src0.a
    case AluScalarOpcode::kKillsOne:
      EmitSourceDepth("if (src0.x == 1.0) {\n");
      EmitSourceDepth("  ps = 1.0;\n");
      EmitSourceDepth("  discard;\n");
      EmitSourceDepth("} else {\n");
      EmitSourceDepth("  ps = 0.0;\n");
      EmitSourceDepth("}\n");
      break;

    // sqrt dest, src0.a
    case AluScalarOpcode::kSqrt:
      EmitSourceDepth("ps = sqrt(src0.x);\n");
      break;

    // mulsc dest, src0.a, src0.b
    case AluScalarOpcode::kMulsc0:
    case AluScalarOpcode::kMulsc1:
      EmitSourceDepth("ps = src0.x * src1.x;\n");
      break;
    // addsc dest, src0.a, src0.b
    case AluScalarOpcode::kAddsc0:
    case AluScalarOpcode::kAddsc1:
      EmitSourceDepth("ps = src0.x + src1.x;\n");
      break;
    // subsc dest, src0.a, src0.b
    case AluScalarOpcode::kSubsc0:
    case AluScalarOpcode::kSubsc1:
      EmitSourceDepth("ps = src0.x - src1.x;\n");
      break;

    // sin dest, src0.a
    case AluScalarOpcode::kSin:
      EmitSourceDepth("ps = sin(src0.x);\n");
      break;

    // cos dest, src0.a
    case AluScalarOpcode::kCos:
      EmitSourceDepth("ps = cos(src0.x);\n");
      break;

    // retain_prev dest
    case AluScalarOpcode::kRetainPrev:
      // ps is reused.
      break;
  }

  EmitStoreScalarResult(instr.result);

  if (instr.is_predicated) {
    Unindent();
    EmitSourceDepth("}\n");
  }
}

}  // namespace gpu
}  // namespace xe
