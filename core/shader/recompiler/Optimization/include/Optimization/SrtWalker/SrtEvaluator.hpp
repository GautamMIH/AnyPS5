#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace ShaderRecompiler::Detail {

class EvaluatedValues {
public:
    EvaluatedValues() = default;
    EvaluatedValues(const EvaluatedValues&) = delete;
    EvaluatedValues& operator=(const EvaluatedValues&) = delete;
    ~EvaluatedValues() {
        if (_dense != nullptr) DensePool().push_back(_dense);
    }

    // Values with ids below limit (unique in the plan) keep their state in an array from a
    // per-thread pool, stamped per evaluator instead of cleared: a hash table grown per walk cost
    // about 1 ms of every heavy frame.
    void UseDense(std::uint32_t limit) {
        // ANYPS5_NO_DENSE_SRT=1 keeps every value in the hash table.
        static const bool hashOnly = std::getenv("ANYPS5_NO_DENSE_SRT") != nullptr;
        if (limit == 0 || hashOnly) return;
        auto& pool = DensePool();
        if (!pool.empty()) {
            _dense = pool.back();
            pool.pop_back();
        } else {
            _dense = new DenseState;
        }
        if (_dense->slots.size() < limit) _dense->slots.resize(limit);
        if (++_dense->stamp == 0) {
            std::fill(_dense->slots.begin(), _dense->slots.end(), DenseSlot{});
            _dense->stamp = 1;
        }
        _limit = limit;
    }
    // Cycle detection for dense values: false when the value is already being evaluated.
    bool BeginVisit(const IrValue* key) {
        auto& slot = _dense->slots[key->Id()];
        if (slot.visiting == _dense->stamp) return false;
        slot.visiting = _dense->stamp;
        return true;
    }
    void EndVisit(const IrValue* key) { _dense->slots[key->Id()].visiting = 0; }
    bool Dense(const IrValue* key) const { return _dense != nullptr && key->Id() < _limit; }

    bool Find(const IrValue* key, std::uint64_t& value) const {
        if (Dense(key)) {
            const auto& slot = _dense->slots[key->Id()];
            if (slot.stamp != _dense->stamp) return false;
            value = slot.value;
            return true;
        }
        if (_slots.empty()) {
            return false;
        }
        for (std::size_t index = Home(key);; index = (index + 1u) & (_slots.size() - 1u)) {
            const auto& slot = _slots[index];
            if (slot.key == key) {
                value = slot.value;
                return true;
            }
            if (slot.key == nullptr) {
                return false;
            }
        }
    }
    void Insert(const IrValue* key, std::uint64_t value) {
        if (Dense(key)) {
            auto& slot = _dense->slots[key->Id()];
            if (slot.stamp != _dense->stamp) {
                slot.stamp = _dense->stamp;
                slot.value = value;
            }
            return;
        }
        if ((_count + 1u) * 2u > _slots.size()) {
            Grow();
        }
        for (std::size_t index = Home(key);; index = (index + 1u) & (_slots.size() - 1u)) {
            auto& slot = _slots[index];
            if (slot.key == key) {
                return;
            }
            if (slot.key == nullptr) {
                slot = {key, value};
                ++_count;
                return;
            }
        }
    }

private:
    struct Slot {
        const IrValue* key = nullptr;
        std::uint64_t value = 0;
    };
    std::size_t Home(const IrValue* key) const {
        return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(key) >> 4u) * 0x9e3779b97f4a7c15ull >> 32u) & (_slots.size() - 1u);
    }
    void Grow() {
        std::vector<Slot> previous(_slots.empty() ? 64u : _slots.size() * 2u);
        previous.swap(_slots);
        _count = 0;
        for (const auto& slot : previous) {
            if (slot.key != nullptr) {
                Insert(slot.key, slot.value);
            }
        }
    }
    std::vector<Slot> _slots;
    std::size_t _count = 0;
    struct DenseSlot {
        std::uint32_t stamp = 0;
        std::uint32_t visiting = 0;
        std::uint64_t value = 0;
    };
    struct DenseState {
        std::vector<DenseSlot> slots;
        std::uint32_t stamp = 0;
    };
    static std::vector<DenseState*>& DensePool() {
        thread_local std::vector<DenseState*> pool;
        return pool;
    }
    DenseState* _dense = nullptr;
    std::uint32_t _limit = 0;
};

class Evaluator {
public:
    Evaluator(const IrResourcePlan& program, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots = {}, Evaluator* cleanEvaluator = nullptr, IrValue* activeMask = nullptr) : _program(program), _runtime(runtime), _cleanFlatSlots(cleanFlatSlots), _cleanEvaluator(cleanEvaluator), _activeMask(activeMask != nullptr ? activeMask->Resolve() : nullptr) {
        _cache.UseDense(program.valueIdLimit);
    }

    bool Evaluate(IrValue* value, std::uint32_t& result);
    bool EvaluateWide(IrValue* raw, std::uint64_t& result);

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
    EvaluatedValues _cache;
    std::vector<IrValue*> _visiting;
};

}

#endif
