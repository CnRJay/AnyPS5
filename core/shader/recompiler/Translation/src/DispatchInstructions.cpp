#include <cstdio>
#include "Translation/DispatchInstructions.hpp"
#include "Translation/TranslationContext.hpp"
#include "Recompiler.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ShaderRecompiler {

void DispatchInstruction(IrBuilder& builder, const RdnaInstruction& instruction, const ControlFlowGraph& cfg, const TranslateOptions& options) {
    throw std::runtime_error("DispatchInstruction not implemented");
}

void TranslationContext::TranslateInstruction(const RdnaInstruction& decoded) {
    RdnaInstruction instruction = decoded;
    instruction.destination = destinationOperand(decoded);
    currentOpcode = instruction.op;
    currentProgramCounter = instruction.programCounter;
    if (instruction.op == RdnaOpcode::Unknown || instruction.op == RdnaOpcode::Count) {
        throw std::runtime_error("decoded opcode has no IR translation at pc " + std::to_string(instruction.programCounter));
    }
    if (instruction.op == RdnaOpcode::Unsupported) {
        throw std::runtime_error(instruction.unsupportedReason.empty() ? "unsupported decoded instruction at pc " + std::to_string(instruction.programCounter) : std::string(instruction.unsupportedReason));
    }
    bool translated = false;
    switch (instruction.family) {
        case RdnaInstructionFamily::SOP1:
        case RdnaInstructionFamily::SOP2:
        case RdnaInstructionFamily::SOPK:
        case RdnaInstructionFamily::SOPC:
        case RdnaInstructionFamily::SOPP:
            translated = emitScalar(instruction);
            break;
        case RdnaInstructionFamily::VOP1:
        case RdnaInstructionFamily::VOP2:
        case RdnaInstructionFamily::VOP3:
        case RdnaInstructionFamily::VOP3P:
        case RdnaInstructionFamily::VOPC:
            translated = emitVector(instruction);
            break;
        case RdnaInstructionFamily::SMEM:
        case RdnaInstructionFamily::MUBUF:
        case RdnaInstructionFamily::MTBUF:
        case RdnaInstructionFamily::FLAT:
        case RdnaInstructionFamily::DS:
        case RdnaInstructionFamily::MIMG:
            translated = emitMemory(instruction);
            break;
        case RdnaInstructionFamily::VINTRP:
            translated = emitInterpolation(instruction);
            break;
        case RdnaInstructionFamily::EXP:
            eXP(instruction);
            translated = true;
            break;
        default:
            break;
    }
    if (!translated) {
        throw std::runtime_error("opcode has no IR translation at pc " + std::to_string(instruction.programCounter));
    }
    if (const DebugProbe probe = DebugProbeConfig(); probe.enabled && probe.byInstruction) {
        const auto index = NextDebugProbeInstruction();
        if (index == probe.sample && instruction.destination.kind == RdnaOperandKind::VectorRegister) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) std::fprintf(stderr, "[shader-probe] instruction %u pc 0x%x opcode %u dest v%u\n", index, instruction.programCounter, static_cast<unsigned>(instruction.op), instruction.destination.reg + probe.component);
            RdnaOperand source = instruction.destination;
            source.reg += probe.component;
            RdnaOperand target{};
            target.kind = RdnaOperandKind::VectorRegister;
            target.reg = 255u;
            writeOperand(target, &readRawU32(source).Value());
        }
    } else if (const DebugProbe probe = DebugProbeConfig(); probe.enabled && instruction.programCounter == probe.programCounter) {
        RdnaOperand source{};
        source.kind = RdnaOperandKind::VectorRegister;
        source.reg = probe.vgpr;
        RdnaOperand target{};
        target.kind = RdnaOperandKind::VectorRegister;
        target.reg = 255u;
        writeOperand(target, &readRawU32(source).Value());
    }
}

namespace {
std::atomic<bool> g_debugProbeActive{false};
}

std::atomic<std::uint32_t> g_debugProbeSamples{0};
std::atomic<std::uint32_t> g_debugProbeInstructions{0};

void SetDebugProbeActive(bool active) {
    g_debugProbeActive.store(active);
    g_debugProbeSamples.store(0);
    g_debugProbeInstructions.store(0);
}

std::uint32_t NextDebugProbeSample() {
    return g_debugProbeSamples.fetch_add(1);
}

std::uint32_t NextDebugProbeInstruction() {
    return g_debugProbeInstructions.fetch_add(1);
}

bool DebugProbeActive() {
    return g_debugProbeActive.load();
}

bool RayTracingStrict() {
    static const bool strict = [] {
        const char* text = std::getenv("APS5_RAYTRACING");
        return text != nullptr && std::strcmp(text, "strict") == 0;
    }();
    return strict;
}

bool RayTracingMiss() {
    static const bool miss = [] {
        const char* text = std::getenv("APS5_RAYTRACING");
        return text != nullptr && std::strcmp(text, "miss") == 0;
    }();
    return miss;
}

DebugProbe DebugProbeConfig() {
    static const DebugProbe parsed = [] {
        DebugProbe result;
        const char* text = std::getenv("APS5_PROBE");
        if (text == nullptr) return result;
        char* end = nullptr;
        result.programCounter = static_cast<std::uint32_t>(std::strtoul(text, &end, 16));
        if (end == nullptr || *end != ':') return result;
        result.vgpr = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 10));
        if (end != nullptr && *end == ':') result.shift = static_cast<std::uint32_t>(std::strtoul(end + 1, nullptr, 10));
        result.enabled = result.vgpr < 255u;
        return result;
    }();
    static const DebugProbe sampled = [] {
        DebugProbe result;
        const char* text = std::getenv("APS5_PROBE_SAMPLE");
        const char* instructionText = std::getenv("APS5_PROBE_INST");
        if (text == nullptr) text = instructionText;
        if (text == nullptr) return result;
        result.byInstruction = instructionText != nullptr && text == instructionText;
        char* end = nullptr;
        result.sample = static_cast<std::uint32_t>(std::strtoul(text, &end, 10));
        if (end != nullptr && *end == ':') result.component = static_cast<std::uint32_t>(std::strtoul(end + 1, nullptr, 10));
        result.enabled = true;
        result.bySample = true;
        result.programCounter = 0xffffffffu;
        return result;
    }();
    DebugProbe probe = sampled.enabled ? sampled : parsed;
    probe.enabled = probe.enabled && DebugProbeActive();
    return probe;
}

void DispatchInstruction(TranslationContext& context, const RdnaInstruction& instruction) {
    throw std::runtime_error("DispatchInstruction not implemented");
}

}
