// The PS5's Zen 2 CPU implements AMD's SSE4a (EXTRQ, INSERTQ, MOVNTSS, MOVNTSD); Intel CPUs raise
// SIGILL on them. Guest code that uses them is emulated here on the signal context, so games run
// on any x86-64 host. Other illegal instructions keep their previous handling.
#if defined(__linux__)
#include <csignal>
#include <cstdint>
#include <cstring>
#include <exception>
#include <ucontext.h>

namespace {

struct sigaction previousAction{};

// ModRM register numbers to ucontext general-register slots.
constexpr int kRegisterSlots[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                    REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};

std::uint64_t lowQword(const ucontext_t* context, unsigned xmm) {
    std::uint64_t value = 0;
    std::memcpy(&value, context->uc_mcontext.fpregs->_xmm[xmm].element, sizeof(value));
    return value;
}

std::uint64_t highQword(const ucontext_t* context, unsigned xmm) {
    std::uint64_t value = 0;
    std::memcpy(&value, context->uc_mcontext.fpregs->_xmm[xmm].element + 2, sizeof(value));
    return value;
}

void setLowQword(ucontext_t* context, unsigned xmm, std::uint64_t value) {
    std::memcpy(context->uc_mcontext.fpregs->_xmm[xmm].element, &value, sizeof(value));
}

std::uint64_t fieldMask(unsigned length) {
    return length == 0 ? ~0ull : (1ull << length) - 1ull;
}

// EXTRQ: dst[63:0] = (dst[63:0] >> index) & mask(length); a zero length means 64.
void extract(ucontext_t* context, unsigned destination, unsigned lengthField, unsigned indexField) {
    const auto length = lengthField & 0x3fu;
    const auto index = indexField & 0x3fu;
    setLowQword(context, destination, (lowQword(context, destination) >> index) & fieldMask(length));
}

// INSERTQ: dst[index + length - 1:index] = src[length - 1:0].
void insert(ucontext_t* context, unsigned destination, unsigned source, unsigned lengthField, unsigned indexField) {
    const auto length = lengthField & 0x3fu;
    const auto index = indexField & 0x3fu;
    const auto mask = fieldMask(length) << index;
    const auto value = (lowQword(context, source) << index) & mask;
    setLowQword(context, destination, (lowQword(context, destination) & ~mask) | value);
}

// Decodes a ModRM memory operand; returns false for forms this emulator does not handle.
bool memoryOperand(const ucontext_t* context, const std::uint8_t*& cursor, std::uint8_t modrm, std::uint8_t rex, const std::uint8_t* instruction, std::uint64_t& address, unsigned trailingBytes) {
    const auto mod = modrm >> 6u;
    auto rm = modrm & 7u;
    const auto& registers = context->uc_mcontext.gregs;
    if (mod == 3) return false;
    std::int64_t displacement = 0;
    bool ripRelative = false;
    if (rm == 4) {
        const auto sib = *cursor++;
        const auto scale = 1u << (sib >> 6u);
        const auto indexRegister = ((sib >> 3u) & 7u) | ((rex & 2u) << 2u);
        auto baseRegister = (sib & 7u) | ((rex & 1u) << 3u);
        address = 0;
        if (indexRegister != 4) address += static_cast<std::uint64_t>(registers[kRegisterSlots[indexRegister]]) * scale;
        if ((sib & 7u) == 5 && mod == 0) {
            std::int32_t value = 0;
            std::memcpy(&value, cursor, sizeof(value));
            cursor += 4;
            displacement = value;
        } else {
            address += static_cast<std::uint64_t>(registers[kRegisterSlots[baseRegister]]);
        }
    } else if (rm == 5 && mod == 0) {
        ripRelative = true;
        std::int32_t value = 0;
        std::memcpy(&value, cursor, sizeof(value));
        cursor += 4;
        displacement = value;
        address = 0;
    } else {
        rm |= (rex & 1u) << 3u;
        address = static_cast<std::uint64_t>(registers[kRegisterSlots[rm]]);
    }
    if (mod == 1) {
        displacement = static_cast<std::int8_t>(*cursor++);
    } else if (mod == 2) {
        std::int32_t value = 0;
        std::memcpy(&value, cursor, sizeof(value));
        cursor += 4;
        displacement = value;
    }
    if (ripRelative) address = reinterpret_cast<std::uint64_t>(instruction) + static_cast<std::uint64_t>(cursor - instruction) + trailingBytes;
    address += static_cast<std::uint64_t>(displacement);
    return true;
}

bool emulate(ucontext_t* context) {
    if (context->uc_mcontext.fpregs == nullptr) return false;
    const auto* instruction = reinterpret_cast<const std::uint8_t*>(context->uc_mcontext.gregs[REG_RIP]);
    const auto* cursor = instruction;
    std::uint8_t mandatory = 0;
    for (int i = 0; i < 4 && (*cursor == 0x66 || *cursor == 0xf2 || *cursor == 0xf3); ++i) mandatory = *cursor++;
    std::uint8_t rex = 0;
    if ((*cursor & 0xf0u) == 0x40u) rex = *cursor++ & 0x0fu;
    if (*cursor++ != 0x0f) return false;
    const auto opcode = *cursor++;
    const auto modrm = *cursor++;
    const auto reg = ((modrm >> 3u) & 7u) | ((rex & 4u) << 1u);
    const auto rm = (modrm & 7u) | ((rex & 1u) << 3u);
    const bool registerForm = (modrm >> 6u) == 3;
    if (opcode == 0x78 && registerForm) {
        const auto length = cursor[0];
        const auto index = cursor[1];
        cursor += 2;
        if (mandatory == 0x66 && (modrm & 0x38u) == 0) extract(context, rm, length, index);
        else if (mandatory == 0xf2) insert(context, reg, rm, length, index);
        else return false;
    } else if (opcode == 0x79 && registerForm) {
        if (mandatory == 0x66) {
            const auto control = lowQword(context, rm);
            extract(context, reg, static_cast<unsigned>(control), static_cast<unsigned>(control >> 8u));
        } else if (mandatory == 0xf2) {
            const auto control = highQword(context, rm);
            insert(context, reg, rm, static_cast<unsigned>(control), static_cast<unsigned>(control >> 8u));
        } else {
            return false;
        }
    } else if (opcode == 0x2b && !registerForm && (mandatory == 0xf2 || mandatory == 0xf3)) {
        // MOVNTSD / MOVNTSS: non-temporal scalar stores; a normal store has the same result.
        std::uint64_t address = 0;
        if (!memoryOperand(context, cursor, modrm, rex, instruction, address, 0)) return false;
        const auto value = lowQword(context, reg);
        std::memcpy(reinterpret_cast<void*>(address), &value, mandatory == 0xf2 ? 8 : 4);
    } else {
        return false;
    }
    context->uc_mcontext.gregs[REG_RIP] += cursor - instruction;
    return true;
}

void handleIllegal(int signal, siginfo_t* info, void* context) {
    if (emulate(static_cast<ucontext_t*>(context))) return;
    if ((previousAction.sa_flags & SA_SIGINFO) != 0 && previousAction.sa_sigaction != nullptr) {
        previousAction.sa_sigaction(signal, info, context);
        return;
    }
    if (previousAction.sa_handler != SIG_DFL && previousAction.sa_handler != SIG_IGN && previousAction.sa_handler != nullptr) {
        previousAction.sa_handler(signal);
        return;
    }
    // Re-raise with the default action when the faulting instruction runs again.
    if (sigaction(signal, &previousAction, nullptr) != 0) std::terminate();
}

__attribute__((constructor)) void installSse4aEmulation() {
    struct sigaction action{};
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    action.sa_sigaction = handleIllegal;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGILL, &action, &previousAction) != 0) std::terminate();
}

}
#endif
