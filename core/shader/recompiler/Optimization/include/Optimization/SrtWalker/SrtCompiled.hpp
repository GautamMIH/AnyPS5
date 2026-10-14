#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTCOMPILED_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTCOMPILED_HPP

#include "Optimization/SrtWalker/SrtEvaluator.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace ShaderRecompiler::Detail {

// An extracted plan's values lowered once into a flat node table (see ResourceMaterializer::ExtractPlan):
// what Evaluator works out from the IR on every walk (resolution, invariant phis, raw-read shapes,
// extract shapes, register slots, immediates) is fixed per plan. CompiledEvaluator evaluates the table
// with Evaluator's semantics: lazy, in the same argument order, with the same memoization, cycle
// detection, memory reads and reports.
struct CompiledSrt {
    enum class Op : std::uint8_t {
        Fail,
        Imm,
        UserData,
        ShaderBase,
        Alias,
        PhiThrow,
        ReadFirstLane,
        ExtractU64,
        AddCarry,
        ConstructU64,
        ReadConst,
        RawRead,
        IAdd32, IAdd64, ISub32, ISub64, IMul32, IMul64, UMin32, SMin32,
        ConvertF32U32, ConvertU32F32, FPMul32, FPTrunc32, FPIsNan32, FPOrdLessThanEqual32, FPOrdGreaterThanEqual32,
        BitwiseAnd32, BitwiseAnd64, BitwiseOr32, BitwiseXor32, BitwiseNot32,
        ShiftLeftLogical32, ShiftLeftLogical64, ShiftRightLogical32, ShiftRightLogical64, ShiftRightArithmetic32, ShiftRightArithmetic64,
        BitFieldUExtract, BitFieldSExtract, BitFieldInsert,
        Select, IEqual32, INotEqual32, SLessThan32, ULessThan32, UGreaterThan32,
        LogicalAnd, LogicalOr, LogicalXor, LogicalNot,
    };
    static constexpr std::uint32_t None = 0xffffffffu;
    struct Node {
        Op op = Op::Fail;
        // Evaluation fails before the instruction runs (a void or unsupported immediate, an
        // unsupported shape): no arguments are evaluated.
        bool failsImmediately = false;
        // RawRead: the handle is loaded from memory (LoadedFromMemory); a ReadConstBuffer handle has
        // four arguments.
        bool loadedFromMemory = false;
        bool constBuffer = false;
        bool fourArgumentHandle = false;
        std::uint8_t argumentCount = 0;
        // ExtractU64 / AddCarry: the component.
        std::uint8_t component = 0;
        std::uint32_t arguments[4] = {None, None, None, None};
        // Imm: the value; UserData: the register index; ReadConst: the slot; RawRead: the memory
        // info index.
        std::uint64_t immediate = 0;
        // Select (the active-mask short-circuit): the condition node; ReadConst: the slot's value node.
        std::uint32_t target = None;
        std::int64_t memoryOffset = 0;
        const IrValue* value = nullptr;
    };
    std::vector<Node> nodes;
    // Node of a plan value by IrValue::Id when the plan's ids are unique (valueIdLimit), else by pointer.
    std::vector<std::uint32_t> byId;
    std::unordered_map<const IrValue*, std::uint32_t> byPointer;

    [[nodiscard]] std::uint32_t Find(const IrValue* value) const;
};

[[nodiscard]] std::shared_ptr<const CompiledSrt> CompileSrt(const IrResourcePlan& plan);
// ANYPS5_SRT_INTERPRETER=1 evaluates every plan with Evaluator.
[[nodiscard]] bool UseCompiledSrt(const IrResourcePlan& plan);

class CompiledEvaluator {
public:
    CompiledEvaluator(const IrResourcePlan& program, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots = {}, CompiledEvaluator* cleanEvaluator = nullptr, std::uint32_t activeMask = CompiledSrt::None);
    CompiledEvaluator(const CompiledEvaluator&) = delete;
    CompiledEvaluator& operator=(const CompiledEvaluator&) = delete;
    ~CompiledEvaluator();

    bool Evaluate(IrValue* value, std::uint32_t& result);
    bool EvaluateWide(IrValue* value, std::uint64_t& result);
    void ReportInaccessibleReads(InaccessibleRead* sink) { _inaccessible = sink; }
    void ReportNullRootReads(std::uint32_t* sink) { _nullRoots = sink; }

private:
    struct State;
    bool node(std::uint32_t index, std::uint64_t& result);
    bool instruction(const CompiledSrt::Node& node, std::uint64_t& result);
    bool rawRead(const CompiledSrt::Node& node, std::uint64_t& result);

    const IrResourcePlan& _program;
    const CompiledSrt& _compiled;
    const SrtRuntime& _runtime;
    std::span<const std::uint8_t> _cleanFlatSlots;
    CompiledEvaluator* _cleanEvaluator = nullptr;
    std::uint32_t _activeMask = CompiledSrt::None;
    InaccessibleRead* _inaccessible = nullptr;
    std::uint32_t* _nullRoots = nullptr;
    State* _state = nullptr;
    // A value the table does not hold (none expected) is evaluated by the interpreter.
    std::unique_ptr<Evaluator> _fallback;
};

}

#endif
