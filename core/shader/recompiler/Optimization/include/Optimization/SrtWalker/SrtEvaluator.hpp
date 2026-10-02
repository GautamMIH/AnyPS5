#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace ShaderRecompiler::Detail {

class Evaluator {
public:
    Evaluator(const IrResourcePlan& program, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots = {}, Evaluator* cleanEvaluator = nullptr, IrValue* activeMask = nullptr);
    ~Evaluator();
    Evaluator(const Evaluator&) = delete;
    Evaluator& operator=(const Evaluator&) = delete;

    bool Evaluate(IrValue* value, std::uint32_t& result);
    bool EvaluateWide(IrValue* raw, std::uint64_t& result);

    // Evaluated values (materialization runs per draw): slots indexed by value id, from a
    // thread-local pool and valid for this evaluator's stamp, so nothing is allocated or cleared per
    // evaluation. A value whose slot another value holds goes to the map.
    struct Slot {
        IrValue* value = nullptr;
        std::uint64_t result = 0;
        std::uint32_t stamp = 0;
    };

private:
    static float Float32(std::uint64_t bits);
    static std::uint64_t Float32Bits(float value);

    bool Arg(IrValue& inst, std::size_t index, std::uint64_t& result);
    bool EvaluatePhi(IrValue& inst, std::uint64_t& result);
    bool EvaluateExtract(IrValue& inst, std::uint64_t& result);
    bool EvaluateRawRead(IrValue& inst, std::uint64_t& result);
    bool EvaluateInst(IrValue& inst, std::uint64_t& result);

    const IrResourcePlan& _program;
    const SrtRuntime& _runtime;
    std::span<const std::uint8_t> _cleanFlatSlots;
    Evaluator* _cleanEvaluator = nullptr;
    IrValue* _activeMask = nullptr;
    std::vector<Slot>* _slots = nullptr;
    std::uint32_t _stamp = 0;
    std::unordered_map<IrValue*, std::uint64_t> _cache;
    std::vector<IrValue*> _visiting;
};

}

#endif
