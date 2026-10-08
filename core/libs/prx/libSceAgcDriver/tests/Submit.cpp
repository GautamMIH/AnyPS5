#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Eq/include/Query.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static_assert(sizeof(Packet) == 16);
static_assert(offsetof(Packet, addr) == 0);
static_assert(offsetof(Packet, dw_num) == 8);
static_assert(offsetof(Packet, flags) == 12);

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
std::string expectFailure(TAction action) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    throw std::runtime_error("expected exception");
}

void testEvents() {
    KernelEvent event{};
    event.filter = -14;
    event.ident = 0x40;
    event.data = 123;
    check(sceAgcDriverGetEqEventType(&event) == 0x40, "graphics event uses wrong field");
    event.filter = -1;
    event.data = -17;
    check(sceAgcDriverGetEqEventType(&event) == -17, "non-graphics event uses wrong field");
    event.data = std::numeric_limits<std::intptr_t>::max();
    expectFailure([&] { sceAgcDriverGetEqEventType(&event); });
    event.filter = -14;
    event.ident = std::numeric_limits<std::uintptr_t>::max();
    expectFailure([&] { sceAgcDriverGetEqEventType(&event); });
    expectFailure([] { sceAgcDriverGetEqEventType(nullptr); });
    expectFailure([&] { sceAgcDriverGetEqEventType(reinterpret_cast<const KernelEvent*>(reinterpret_cast<const std::byte*>(&event) + 1)); });
    event.ident = 0x29;
    check(sceAgcDriverGetEqContextId(&event) == 0x29, "graphics event context id uses wrong field");
    event.ident = std::numeric_limits<std::uintptr_t>::max();
    expectFailure([&] { sceAgcDriverGetEqContextId(&event); });
    event.ident = 1;
    event.filter = -1;
    expectFailure([&] { sceAgcDriverGetEqContextId(&event); });
    expectFailure([] { sceAgcDriverGetEqContextId(nullptr); });
    expectFailure([&] { sceAgcDriverGetEqContextId(reinterpret_cast<const KernelEvent*>(reinterpret_cast<const std::byte*>(&event) + 1)); });
}

void testValidation() {
    std::array<std::uint32_t, 3> commands{0xc0017600, 0x20c, 0};
    Packet packet{commands.data(), 3, 0, {}};
    expectFailure([] { sceAgcDriverSubmitDcb(nullptr); });
    expectFailure([] { sceAgcDriverAgrSubmitDcb(nullptr); });
    expectFailure([] { sceAgcDriverSubmitAcb(0x20, nullptr); });
    expectFailure([&] { sceAgcDriverSubmitAcb(0, &packet); });
    expectFailure([&] { sceAgcDriverSubmitAcb(0x58, &packet); });
    packet.dw_num = 2;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.dw_num = 3;
    packet.flags = 1;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.flags = 0;
    commands[0] = 0xc001ff00;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    commands[0] = 0xc001105c;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    commands[0] = 0xc0017604;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    commands[0] = 0xc0017600;
    commands[1] = 0x10000;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.addr = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(commands.data()) + 1);
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.addr = reinterpret_cast<std::uint32_t*>(std::numeric_limits<std::uintptr_t>::max() - 3);
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.addr = reinterpret_cast<std::uint32_t*>(0x1000);
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    AgcDriverWaitIdle_nid_postfix();
}

void testClearState() {
    AgcDriver::QueueState graphics{{{0x20c, 1}}, {{0x10, 17}, {0x11, 23}}, {{0x242, 5}}};
    const auto shader = graphics.shader;
    const auto userConfig = graphics.userConfig;
    graphics.ClearContext();
    check(graphics.context == AgcDriver::InitialContextRegisters(), "CLEAR_STATE retained context registers");
    check(graphics.shader == shader && graphics.userConfig == userConfig, "CLEAR_STATE reset unrelated registers");
    check(graphics.context.count(0x1b3) == 1 && graphics.context.at(0x1b3) == 0 && graphics.context.count(0x1b4) == 1 && graphics.context.at(0x1b4) == 0, "CLEAR_STATE left SPI_PS_INPUT_ENA/ADDR unset");
    graphics.context.emplace(0x10, 31);
    graphics.ClearContext();
    check(graphics.context == AgcDriver::InitialContextRegisters(), "repeated CLEAR_STATE retained context registers");

    std::array<std::uint32_t, 3> words{0xc0001200, 0, 0};
    Packet packet{words.data(), 2, 0, {}};
    expectFailure([&] { sceAgcDriverSubmitAcb(0x20, &packet); });
    words[1] = 0x10;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    words[1] = 0;
    words[0] = 0xc0011200;
    packet.dw_num = 3;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    words[0] = 0xc0001204;
    packet.dw_num = 2;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    words[0] = 0xc0001200;
    packet.dw_num = 1;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    packet.dw_num = 2;
    for (std::uint32_t state = 0; state <= 0xf; ++state) {
        words[1] = state;
        check(sceAgcDriverSubmitDcb(&packet) == 0, "CLEAR_STATE submit failed");
    }
    AgcDriverWaitIdle_nid_postfix();
}

std::vector<std::uint32_t> makePacket(std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    std::vector<std::uint32_t> result{0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u)};
    result.insert(result.end(), payload);
    return result;
}

std::uint32_t low(const void* pointer) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer)); }
std::uint32_t high(const void* pointer) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer) >> 32u); }

std::vector<std::uint32_t> joinPackets(std::initializer_list<std::vector<std::uint32_t>> packets) {
    std::vector<std::uint32_t> words;
    for (const auto& packet : packets) words.insert(words.end(), packet.begin(), packet.end());
    return words;
}

// WRITE_DATA of one dword to memory (5 dwords).
std::vector<std::uint32_t> writeWord(std::uint32_t& target, std::uint32_t value) {
    return makePacket(0x37, {0x00100200, low(&target), high(&target), value});
}

void submitWords(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&packet) == 0, "conditional submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

void testConditionalExecution() {
    alignas(4) static std::uint32_t condition = 0, guarded = 0, after = 0, nestedTarget = 0;
    const auto conditional = [&](std::uint32_t words) { return makePacket(0x22, {low(&condition), high(&condition), 0, words}); };
    // A zero condition skips the guarded packet; the next one runs.
    auto words = joinPackets({conditional(5), writeWord(guarded, 1), writeWord(after, 2)});
    submitWords(words);
    check(guarded == 0 && after == 2, "COND_EXEC with a zero condition did not skip its range");
    condition = 7;
    after = 0;
    words = joinPackets({conditional(5), writeWord(guarded, 1), writeWord(after, 2)});
    submitWords(words);
    check(guarded == 1 && after == 2, "COND_EXEC with a non-zero condition skipped its range");
    // A guarded INDIRECT_BUFFER is skipped whole.
    condition = 0;
    after = 0;
    auto nested = writeWord(nestedTarget, 3);
    words = joinPackets({conditional(4), makePacket(0x3f, {low(nested.data()), high(nested.data()), static_cast<std::uint32_t>(nested.size()) | 0x0f200000u}), writeWord(after, 4)});
    submitWords(words);
    check(nestedTarget == 0 && after == 4, "a guarded INDIRECT_BUFFER ran or the packet after it did not");
    // Ranges ending inside a packet or past the command buffer are rejected at submission.
    words = joinPackets({conditional(3), writeWord(guarded, 9)});
    Packet inside{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(expectFailure([&] { sceAgcDriverSubmitDcb(&inside); }).find("ends inside a packet") != std::string::npos, "a COND_EXEC range ending inside a packet was accepted");
    words = joinPackets({conditional(9), writeWord(guarded, 9)});
    Packet past{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(expectFailure([&] { sceAgcDriverSubmitDcb(&past); }).find("exceeds its command buffer") != std::string::npos, "a COND_EXEC range past its command buffer was accepted");
    check(guarded == 1, "a rejected COND_EXEC submission executed");
}

void testSubmissions() {
    std::vector<std::thread> producers;
    std::array<std::exception_ptr, 4> errors{};
    for (std::uint32_t i = 0; i < errors.size(); ++i) {
        producers.emplace_back([&, i] {
            try {
                for (std::uint32_t j = 0; j < 100; ++j) {
                    std::array<std::uint32_t, 5> words{0xc0017600, 0x240, j, 0xc0001000, 0};
                    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
                    if (i == 0) check(sceAgcDriverSubmitDcb(&packet) == 0, "DCB submit failed");
                    else if (i == 1) check(sceAgcDriverAgrSubmitDcb(&packet) == 0, "AGR submit failed");
                    else check(sceAgcDriverSubmitAcb(i == 2 ? 0x20 : 0x57, &packet) == 0, "ACB submit failed");
                    words.fill(0xffffffffu);
                }
            } catch (...) {
                errors[i] = std::current_exception();
            }
        });
    }
    for (auto& producer : producers) producer.join();
    for (auto& error : errors) if (error) std::rethrow_exception(error);
    AgcDriverWaitIdle_nid_postfix();
    Packet empty{};
    check(sceAgcDriverSubmitDcb(&empty) == 0, "empty submit failed");
    AgcDriverWaitIdle_nid_postfix();
}

void testWorkerFailure() {
    std::array<std::uint32_t, 5> words{0xc0031500, 1, 1, 1, 0x41};
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitAcb(0x21, &packet) == 0, "dispatch was not accepted");
    std::array<std::string, 4> messages;
    std::vector<std::thread> waiters;
    for (auto& message : messages) {
        waiters.emplace_back([&message] { message = expectFailure([] { AgcDriverWaitIdle_nid_postfix(); }); });
    }
    for (auto& waiter : waiters) waiter.join();
    for (const auto& message : messages) check(message.find("required shader register") != std::string::npos, "worker failure was lost");
    check(expectFailure([&] { sceAgcDriverSubmitDcb(&packet); }) == messages[0], "subsequent DCB lost worker failure");
    check(expectFailure([&] { sceAgcDriverAgrSubmitDcb(&packet); }) == messages[0], "subsequent AGR lost worker failure");
    check(expectFailure([&] { sceAgcDriverSubmitAcb(0x20, &packet); }) == messages[0], "subsequent ACB lost worker failure");
}

std::array<std::uint32_t, 5> writeData(volatile std::uint32_t* address, std::uint32_t value) {
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    return {0xc0033700, 0x00100200, static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(static_cast<std::uint64_t>(target) >> 32u), value};
}

std::array<std::uint32_t, 7> waitEqual(volatile std::uint32_t* address, std::uint32_t value) {
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    return {0xc0053c00, 0x13, static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(static_cast<std::uint64_t>(target) >> 32u), value, 0xffffffffu, 0x19};
}

void submit(std::uint32_t queue, const std::vector<std::uint32_t>& words) {
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    check((queue == 0 ? sceAgcDriverSubmitDcb(&packet) : sceAgcDriverSubmitAcb(queue, &packet)) == 0, "label submit failed");
}

std::chrono::milliseconds waitFor(volatile std::uint32_t* address, std::uint32_t value, const char* message) {
    const auto start = std::chrono::steady_clock::now();
    while (*address != value) {
        check(std::chrono::steady_clock::now() - start < std::chrono::seconds(10), message);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

template<std::size_t... N>
std::vector<std::uint32_t> commands(const std::array<std::uint32_t, N>&... packets) {
    std::vector<std::uint32_t> words;
    (words.insert(words.end(), packets.begin(), packets.end()), ...);
    return words;
}

// sceAgcDriverSubmitMultiAcbs (upstream 78193c10): compute queues 0x20-0x57 only, the ACBs run in
// order and before a later submission on the same queue.
void testMultiAcbs() {
    alignas(64) static volatile std::uint32_t value = 0, done = 0, gate = 0;
    check(sceAgcDriverSubmitMultiAcbs(0x20, nullptr, nullptr, 0) == 0, "empty multi-ACB submit failed");
    auto write = writeData(&value, 1);
    std::array<std::uint32_t*, 1> addresses{write.data()};
    std::array<std::uint32_t, 1> sizes{static_cast<std::uint32_t>(write.size())};
    expectFailure([&] { sceAgcDriverSubmitMultiAcbs(0x1f, addresses.data(), sizes.data(), 1); });
    expectFailure([&] { sceAgcDriverSubmitMultiAcbs(0x58, addresses.data(), sizes.data(), 1); });
    expectFailure([&] { sceAgcDriverSubmitMultiAcbs(0, addresses.data(), sizes.data(), 1); });
    check(sceAgcDriverSubmitMultiAcbs(0, nullptr, nullptr, 0) == 0, "empty multi-ACB submit checked its queue");
    expectFailure([&] { sceAgcDriverSubmitMultiAcbs(0x20, nullptr, sizes.data(), 1); });
    expectFailure([&] { sceAgcDriverSubmitMultiAcbs(0x20, addresses.data(), nullptr, 1); });
    check(value == 0, "a rejected multi-ACB submit ran a command buffer");
    for (std::uint32_t queue : {0x20u, 0x57u}) {
        value = 0;
        done = 0;
        auto first = writeData(&value, 1);
        auto second = writeData(&value, 2);
        auto third = writeData(&done, 1);
        std::array<std::uint32_t*, 3> buffers{first.data(), second.data(), third.data()};
        std::array<std::uint32_t, 3> lengths{static_cast<std::uint32_t>(first.size()), static_cast<std::uint32_t>(second.size()), static_cast<std::uint32_t>(third.size())};
        check(sceAgcDriverSubmitMultiAcbs(queue, buffers.data(), lengths.data(), 3) == 0, "multi-ACB submit failed");
        waitFor(&done, 1, "the last ACB of a multi-ACB submit never ran");
        check(value == 2, "multi-ACB submit did not run its ACBs in order");
        AgcDriverWaitIdle_nid_postfix();
    }
    done = 0;
    value = 0;
    auto wait = waitEqual(&gate, 1);
    auto finish = writeData(&done, 1);
    std::array<std::uint32_t*, 2> buffers{wait.data(), finish.data()};
    std::array<std::uint32_t, 2> lengths{static_cast<std::uint32_t>(wait.size()), static_cast<std::uint32_t>(finish.size())};
    check(sceAgcDriverSubmitMultiAcbs(0x21, buffers.data(), lengths.data(), 2) == 0, "waiting multi-ACB submit failed");
    submit(0x21, commands(writeData(&value, 1)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(value == 0 && done == 0, "a later submission on the same compute queue overtook a multi-ACB submit");
    gate = 1;
    waitFor(&value, 1, "a submission queued behind a multi-ACB submit never ran");
    check(done == 1, "a multi-ACB submit did not run before a later submission on its queue");
    AgcDriverWaitIdle_nid_postfix();
}

}

void testShaderHeaderAlignment() {
    alignas(256) static const std::array<std::uint32_t, 64> code{0xbf810000};
    alignas(8) static std::array<std::byte, 2 * sizeof(Shader)> storage{};
    Shader shader{};
    shader.file_header = 0x34333231;
    shader.version = 0x18;
    shader.header_size = sizeof(Shader);
    shader.shader_size = sizeof(code);
    shader.code = code.data();
    const auto at = [](std::size_t offset, const Shader& fields) {
        std::memcpy(storage.data() + offset, &fields, sizeof(fields));
        return reinterpret_cast<const Shader*>(storage.data() + offset);
    };
    for (const std::size_t offset : {0, 4, 1}) AgcDriverRegisterShader_nid_postfix(at(offset, shader));
    const auto refused = [](const std::string& message, const char* reason) { check(message.find(reason) != std::string::npos, message.c_str()); };
    refused(expectFailure([] { AgcDriverRegisterShader_nid_postfix(nullptr); }), "null or misaligned address");
    refused(expectFailure([] { AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x1001)); }), "not readable");
    Shader misplaced = shader;
    misplaced.code = code.data() + 1;
    refused(expectFailure([&] { AgcDriverRegisterShader_nid_postfix(at(4, misplaced)); }), "null or misaligned address");
    Shader older = shader;
    older.version = 0x17;
    refused(expectFailure([&] { AgcDriverRegisterShader_nid_postfix(at(1, older)); }), "invalid shader header");
    Shader truncated = shader;
    truncated.header_size = sizeof(Shader) - 4;
    refused(expectFailure([&] { AgcDriverRegisterShader_nid_postfix(at(4, truncated)); }), "smaller than its fixed fields");
}

int main() {
    try {
        testEvents();
        testValidation();
        testClearState();
        testSubmissions();
        testConditionalExecution();
        testMultiAcbs();
        testShaderHeaderAlignment();
        testWorkerFailure();
        check(expectFailure([] { LibcRunShutdown_nid_postfix(); }).find("required shader register") != std::string::npos, "shutdown lost worker failure");
        std::puts("AGC driver submit tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        try { LibcRunShutdown_nid_postfix(); }
        catch (const std::exception& shutdown) { std::fprintf(stderr, "shutdown: %s\n", shutdown.what()); }
        return 1;
    }
}
