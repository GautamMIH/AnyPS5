#include "Translation/TranslationContext.hpp"
#include "RdnaDecoder/RdnaImageOpDecoder.hpp"
#include "Recompiler.hpp"
#include <array>
#include <stdexcept>

using namespace ShaderRecompiler;
static void Require(bool value) { if (!value) throw std::runtime_error("ray tracing miss regression"); }
int main() {
    const std::array<std::uint32_t, 2> code{0xF1981F01u, 0x00000000u};
    const RdnaInstruction instruction = DecodeRdnaMimg(0u, code, 0u);
    Require(instruction.op == RdnaOpcode::ImageBvhIntersectRay);
    Require(instruction.family == RdnaInstructionFamily::MIMG);
    Require(IsImageOpcode(instruction.op));
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    TranslationContext context(program, block, 256);
    if (RayTracingStrict()) {
        bool thrown = false;
        try {
            context.TranslateInstruction(instruction);
        } catch (const std::runtime_error&) {
            thrown = true;
        }
        Require(thrown);
    } else {
        context.TranslateInstruction(instruction);
        if (RayTracingMiss()) {
            for (auto* value : block.Instructions()) Require(value->Opcode() != IrOpcode::ImageBvhIntersectRay);
        }
    }
}
