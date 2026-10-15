#include "Optimization/SrtWalker/SrtCompiled.hpp"
#include "Optimization/SrtWalker/SrtAddressArithmetic.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ShaderRecompiler::Detail {

namespace {

using Op = CompiledSrt::Op;
using Node = CompiledSrt::Node;

float Float32(std::uint64_t bits) { return std::bit_cast<float>(static_cast<std::uint32_t>(bits)); }
std::uint64_t Float32Bits(float value) { return std::bit_cast<std::uint32_t>(value); }

// Evaluator::EvaluateWide's immediate decoding; false for a type it does not evaluate.
bool ImmediateBits(const IrValue& value, std::uint64_t& result) {
    switch (value.Type()) {
        case IrType::Bool: result = value.ImmediateBool() ? 1u : 0u; return true;
        case IrType::U8: result = value.ImmediateU8(); return true;
        case IrType::U16: result = value.ImmediateU16(); return true;
        case IrType::U32: result = value.ImmediateU32(); return true;
        case IrType::U64: result = value.ImmediateU64(); return true;
        case IrType::F32: result = Float32Bits(value.ImmediateF32()); return true;
        default: return false;
    }
}

bool Simple(IrOpcode opcode, Op& op, std::uint8_t& arguments) {
    switch (opcode) {
        case IrOpcode::IAdd32: op = Op::IAdd32; arguments = 2; return true;
        case IrOpcode::IAdd64: op = Op::IAdd64; arguments = 2; return true;
        case IrOpcode::ISub32: op = Op::ISub32; arguments = 2; return true;
        case IrOpcode::ISub64: op = Op::ISub64; arguments = 2; return true;
        case IrOpcode::IMul32: op = Op::IMul32; arguments = 2; return true;
        case IrOpcode::IMul64: op = Op::IMul64; arguments = 2; return true;
        case IrOpcode::UMin32: op = Op::UMin32; arguments = 2; return true;
        case IrOpcode::SMin32: op = Op::SMin32; arguments = 2; return true;
        case IrOpcode::CompositeConstructU64: op = Op::ConstructU64; arguments = 2; return true;
        case IrOpcode::ConvertF32U32: op = Op::ConvertF32U32; arguments = 1; return true;
        case IrOpcode::ConvertU32F32: op = Op::ConvertU32F32; arguments = 1; return true;
        case IrOpcode::FPMul32: op = Op::FPMul32; arguments = 2; return true;
        case IrOpcode::FPTrunc32: op = Op::FPTrunc32; arguments = 1; return true;
        case IrOpcode::FPIsNan32: op = Op::FPIsNan32; arguments = 1; return true;
        case IrOpcode::FPOrdLessThanEqual32: op = Op::FPOrdLessThanEqual32; arguments = 2; return true;
        case IrOpcode::FPOrdGreaterThanEqual32: op = Op::FPOrdGreaterThanEqual32; arguments = 2; return true;
        case IrOpcode::BitwiseAnd32: op = Op::BitwiseAnd32; arguments = 2; return true;
        case IrOpcode::BitwiseAnd64: op = Op::BitwiseAnd64; arguments = 2; return true;
        case IrOpcode::BitwiseOr32: op = Op::BitwiseOr32; arguments = 2; return true;
        case IrOpcode::BitwiseXor32: op = Op::BitwiseXor32; arguments = 2; return true;
        case IrOpcode::BitwiseNot32: op = Op::BitwiseNot32; arguments = 1; return true;
        case IrOpcode::ShiftLeftLogical32: op = Op::ShiftLeftLogical32; arguments = 2; return true;
        case IrOpcode::ShiftLeftLogical64: op = Op::ShiftLeftLogical64; arguments = 2; return true;
        case IrOpcode::ShiftRightLogical32: op = Op::ShiftRightLogical32; arguments = 2; return true;
        case IrOpcode::ShiftRightLogical64: op = Op::ShiftRightLogical64; arguments = 2; return true;
        case IrOpcode::ShiftRightArithmetic32: op = Op::ShiftRightArithmetic32; arguments = 2; return true;
        case IrOpcode::ShiftRightArithmetic64: op = Op::ShiftRightArithmetic64; arguments = 2; return true;
        case IrOpcode::BitFieldUExtract: op = Op::BitFieldUExtract; arguments = 3; return true;
        case IrOpcode::BitFieldSExtract: op = Op::BitFieldSExtract; arguments = 3; return true;
        case IrOpcode::BitFieldInsert: op = Op::BitFieldInsert; arguments = 4; return true;
        case IrOpcode::SelectU32:
        case IrOpcode::SelectU1:
        case IrOpcode::SelectF32: op = Op::Select; arguments = 3; return true;
        case IrOpcode::IEqual32: op = Op::IEqual32; arguments = 2; return true;
        case IrOpcode::INotEqual32: op = Op::INotEqual32; arguments = 2; return true;
        case IrOpcode::SLessThan32: op = Op::SLessThan32; arguments = 2; return true;
        case IrOpcode::ULessThan32: op = Op::ULessThan32; arguments = 2; return true;
        case IrOpcode::UGreaterThan32: op = Op::UGreaterThan32; arguments = 2; return true;
        case IrOpcode::LogicalAnd: op = Op::LogicalAnd; arguments = 2; return true;
        case IrOpcode::LogicalOr: op = Op::LogicalOr; arguments = 2; return true;
        case IrOpcode::LogicalXor: op = Op::LogicalXor; arguments = 2; return true;
        case IrOpcode::LogicalNot: op = Op::LogicalNot; arguments = 1; return true;
        default: return false;
    }
}

class Lowering {
public:
    explicit Lowering(const IrResourcePlan& plan, CompiledSrt& compiled) : _plan(plan), _compiled(compiled) {}

    void Run() {
        for (const auto& stored : _plan.valueStorage) {
            const IrValue* value = stored.Resolve();
            if (_compiled.byPointer.emplace(value, static_cast<std::uint32_t>(_compiled.nodes.size())).second) {
                _compiled.nodes.emplace_back();
                _compiled.nodes.back().value = value;
            }
        }
        if (_plan.valueIdLimit != 0) {
            _compiled.byId.assign(_plan.valueIdLimit, CompiledSrt::None);
            for (std::uint32_t index = 0; index < _compiled.nodes.size(); ++index) {
                const auto id = _compiled.nodes[index].value->Id();
                if (id < _compiled.byId.size()) _compiled.byId[id] = index;
            }
        }
        for (auto& node : _compiled.nodes) lower(node);
    }

private:
    std::uint32_t index(const IrValue* value) const {
        if (value == nullptr) return CompiledSrt::None;
        const auto found = _compiled.byPointer.find(value->Resolve());
        return found == _compiled.byPointer.end() ? CompiledSrt::None : found->second;
    }

    void lower(Node& node) {
        const IrValue& value = *node.value;
        if (value.HasImmediate()) {
            node.op = Op::Imm;
            if (!ImmediateBits(value, node.immediate)) {
                node.op = Op::Fail;
                node.failsImmediately = true;
            }
            return;
        }
        if (value.Opcode() == IrOpcode::Void) {
            node.failsImmediately = true;
            return;
        }
        const auto argument = [&](std::size_t at) { return at < value.ArgumentCount() ? index(value.Argument(at)) : CompiledSrt::None; };
        node.argumentCount = static_cast<std::uint8_t>(std::min<std::size_t>(value.ArgumentCount(), 255u));
        switch (value.Opcode()) {
            case IrOpcode::GetUserData:
                if (value.ArgumentCount() < 1 || value.Argument(0) == nullptr) return;
                node.op = Op::UserData;
                node.immediate = RegIndex(static_cast<ScalarReg>(value.Argument(0)->Register().index));
                return;
            case IrOpcode::GetShaderBase: node.op = Op::ShaderBase; return;
            case IrOpcode::Phi: {
                IrValue* invariant = nullptr;
                try {
                    invariant = ResolveInvariantPhi(_plan, const_cast<IrValue*>(&value));
                } catch (...) {
                    node.op = Op::PhiThrow;
                    return;
                }
                if (invariant == nullptr) return;
                node.op = Op::Alias;
                node.target = index(invariant);
                return;
            }
            case IrOpcode::ReadFirstLane:
                node.op = Op::ReadFirstLane;
                node.arguments[0] = argument(0);
                node.target = argument(1);
                return;
            case IrOpcode::BitCastU32F32:
            case IrOpcode::BitCastF32U32:
                node.op = Op::Alias;
                node.target = argument(0);
                return;
            case IrOpcode::CompositeExtractU64:
            case IrOpcode::CompositeExtractU32x2: {
                const IrValue* component = value.ArgumentCount() > 1 && value.Argument(1) != nullptr ? value.Argument(1)->Resolve() : nullptr;
                if (component == nullptr || !component->HasImmediate() || component->Type() != IrType::U32 || component->ImmediateU32() >= 2u) return;
                node.component = static_cast<std::uint8_t>(component->ImmediateU32());
                if (value.Opcode() == IrOpcode::CompositeExtractU64) {
                    node.op = Op::ExtractU64;
                    node.arguments[0] = argument(0);
                    return;
                }
                if (value.Argument(0) == nullptr) return;
                const IrValue* source = value.Argument(0)->Resolve();
                if (source->Opcode() == IrOpcode::CompositeConstructU32x2) {
                    node.op = Op::Alias;
                    node.target = index(source->Argument(node.component));
                } else if (source->Opcode() == IrOpcode::IAddCarry32) {
                    node.op = Op::AddCarry;
                    node.arguments[0] = index(source->Argument(0));
                    node.arguments[1] = index(source->Argument(1));
                }
                return;
            }
            case IrOpcode::ReadConst: {
                const IrValue* slot = value.ArgumentCount() > 1 && value.Argument(1) != nullptr ? value.Argument(1)->Resolve() : nullptr;
                if (slot == nullptr || !slot->HasImmediate() || slot->Type() != IrType::U32 || slot->ImmediateU32() >= _plan.srtReads.size()) return;
                node.op = Op::ReadConst;
                node.immediate = slot->ImmediateU32();
                node.target = index(_plan.srtReads[slot->ImmediateU32()].value);
                return;
            }
            case IrOpcode::LoadAddressU32:
            case IrOpcode::ReadConstBuffer: {
                if (!IsRawRead(_plan, value) || value.ArgumentCount() < 1 || value.Argument(0) == nullptr) return;
                const auto flags = value.Flags<MemoryFlags>();
                const IrValue* handle = value.Argument(0)->Resolve();
                if (handle->Opcode() == IrOpcode::Void) return;
                node.op = Op::RawRead;
                node.immediate = flags.index;
                node.memoryOffset = static_cast<std::int64_t>(static_cast<std::int32_t>(_plan.memoryInfo[flags.index].offset));
                node.constBuffer = value.Opcode() == IrOpcode::ReadConstBuffer;
                node.fourArgumentHandle = handle->ArgumentCount() == 4u;
                node.loadedFromMemory = LoadedFromMemory(*handle);
                node.arguments[0] = handle->ArgumentCount() > 0 ? index(handle->Argument(0)) : CompiledSrt::None;
                node.arguments[1] = handle->ArgumentCount() > 1 ? index(handle->Argument(1)) : CompiledSrt::None;
                node.arguments[2] = argument(1);
                if (node.fourArgumentHandle) {
                    node.arguments[3] = index(handle->Argument(2));
                    node.target = index(handle->Argument(3));
                }
                return;
            }
            default: break;
        }
        Op op = Op::Fail;
        std::uint8_t count = 0;
        if (!Simple(value.Opcode(), op, count)) return;
        node.op = op;
        for (std::uint8_t at = 0; at < count; ++at) node.arguments[at] = argument(at);
        if (op == Op::Select) node.target = argument(0);
    }

    const IrResourcePlan& _plan;
    CompiledSrt& _compiled;
};

}

std::uint32_t CompiledSrt::Find(const IrValue* value) const {
    value = value->Resolve();
    if (!byId.empty() && value->Id() < byId.size()) {
        const auto index = byId[value->Id()];
        if (index != None && nodes[index].value == value) return index;
    }
    const auto found = byPointer.find(value);
    return found == byPointer.end() ? None : found->second;
}

std::shared_ptr<const CompiledSrt> CompileSrt(const IrResourcePlan& plan) {
    auto compiled = std::make_shared<CompiledSrt>();
    Lowering(plan, *compiled).Run();
    return compiled;
}

bool UseCompiledSrt(const IrResourcePlan& plan) {
    static const bool interpreter = std::getenv("ANYPS5_SRT_INTERPRETER") != nullptr;
    return !interpreter && plan.compiledSrt != nullptr;
}

// Per-node memo and cycle state, stamped per evaluator instead of cleared, from a per-thread pool.
struct CompiledEvaluator::State {
    struct Slot {
        std::uint32_t stamp = 0;
        std::uint32_t visiting = 0;
        std::uint64_t value = 0;
    };
    std::vector<Slot> slots;
    std::uint32_t stamp = 0;

    static std::vector<State*>& Pool() {
        thread_local std::vector<State*> pool;
        return pool;
    }
};

CompiledEvaluator::CompiledEvaluator(const IrResourcePlan& program, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots, CompiledEvaluator* cleanEvaluator, std::uint32_t activeMask)
    : _program(program), _compiled(*program.compiledSrt), _runtime(runtime), _cleanFlatSlots(cleanFlatSlots), _cleanEvaluator(cleanEvaluator), _activeMask(activeMask) {
    auto& pool = State::Pool();
    if (!pool.empty()) {
        _state = pool.back();
        pool.pop_back();
    } else {
        _state = new State;
    }
    if (_state->slots.size() < _compiled.nodes.size()) _state->slots.resize(_compiled.nodes.size());
    if (++_state->stamp == 0) {
        std::fill(_state->slots.begin(), _state->slots.end(), State::Slot{});
        _state->stamp = 1;
    }
}

CompiledEvaluator::~CompiledEvaluator() {
    State::Pool().push_back(_state);
}

bool CompiledEvaluator::Evaluate(IrValue* value, std::uint32_t& result) {
    std::uint64_t wide = 0;
    if (!EvaluateWide(value, wide)) return false;
    result = static_cast<std::uint32_t>(wide);
    return true;
}

bool CompiledEvaluator::EvaluateWide(IrValue* raw, std::uint64_t& result) {
    const IrValue* value = raw->Resolve();
    if (value->HasImmediate()) return ImmediateBits(*value, result);
    const auto index = _compiled.Find(value);
    if (index != CompiledSrt::None) return node(index, result);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) std::fprintf(stderr, "[AnyPS5] compiled SRT plan lacks a value; it is interpreted\n");
    if (!_fallback) {
        _fallback = std::make_unique<Evaluator>(_program, _runtime);
        _fallback->ReportInaccessibleReads(_inaccessible);
        _fallback->ReportNullRootReads(_nullRoots);
    }
    return _fallback->EvaluateWide(raw, result);
}

bool CompiledEvaluator::node(std::uint32_t index, std::uint64_t& result) {
    if (index == CompiledSrt::None) return false;
    const Node& node = _compiled.nodes[index];
    if (node.op == Op::Imm) {
        result = node.immediate;
        return true;
    }
    if (node.failsImmediately) return false;
    if (_activeMask != CompiledSrt::None && node.op == Op::Select && node.argumentCount == 3 && node.target == _activeMask) {
        return this->node(node.arguments[1], result);
    }
    auto& slot = _state->slots[index];
    if (slot.stamp == _state->stamp) {
        result = slot.value;
        return true;
    }
    if (slot.visiting == _state->stamp) return false;
    slot.visiting = _state->stamp;
    std::uint64_t out = 0;
    const bool evaluated = instruction(node, out);
    // The slot is re-fetched: nested evaluation does not resize the state, but stays explicit.
    auto& done = _state->slots[index];
    done.visiting = 0;
    if (!evaluated) {
        static const bool debug = std::getenv("APS5_SRT_DEBUG") != nullptr;
        if (debug) std::fprintf(stderr, "[srt] cannot evaluate %s (%zu arguments)\n", std::string(IrOpcodeName(node.value->Opcode())).c_str(), node.value->ArgumentCount());
        return false;
    }
    done.stamp = _state->stamp;
    done.value = out;
    result = out;
    return true;
}

bool CompiledEvaluator::rawRead(const Node& node, std::uint64_t& result) {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    std::uint64_t offset = 0;
    if (!this->node(node.arguments[0], low) || !this->node(node.arguments[1], high) || !this->node(node.arguments[2], offset)) return false;
    const auto base = ((high << 32u) | static_cast<std::uint32_t>(low)) & AddressMask;
    const auto immediate = node.memoryOffset;
    std::uint64_t address = 0;
    if (node.constBuffer) {
        std::uint64_t records = 0;
        std::uint64_t word3 = 0;
        if (!node.fourArgumentHandle || !this->node(node.arguments[3], records) || !this->node(node.target, word3)) return false;
        if (immediate < 0) return false;
        const auto byteOffset = static_cast<std::uint64_t>(immediate) + static_cast<std::uint32_t>(offset);
        const auto aligned = byteOffset & ~std::uint64_t {3};
        const auto stride = (static_cast<std::uint32_t>(high) >> 16u) & 0x3fffu;
        const auto size = stride == 0u ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(records)) : static_cast<std::uint64_t>(stride) * static_cast<std::uint32_t>(records);
        // s_buffer_load reads zero outside the V#'s range (upstream 5a9d73154, as Evaluator).
        if (aligned > size || size - aligned < sizeof(std::uint32_t)) {
            result = 0;
            return true;
        }
        address = ((base & ~std::uint64_t {3}) + byteOffset) & ~std::uint64_t {3};
    } else {
        const auto relative = (immediate & ~std::int64_t {3}) + static_cast<std::int64_t>(static_cast<std::uint32_t>(offset) & ~3u);
        if (!AddSignedAddress(base & ~std::uint64_t {3}, relative, address)) return false;
    }
    if (_nullRoots != nullptr && base == 0 && !node.loadedFromMemory) {
        ++*_nullRoots;
        result = 0;
        return true;
    }
    if (_inaccessible != nullptr && _runtime.accessible != nullptr && node.loadedFromMemory && !_runtime.accessible(_runtime.userContext, address, sizeof(std::uint32_t))) {
        *_inaccessible = {node.value, address};
        return false;
    }
    if (auto* trace = _runtime.readTrace; trace != nullptr) {
        if (node.value == trace->leaf) trace->leaves.emplace_back(trace->leafSlot, address);
        else trace->otherReads.push_back(address);
    }
    std::uint32_t word = 0;
    if (_runtime.readMemory != nullptr) {
        if (!_runtime.readMemory(_runtime.userContext, address, &word)) return false;
    } else {
        std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
    }
    result = word;
    return true;
}

bool CompiledEvaluator::instruction(const Node& node, std::uint64_t& result) {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
    const auto unary = [&]() { return this->node(node.arguments[0], a); };
    const auto binary = [&]() { return this->node(node.arguments[0], a) && this->node(node.arguments[1], b); };
    const auto ternary = [&]() { return binary() && this->node(node.arguments[2], c); };
    switch (node.op) {
        case Op::Fail: return false;
        case Op::Imm: result = node.immediate; return true;
        case Op::UserData: {
            const auto reg = static_cast<std::uint32_t>(node.immediate);
            if (reg < _program.userDataBase || reg - _program.userDataBase >= _runtime.userData.size()) return false;
            result = _runtime.userData[reg - _program.userDataBase];
            return true;
        }
        case Op::ShaderBase: result = _runtime.shaderBase; return true;
        case Op::Alias: return this->node(node.target, result);
        case Op::PhiThrow: static_cast<void>(ResolveInvariantPhi(_program, const_cast<IrValue*>(node.value))); return false;
        case Op::ReadFirstLane: {
            CompiledEvaluator active(_program, _runtime, _cleanFlatSlots, _cleanEvaluator, node.target);
            active.ReportInaccessibleReads(_inaccessible);
            active.ReportNullRootReads(_nullRoots);
            return active.node(node.arguments[0], result);
        }
        case Op::ExtractU64:
            if (!unary()) return false;
            result = static_cast<std::uint32_t>(a >> (node.component * 32u));
            return true;
        case Op::AddCarry: {
            if (!binary()) return false;
            const auto sum = static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) + static_cast<std::uint32_t>(b);
            result = node.component == 0u ? static_cast<std::uint32_t>(sum) : static_cast<std::uint32_t>(sum >> 32u);
            return true;
        }
        case Op::ConstructU64:
            if (!binary()) return false;
            result = static_cast<std::uint32_t>(a) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b)) << 32u);
            return true;
        case Op::ReadConst: {
            const auto slot = static_cast<std::size_t>(node.immediate);
            if (slot < _cleanFlatSlots.size() && _cleanFlatSlots[slot] != 0u && _cleanEvaluator != nullptr) return _cleanEvaluator->node(node.target, result);
            return this->node(node.target, result);
        }
        case Op::RawRead: return rawRead(node, result);
        case Op::IAdd32: if (!binary()) return false; result = static_cast<std::uint32_t>(a + b); return true;
        case Op::IAdd64: if (!binary()) return false; result = a + b; return true;
        case Op::ISub32: if (!binary()) return false; result = static_cast<std::uint32_t>(a - b); return true;
        case Op::ISub64: if (!binary()) return false; result = a - b; return true;
        case Op::IMul32: if (!binary()) return false; result = static_cast<std::uint32_t>(a * b); return true;
        case Op::IMul64: if (!binary()) return false; result = a * b; return true;
        case Op::UMin32: if (!binary()) return false; result = std::min(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)); return true;
        case Op::SMin32: if (!binary()) return false; result = static_cast<std::uint32_t>(std::min(static_cast<std::int32_t>(a), static_cast<std::int32_t>(b))); return true;
        case Op::ConvertF32U32: if (!unary()) return false; result = Float32Bits(static_cast<float>(static_cast<std::uint32_t>(a))); return true;
        case Op::ConvertU32F32: {
            if (!unary()) return false;
            const auto value = Float32(a);
            if (!std::isfinite(value) || value < 0.0f || static_cast<double>(value) > 4294967295.0) return false;
            result = static_cast<std::uint32_t>(value);
            return true;
        }
        case Op::FPMul32: if (!binary()) return false; result = Float32Bits(Float32(a) * Float32(b)); return true;
        case Op::FPTrunc32: if (!unary()) return false; result = Float32Bits(std::trunc(Float32(a))); return true;
        case Op::FPIsNan32: if (!unary()) return false; result = std::isnan(Float32(a)) ? 1u : 0u; return true;
        case Op::FPOrdLessThanEqual32: if (!binary()) return false; result = Float32(a) <= Float32(b) ? 1u : 0u; return true;
        case Op::FPOrdGreaterThanEqual32: if (!binary()) return false; result = Float32(a) >= Float32(b) ? 1u : 0u; return true;
        case Op::BitwiseAnd32: if (!binary()) return false; result = static_cast<std::uint32_t>(a & b); return true;
        case Op::BitwiseAnd64: if (!binary()) return false; result = a & b; return true;
        case Op::BitwiseOr32: if (!binary()) return false; result = static_cast<std::uint32_t>(a | b); return true;
        case Op::BitwiseXor32: if (!binary()) return false; result = static_cast<std::uint32_t>(a ^ b); return true;
        case Op::BitwiseNot32: if (!unary()) return false; result = ~static_cast<std::uint32_t>(a); return true;
        case Op::ShiftLeftLogical32: if (!binary()) return false; result = static_cast<std::uint32_t>(a) << (b & 31u); return true;
        case Op::ShiftLeftLogical64: if (!binary()) return false; result = a << (b & 63u); return true;
        case Op::ShiftRightLogical32: if (!binary()) return false; result = static_cast<std::uint32_t>(a) >> (b & 31u); return true;
        case Op::ShiftRightLogical64: if (!binary()) return false; result = a >> (b & 63u); return true;
        case Op::ShiftRightArithmetic32: if (!binary()) return false; result = static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a)) >> (b & 31u)); return true;
        case Op::ShiftRightArithmetic64: if (!binary()) return false; result = static_cast<std::uint64_t>(std::bit_cast<std::int64_t>(a) >> (b & 63u)); return true;
        case Op::BitFieldUExtract: {
            if (!ternary()) return false;
            const auto offset = static_cast<std::uint32_t>(b);
            const auto width = static_cast<std::uint32_t>(c);
            if (offset > 32u || width > 32u - offset) return false;
            const auto mask = width == 32u ? 0xffffffffu : width == 0u ? 0u : (std::uint32_t {1} << width) - 1u;
            result = width == 0u ? 0u : (static_cast<std::uint32_t>(a) >> offset) & mask;
            return true;
        }
        case Op::BitFieldSExtract: {
            if (!ternary()) return false;
            const auto offset = static_cast<std::uint32_t>(b);
            const auto width = static_cast<std::uint32_t>(c);
            if (offset > 32u || width > 32u - offset) return false;
            if (width == 0u) {
                result = 0;
                return true;
            }
            const auto mask = width == 32u ? 0xffffffffu : (std::uint32_t {1} << width) - 1u;
            auto bits = (static_cast<std::uint32_t>(a) >> offset) & mask;
            if (width < 32u && (bits & (std::uint32_t {1} << (width - 1u))) != 0u) bits |= ~mask;
            result = bits;
            return true;
        }
        case Op::BitFieldInsert: {
            std::uint64_t d = 0;
            if (!ternary() || !this->node(node.arguments[3], d)) return false;
            const auto offset = static_cast<std::uint32_t>(c);
            const auto width = static_cast<std::uint32_t>(d);
            if (offset > 32u || width > 32u - offset) return false;
            if (width == 0u) {
                result = static_cast<std::uint32_t>(a);
                return true;
            }
            const auto mask = width == 32u ? 0xffffffffu : ((std::uint32_t {1} << width) - 1u) << offset;
            result = (static_cast<std::uint32_t>(a) & ~mask) | ((static_cast<std::uint32_t>(b) << offset) & mask);
            return true;
        }
        case Op::Select: if (!ternary()) return false; result = a != 0u ? b : c; return true;
        case Op::IEqual32: if (!binary()) return false; result = static_cast<std::uint32_t>(a) == static_cast<std::uint32_t>(b) ? 1u : 0u; return true;
        case Op::INotEqual32: if (!binary()) return false; result = static_cast<std::uint32_t>(a) != static_cast<std::uint32_t>(b) ? 1u : 0u; return true;
        case Op::SLessThan32: if (!binary()) return false; result = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a)) < std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(b)) ? 1u : 0u; return true;
        case Op::ULessThan32: if (!binary()) return false; result = static_cast<std::uint32_t>(a) < static_cast<std::uint32_t>(b) ? 1u : 0u; return true;
        case Op::UGreaterThan32: if (!binary()) return false; result = static_cast<std::uint32_t>(a) > static_cast<std::uint32_t>(b) ? 1u : 0u; return true;
        case Op::LogicalAnd: if (!binary()) return false; result = (a != 0u) && (b != 0u) ? 1u : 0u; return true;
        case Op::LogicalOr: if (!binary()) return false; result = (a != 0u) || (b != 0u) ? 1u : 0u; return true;
        case Op::LogicalXor: if (!binary()) return false; result = (a != 0u) != (b != 0u) ? 1u : 0u; return true;
        case Op::LogicalNot: if (!unary()) return false; result = a == 0u ? 1u : 0u; return true;
    }
    return false;
}

}
