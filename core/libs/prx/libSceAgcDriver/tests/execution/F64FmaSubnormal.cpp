#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Batches = 64;
constexpr std::uint32_t Inputs = 8;
constexpr std::uint32_t Results = 2;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 16> Code{
    0x34020085, 0x34060083, 0xe0381000, 0x80000401, 0xe0381010, 0x80000801, 0xbf8c3f70, 0xd54c000c,
    0x04220d04, 0xe0741000, 0x80010c03, 0xbf810000, 0xbf810000, 0xbf810000, 0xbf810000, 0xbf810000,
};

std::uint64_t g_state = 0x9e3779b97f4a7c15ull;

std::uint64_t Next() {
    g_state ^= g_state << 13u;
    g_state ^= g_state >> 7u;
    g_state ^= g_state << 17u;
    return g_state;
}

std::uint64_t Operand(std::uint32_t exponent) {
    const auto mantissa = Next() & 0x000fffffffffffffull;
    const auto sign = Next() & 0x8000000000000000ull;
    const auto fraction = exponent == 0u && mantissa == 0u ? 1u : mantissa;
    return sign | (static_cast<std::uint64_t>(exponent) << 52u) | fraction;
}

std::uint32_t Exponent(std::uint32_t low, std::uint32_t high) {
    return low + static_cast<std::uint32_t>(Next() % (high - low + 1u));
}

std::uint64_t Bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

double Value(std::uint64_t bits) {
    return std::bit_cast<double>(bits);
}

std::array<std::uint64_t, 3> Case(std::uint32_t index) {
    switch (index % 12u) {
    case 0u: return {Operand(0u), Operand(Exponent(1023u, 1100u)), Operand(Exponent(0u, 60u))};
    case 1u: return {Operand(Exponent(1000u, 1100u)), Operand(0u), Operand(Exponent(0u, 2u))};
    case 2u: return {Operand(0u), Operand(Exponent(1900u, 2046u)), Operand(Exponent(800u, 1100u))};
    case 3u: return {Operand(Exponent(900u, 1200u)), Operand(Exponent(900u, 1200u)), Operand(0u)};
    case 4u: return {Operand(Exponent(1u, 60u)), Operand(Exponent(960u, 1023u)), Operand(Exponent(0u, 4u))};
    case 5u: return {Operand(0u), Operand(0u), Operand(Exponent(0u, 1u))};
    case 6u: return {Operand(Exponent(400u, 600u)), Operand(Exponent(400u, 600u)), Operand(Exponent(0u, 2u))};
    case 7u: return {Operand(0u), Operand(Exponent(1990u, 2046u)), Operand(Exponent(0u, 2046u))};
    case 8u: {
        const auto a = Operand(Exponent(1u, 40u));
        const auto b = Operand(Exponent(970u, 1030u));
        const auto c = Bits(-(Value(a) * Value(b))) ^ (Next() & 0xfu);
        return {a, b, c};
    }
    case 9u: {
        const auto a = Operand(0u);
        const auto b = Operand(Exponent(1020u, 1080u));
        const auto c = Bits(-(Value(a) * Value(b))) ^ (Next() & 0x3u);
        return {a, b, c};
    }
    case 10u: {
        const auto shift = static_cast<int>(Exponent(20u, 40u));
        const auto odd = static_cast<double>((Next() & 0xffffffu) | 1u);
        const auto a = Bits(std::ldexp(odd, -shift)) + (Next() % 3u == 0u ? 1u : 0u);
        const auto b = Bits(std::ldexp((Next() & 1u) != 0u ? 1.0 : -1.0, shift - 1075));
        const auto c = Next() % 4u == 0u ? (Next() & 0x8000000000000001ull) : std::uint64_t{0};
        return {a, b, c};
    }
    default: {
        const auto a = Operand(Exponent(500u, 540u));
        const auto b = Operand(Exponent(480u, 520u));
        const auto target = std::ldexp(1.0, -1022) * ((Next() & 1u) != 0u ? 1.0 : -1.0);
        const auto c = Bits(target - Value(a) * Value(b)) ^ (Next() & 0x7u);
        return {a, b, c};
    }
    }
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x01016facu};
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
    return text;
}

void Run(AgcDriver::VulkanDevice& device) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (std::uint32_t batch = 0; batch < Batches; ++batch) {
            std::array<std::array<std::uint64_t, 3>, Threads> cases{};
            for (std::uint32_t tid = 0; tid < Threads; ++tid) {
                cases[tid] = Case(batch * Threads + tid);
                for (std::uint32_t i = 0; i < 3; ++i) {
                    Input[tid * Inputs + i * 2u] = static_cast<std::uint32_t>(cases[tid][i]);
                    Input[tid * Inputs + i * 2u + 1u] = static_cast<std::uint32_t>(cases[tid][i] >> 32u);
                }
            }
            Run(*device);
            for (std::uint32_t tid = 0; tid < Threads; ++tid) {
                const auto& [a, b, c] = cases[tid];
                const auto expected = std::bit_cast<std::uint64_t>(std::fma(std::bit_cast<double>(a), std::bit_cast<double>(b), std::bit_cast<double>(c)));
                const auto actual = Output[tid * Results] | (static_cast<std::uint64_t>(Output[tid * Results + 1u]) << 32u);
                Require(actual == expected, "f64 fma subnormal: fma(" + Hex(a) + ", " + Hex(b) + ", " + Hex(c) + ") is " + Hex(actual) + ", expected " + Hex(expected));
            }
        }
        std::puts("f64 fma subnormal tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
