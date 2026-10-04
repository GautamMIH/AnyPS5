#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_QUEUESTATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_QUEUESTATE_HPP

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <map>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace AgcDriver {

// A register bank: offset -> value, with std::map's interface as the driver uses it. Every context
// and shader register (and most user-config ones) lies below DenseLimit and is kept in an array
// with a presence mask, so the per-draw state decode reads it by index (a std::map lookup cost
// ~11% of the worker in heavy Zorro frames); larger offsets go to a map. Iteration is in offset
// order, like std::map's.
class Registers {
public:
    static constexpr std::uint32_t DenseLimit = 0x400;
    using value_type = std::pair<const std::uint32_t, std::uint32_t>;

    class const_iterator {
    public:
        struct Arrow {
            value_type value;
            const value_type* operator->() const { return &value; }
        };
        const_iterator() = default;
        value_type operator*() const { return {key(), value()}; }
        Arrow operator->() const { return {{key(), value()}}; }
        const_iterator& operator++() {
            if (dense < DenseLimit) {
                dense = owner->nextPresent(dense + 1);
                if (dense == DenseLimit) sparse = owner->sparse.begin();
            } else {
                ++sparse;
            }
            return *this;
        }
        bool operator==(const const_iterator& other) const { return dense == other.dense && (dense < DenseLimit || sparse == other.sparse); }

    private:
        friend class Registers;
        const_iterator(const Registers* owner, std::uint32_t dense, std::map<std::uint32_t, std::uint32_t>::const_iterator sparse) : owner(owner), dense(dense), sparse(sparse) {}
        std::uint32_t key() const { return dense < DenseLimit ? dense : sparse->first; }
        std::uint32_t value() const { return dense < DenseLimit ? owner->dense[dense] : sparse->second; }
        const Registers* owner = nullptr;
        std::uint32_t dense = DenseLimit;
        std::map<std::uint32_t, std::uint32_t>::const_iterator sparse;
    };
    using iterator = const_iterator;

    Registers() = default;
    Registers(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> values) {
        for (const auto& [offset, value] : values) emplace(offset, value);
    }

    bool contains(std::uint32_t offset) const { return offset < DenseLimit ? has(offset) : sparse.contains(offset); }
    std::size_t count(std::uint32_t offset) const { return contains(offset) ? 1 : 0; }
    std::size_t size() const { return denseCount + sparse.size(); }
    bool empty() const { return size() == 0; }

    std::uint32_t at(std::uint32_t offset) const {
        if (offset < DenseLimit) {
            if (!has(offset)) throw std::out_of_range("register not written");
            return dense[offset];
        }
        return sparse.at(offset);
    }
    std::uint32_t& operator[](std::uint32_t offset) {
        if (offset >= DenseLimit) return sparse[offset];
        mark(offset);
        return dense[offset];
    }
    std::pair<const_iterator, bool> emplace(std::uint32_t offset, std::uint32_t value) {
        if (contains(offset)) return {find(offset), false};
        (*this)[offset] = value;
        return {find(offset), true};
    }
    std::pair<const_iterator, bool> insert_or_assign(std::uint32_t offset, std::uint32_t value) {
        const bool added = !contains(offset);
        (*this)[offset] = value;
        return {find(offset), added};
    }
    std::size_t erase(std::uint32_t offset) {
        if (offset >= DenseLimit) return sparse.erase(offset);
        if (!has(offset)) return 0;
        presence[offset / 64] &= ~(std::uint64_t{1} << (offset % 64));
        dense[offset] = 0;
        --denseCount;
        return 1;
    }
    void clear() { *this = Registers{}; }

    const_iterator find(std::uint32_t offset) const {
        if (offset < DenseLimit) return has(offset) ? const_iterator(this, offset, sparse.end()) : end();
        const auto found = sparse.find(offset);
        return found == sparse.end() ? end() : const_iterator(this, DenseLimit, found);
    }
    const_iterator begin() const {
        const auto first = nextPresent(0);
        return const_iterator(this, first, first == DenseLimit ? sparse.begin() : sparse.end());
    }
    const_iterator end() const { return const_iterator(this, DenseLimit, sparse.end()); }

    bool operator==(const Registers& other) const { return presence == other.presence && dense == other.dense && sparse == other.sparse; }

private:
    bool has(std::uint32_t offset) const { return (presence[offset / 64] >> (offset % 64)) & 1u; }
    void mark(std::uint32_t offset) {
        if (has(offset)) return;
        presence[offset / 64] |= std::uint64_t{1} << (offset % 64);
        ++denseCount;
    }
    std::uint32_t nextPresent(std::uint32_t from) const {
        for (auto word = from / 64; word < presence.size(); ++word) {
            auto bits = presence[word];
            if (word == from / 64) bits &= ~std::uint64_t{0} << (from % 64);
            if (bits != 0) return word * 64 + static_cast<std::uint32_t>(std::countr_zero(bits));
        }
        return DenseLimit;
    }
    // Absent dense entries hold 0, so equal banks compare equal as arrays.
    std::array<std::uint32_t, DenseLimit> dense{};
    std::array<std::uint64_t, DenseLimit / 64> presence{};
    std::size_t denseCount = 0;
    std::map<std::uint32_t, std::uint32_t> sparse;
};

inline Registers buildInitialContextRegisters() {
    Registers result{
        {0x200, 0}, {0x201, 0}, {0x202, 0xcc0010}, {0x203, 0},
        {0x204, 0}, {0x205, 0}, {0x206, 1087}, {0x207, 0},
        {0x0, 0}, {0x2, 0}, {0x3, 0}, {0x4, 0}, {0x7, 0}, {0x8, 0}, {0x9, 0x3f800000}, {0xa, 0}, {0xb, 0},
        {0x10, 0}, {0x11, 0}, {0x12, 0}, {0x13, 0}, {0x14, 0}, {0x15, 0},
        {0x1a, 0}, {0x1b, 0}, {0x1c, 0}, {0x1d, 0}, {0x1e, 0}, {0x10b, 0}, {0x10c, 0}, {0x10d, 0},
        {0x2de, 0}, {0x2df, 0}, {0x2e0, 0}, {0x2e1, 0}, {0x2e2, 0}, {0x2e3, 0},
        {0x80, 0}, {0x83, 0xffff}, {0x8c, 0xaa99aaaa}, {0x8d, 0}, {0x8e, 0}, {0x8f, 0},
        {0xc, 0}, {0xd, 0x40004000}, {0x81, 0x80000000}, {0x82, 0x40004000},
        {0x90, 0x80000000}, {0x91, 0x40004000},
        {0x105, 0}, {0x106, 0}, {0x107, 0}, {0x108, 0},
        {0x1b1, 0}, {0x1b3, 0}, {0x1b4, 0}, {0x1b6, 0}, {0x1c3, 0}, {0x1c4, 0}, {0x1c5, 0},
        {0x1ff, 0}, {0x292, 2}, {0x293, 0}, {0x29b, 0},
        {0x2ce, 0}, {0x2d3, 0}, {0x2d5, 0}, {0x2d6, 0}, {0x2db, 0},
        {0x2dc, 0xaa00}, {0x2e4, 0}, {0x2f8, 0}, {0x2f9, 0x2d},
        {0x30e, 0xffffffff}, {0x30f, 0xffffffff}, {0x313, 0x6000},
        {0x318, 0}, {0x31b, 0}, {0x31c, 0}, {0x31d, 0},
        {0x390, 0}, {0x3b0, 0}, {0x3b8, 0}
    };
    for (std::uint32_t i = 0; i < 8; ++i) result.emplace(0x1e0 + i, 0x20010001);
    for (std::uint32_t i = 0; i < 8; ++i) {
        // CB_COLOR<i> BASE, VIEW, INFO, ATTRIB, CMASK and CLEAR_WORD0/1, then the per-target arrays
        // BASE_EXT, CMASK_BASE_EXT, ATTRIB2 and ATTRIB3.
        for (const auto offset : {0x318u, 0x31bu, 0x31cu, 0x31du, 0x31fu, 0x323u, 0x324u}) result.emplace(offset + 0xf * i, 0);
        for (const auto offset : {0x390u, 0x398u, 0x3b0u, 0x3b8u}) result.emplace(offset + i, 0);
    }
    for (std::uint32_t i = 0; i < 16; ++i) {
        result.emplace(0x94 + 2 * i, 0x80000000);
        result.emplace(0x95 + 2 * i, 0x40004000);
        result.emplace(0xb4 + 2 * i, 0);
        result.emplace(0xb5 + 2 * i, 0);
        for (std::uint32_t j = 0; j < 6; ++j) result.emplace(0x10f + 6 * i + j, j % 2 == 0 ? 0x3f800000 : 0);
    }
    return result;
}

// Built once: the map has ~350 entries (CLEAR_STATE and every new queue state copy it).
inline const Registers& InitialContext() {
    static const Registers initial = buildInitialContextRegisters();
    return initial;
}

inline Registers InitialContextRegisters() {
    return InitialContext();
}

struct QueueState {
    Registers shader;
    Registers context = InitialContext();
    Registers userConfig{{0x24a, 0}, {0x24b, 0}};
    std::optional<Registers> savedContext;
    std::array<std::uint32_t, 0x3000> constantRam{};
    std::uint64_t indexBase = 0;
    std::uint64_t drawIndirectBase = 0;
    std::uint64_t dispatchIndirectBase = 0;
    std::uint32_t indexBufferSize = 0;
    std::uint32_t indexType = 0;
    std::uint32_t instanceCount = 1;
    // Set by SET_PREDICATION; packets with the PREDICATE header bit are skipped while it is true.
    bool predicateSkip = false;
    std::vector<std::string> markers;

    void ClearContext() {
        // Copy-assignment reuses the map's nodes.
        context = InitialContext();
    }
};

}

#endif
