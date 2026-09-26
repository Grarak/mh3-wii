using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using Translator.Core.Loading;
using Xunit;

namespace Translator.Tests;

public class PpcRuntimeHelperTests
{
    private static (int ExitCode, string Output) RunProcess(string fileName, string arguments, TimeSpan? timeout = null)
    {
        using var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = fileName,
                Arguments = arguments,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
                UseShellExecute = false
            }
        };

        process.Start();
        var stdoutTask = process.StandardOutput.ReadToEndAsync();
        var stderrTask = process.StandardError.ReadToEndAsync();
        var exitedInTime = timeout.HasValue
            ? process.WaitForExit((int)timeout.Value.TotalMilliseconds)
            : process.WaitForExit(60000);

        if (!exitedInTime)
        {
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch
            {
                // ignored
            }

            process.WaitForExit();
            var staleOutput = stdoutTask.GetAwaiter().GetResult() + stderrTask.GetAwaiter().GetResult();
            throw new TimeoutException($"Process '{fileName}' timed out. Output before kill:\n{staleOutput}");
        }

        var output = stdoutTask.GetAwaiter().GetResult() + stderrTask.GetAwaiter().GetResult();
        return (process.ExitCode, output);
    }

    private static string CppCompiler()
    {
        var envCompiler = Environment.GetEnvironmentVariable("CXX");
        if (!string.IsNullOrWhiteSpace(envCompiler) && ExecutableExists(envCompiler))
        {
            return envCompiler;
        }

        foreach (var candidate in new[] { "g++", "clang++" })
        {
            if (ExecutableExists(candidate))
            {
                return candidate;
            }
        }

        throw new FileNotFoundException("No C++ compiler found. Set CXX or install g++/clang++ on PATH.");
    }

    private static bool ExecutableExists(string fileName)
    {
        if (Path.IsPathFullyQualified(fileName))
        {
            return File.Exists(fileName);
        }

        var path = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
        var extensions = OperatingSystem.IsWindows()
            ? (Environment.GetEnvironmentVariable("PATHEXT") ?? ".EXE;.BAT;.CMD").Split(';', StringSplitOptions.RemoveEmptyEntries)
            : new[] { string.Empty };

        foreach (var directory in path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            foreach (var extension in extensions)
            {
                var candidate = Path.Combine(directory, fileName);
                if (!candidate.EndsWith(extension, StringComparison.OrdinalIgnoreCase))
                {
                    candidate += extension;
                }

                if (File.Exists(candidate))
                {
                    return true;
                }
            }
        }

        return false;
    }

    private static string RunnerPath(string runnerBasePath)
    {
        if (OperatingSystem.IsWindows())
        {
            var exePath = runnerBasePath + ".exe";
            if (File.Exists(exePath))
            {
                return exePath;
            }
        }

        return runnerBasePath;
    }

    [Fact]
    public void FselAndPsSelMatchDolphinForNaNAndSignedZeroControls()
    {
        var root = ProjectPaths.FindRepositoryRoot();
        var tempRoot = Path.Combine(root, "test_output", "ppc_runtime_helpers");
        Directory.CreateDirectory(tempRoot);

        var harness = new StringBuilder();
        harness.AppendLine("#include <cmath>");
        harness.AppendLine("#include <cstdint>");
        harness.AppendLine("#include <cstring>");
        harness.AppendLine("#include <iostream>");
        harness.AppendLine("#include <limits>");
        harness.AppendLine("#include \"ppc_runtime.h\"");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write8(uint8_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write16(uint16_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write32(uint32_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_WriteFloat(float) {}");
        harness.AppendLine("extern \"C\" void DumpRecentPcTrace(size_t) {}");
        harness.AppendLine("int main() {");
        harness.AppendLine("    const double nan = std::numeric_limits<double>::quiet_NaN();");
        harness.AppendLine("    if (PPC_Fsel(nan, 11.0, 22.0) != 11.0) return 1;");
        harness.AppendLine("    if (PPC_Fsel(-1.0, 11.0, 22.0) != 11.0) return 2;");
        harness.AppendLine("    if (PPC_Fsel(-0.0, 11.0, 22.0) != 22.0) return 3;");
        harness.AppendLine("    if (PPC_Fsel(0.0, 11.0, 22.0) != 22.0) return 4;");
        harness.AppendLine("    PPC_FPR lhs{}; lhs.paired.ps0 = 10.0f; lhs.paired.ps1 = 20.0f;");
        harness.AppendLine("    PPC_FPR rhs{}; rhs.paired.ps0 = -10.0f; rhs.paired.ps1 = -20.0f;");
        harness.AppendLine("    PPC_FPR control{}; control.paired.ps0 = std::numeric_limits<float>::quiet_NaN(); control.paired.ps1 = -0.0f;");
        harness.AppendLine("    PPC_FPR out{}; out.d = PPC_PsSel(lhs.d, control.d, rhs.d);");
        harness.AppendLine("    if (out.paired.ps0 != rhs.paired.ps0) return 5;");
        harness.AppendLine("    if (out.paired.ps1 != lhs.paired.ps1) return 6;");
        harness.AppendLine("    control.paired.ps0 = 1.0f; control.paired.ps1 = -1.0f;");
        harness.AppendLine("    out.d = PPC_PsSel(lhs.d, control.d, rhs.d);");
        harness.AppendLine("    if (out.paired.ps0 != lhs.paired.ps0) return 7;");
        harness.AppendLine("    if (out.paired.ps1 != rhs.paired.ps1) return 8;");
        harness.AppendLine("    std::cout << \"ok\";");
        harness.AppendLine("    return 0;");
        harness.AppendLine("}");

        var harnessPath = Path.Combine(tempRoot, "harness_ppc_runtime_helpers.cpp");
        File.WriteAllText(harnessPath, harness.ToString());

        var runnerBasePath = Path.Combine(tempRoot, "runner_ppc_runtime_helpers");
        var args = TranslatorCppTestHarness.BuildCompileArguments(
            root,
            tempRoot,
            Array.Empty<string>(),
            harnessPath,
            runnerBasePath,
            includeDataSections: false);

        var compiler = CppCompiler();
        var (compileExitCode, compileOutput) = RunProcess(compiler, args, TimeSpan.FromSeconds(60));
        Assert.True(compileExitCode == 0, $"{compiler} failed: {compileOutput}");

        var runnerPath = RunnerPath(runnerBasePath);
        var (runExitCode, runOutput) = RunProcess(runnerPath, "", TimeSpan.FromSeconds(10));
        Assert.True(runExitCode == 0, $"Runner failed ({runExitCode}). Output:\n{runOutput}");
        Assert.Equal("ok", runOutput.Trim());
    }

    [Fact]
    public void InlinePsqFloatFastPathPreservesGuestLaneOrder()
    {
        var root = ProjectPaths.FindRepositoryRoot();
        var tempRoot = Path.Combine(root, "test_output", "ppc_runtime_psq_lane_order");
        Directory.CreateDirectory(tempRoot);

        var harness = new StringBuilder();
        harness.AppendLine("#include <cstdint>");
        harness.AppendLine("#include <cstring>");
        harness.AppendLine("#include <iostream>");
        harness.AppendLine("#include \"memory.h\"");
        harness.AppendLine("#include \"ppc_runtime.h\"");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write8(uint8_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write16(uint16_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write32(uint32_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_WriteFloat(float) {}");
        harness.AppendLine("extern \"C\" void DumpRecentPcTrace(size_t) {}");
        harness.AppendLine("static uint32_t Bits(float value) { uint32_t raw = 0; std::memcpy(&raw, &value, sizeof(raw)); return raw; }");
        harness.AppendLine("static uint64_t DBits(double value) { uint64_t raw = 0; std::memcpy(&raw, &value, sizeof(raw)); return raw; }");
        harness.AppendLine("template <uint32_t GQR> static int CheckKnown(CpuContext& ctx, uint8_t* range, uint32_t base, double source, uint32_t slot) {");
        harness.AppendLine("    ctx.gqr[0] = GQR; const uint32_t addr = base + 256 + slot * 96;");
        harness.AppendLine("    Memory::Write64(addr, 0x81FE1234A5C35A7Eull + slot);");
        harness.AppendLine("    const double pair = PPC_PsqLInline<0u, 0u>(&ctx, addr);");
        harness.AppendLine("    if (DBits(pair) != DBits(PPC_PsqLKnownInline<0u, 0u, GQR>(&ctx, addr))) return 1;");
        harness.AppendLine("    if (DBits(pair) != DBits(PPC_PsqLKnownStackInline<0u, 0u, GQR>(&ctx, addr))) return 2;");
        harness.AppendLine("    if (DBits(pair) != DBits(PPC_PsqLKnownResolvedInline<0u, 0u, GQR>(&ctx, range, addr - base, addr))) return 3;");
        harness.AppendLine("    if (DBits(pair) != DBits(PPC_PsqLKnownResolvedInline<0u, 0u, GQR>(&ctx, nullptr, 0, addr))) return 9;");
        harness.AppendLine("    const double single = PPC_PsqLInline<1u, 0u>(&ctx, addr);");
        harness.AppendLine("    if (DBits(single) != DBits(PPC_PsqLKnownInline<1u, 0u, GQR>(&ctx, addr))) return 4;");
        harness.AppendLine("    if (DBits(single) != DBits(PPC_PsqLKnownStackInline<1u, 0u, GQR>(&ctx, addr))) return 5;");
        harness.AppendLine("    if (DBits(single) != DBits(PPC_PsqLKnownResolvedInline<1u, 0u, GQR>(&ctx, range, addr - base, addr))) return 6;");
        harness.AppendLine("    if (DBits(single) != DBits(PPC_PsqLKnownResolvedInline<1u, 0u, GQR>(&ctx, nullptr, 0, addr))) return 10;");
        harness.AppendLine("    const uint32_t a = addr + 16, b = addr + 32, c = addr + 48, d = addr + 64, e = addr + 80;");
        harness.AppendLine("    Memory::Write64(a, ~0ull); Memory::Write64(b, ~0ull); Memory::Write64(c, ~0ull); Memory::Write64(d, ~0ull); Memory::Write64(e, ~0ull);");
        harness.AppendLine("    PPC_PsqStInline<0u, 0u>(&ctx, a, source);");
        harness.AppendLine("    PPC_PsqStKnownInline<0u, 0u, GQR>(&ctx, b, source);");
        harness.AppendLine("    PPC_PsqStKnownStackInline<0u, 0u, GQR>(&ctx, c, source);");
        harness.AppendLine("    PPC_PsqStKnownResolvedInline<0u, 0u, GQR>(&ctx, range, d - base, d, source);");
        harness.AppendLine("    PPC_PsqStKnownResolvedInline<0u, 0u, GQR>(&ctx, nullptr, 0, e, source);");
        harness.AppendLine("    if (Memory::Read64(a) != Memory::Read64(b) || Memory::Read64(a) != Memory::Read64(c) || Memory::Read64(a) != Memory::Read64(d) || Memory::Read64(a) != Memory::Read64(e)) return 7;");
        harness.AppendLine("    Memory::Write64(a, ~0ull); Memory::Write64(b, ~0ull); Memory::Write64(c, ~0ull); Memory::Write64(d, ~0ull); Memory::Write64(e, ~0ull);");
        harness.AppendLine("    PPC_PsqStInline<1u, 0u>(&ctx, a, source);");
        harness.AppendLine("    PPC_PsqStKnownInline<1u, 0u, GQR>(&ctx, b, source);");
        harness.AppendLine("    PPC_PsqStKnownStackInline<1u, 0u, GQR>(&ctx, c, source);");
        harness.AppendLine("    PPC_PsqStKnownResolvedInline<1u, 0u, GQR>(&ctx, range, d - base, d, source);");
        harness.AppendLine("    PPC_PsqStKnownResolvedInline<1u, 0u, GQR>(&ctx, nullptr, 0, e, source);");
        harness.AppendLine("    if (Memory::Read64(a) != Memory::Read64(b) || Memory::Read64(a) != Memory::Read64(c) || Memory::Read64(a) != Memory::Read64(d) || Memory::Read64(a) != Memory::Read64(e)) return 8;");
        harness.AppendLine("    return 0;");
        harness.AppendLine("}");
        harness.AppendLine("int main() {");
        harness.AppendLine("    Memory::Init(Memory::Config::WiiDefaults());");
        harness.AppendLine("    CpuContext ctx{};");
        harness.AppendLine("    ctx.gpr[1] = 0x817FF000u;");
        harness.AppendLine("    ctx.gqr[0] = 0;");
        harness.AppendLine("    CpuContextScope scope(&ctx);");
        harness.AppendLine("    const auto RefLoadPair = [](uint64_t v) { return (static_cast<uint64_t>(PpcLoadPsqFloatBitsInline(static_cast<uint32_t>(v >> 32))) << 32) | PpcLoadPsqFloatBitsInline(static_cast<uint32_t>(v)); };");
        harness.AppendLine("    const auto RefStorePair = [](uint64_t v) { return (static_cast<uint64_t>(PpcStorePsqFloatBitsInline(static_cast<uint32_t>(v >> 32))) << 32) | PpcStorePsqFloatBitsInline(static_cast<uint32_t>(v)); };");
        harness.AppendLine("    const uint32_t floatEdges[] = { 0u, 0x80000000u, 1u, 0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0x7F800000u, 0x7F800001u, 0x7FC00000u, 0xFF800001u }; ");
        harness.AppendLine("    for (uint32_t hi : floatEdges) for (uint32_t lo : floatEdges) { const uint64_t v = (static_cast<uint64_t>(hi) << 32) | lo; if (PpcLoadPairPsqFloatBitsPackedInline(v) != RefLoadPair(v)) return 90; if (PpcStorePairPsqFloatBitsPackedInline(v) != RefStorePair(v)) return 91; }");
        harness.AppendLine("    uint32_t pairState = 0x7E57A11Du; for (uint32_t i = 0; i < 1000000u; ++i) { pairState = pairState * 1664525u + 1013904223u; const uint64_t hi = pairState; pairState = pairState * 1664525u + 1013904223u; const uint64_t v = (hi << 32) | pairState; if (PpcLoadPairPsqFloatBitsPackedInline(v) != RefLoadPair(v)) return 92; if (PpcStorePairPsqFloatBitsPackedInline(v) != RefStorePair(v)) return 93; }");
        harness.AppendLine("    constexpr uint32_t base = 0x80010000u;");
        harness.AppendLine("    Memory::Write32(base, Bits(12.5f));");
        harness.AppendLine("    Memory::Write32(base + 4, Bits(-7.25f));");
        harness.AppendLine("    auto* range = MemoryInline::ResolveRangeHost(base, 0, 2048, true, true);");
        harness.AppendLine("    if (!range) return 20;");
        harness.AppendLine("    PPC_FPR loaded{};");
        harness.AppendLine("    loaded.d = PPC_PsqLInline<0u, 0u>(base);");
        harness.AppendLine("    if (DBits(loaded.d) != DBits(PPC_PsqLResolvedInline<0u, 0u>(&ctx, range, 0, base))) return 21;");
        harness.AppendLine("    if (loaded.paired.ps0 != 12.5f) return 1;");
        harness.AppendLine("    if (loaded.paired.ps1 != -7.25f) return 2;");
        harness.AppendLine("    PPC_FPR source{};");
        harness.AppendLine("    source.paired.ps0 = 3.5f;");
        harness.AppendLine("    source.paired.ps1 = -9.75f;");
        harness.AppendLine("    PPC_PsqStInline<0u, 0u>(base + 8, source.d);");
        harness.AppendLine("    if (Memory::Read32(base + 8) != Bits(3.5f)) return 3;");
        harness.AppendLine("    if (Memory::Read32(base + 12) != Bits(-9.75f)) return 4;");
        harness.AppendLine("    PPC_PsqStResolvedInline<0u, 0u>(&ctx, range, 24, base + 24, source.d);");
        harness.AppendLine("    if (Memory::Read32(base + 24) != Bits(3.5f) || Memory::Read32(base + 28) != Bits(-9.75f)) return 22;");
        harness.AppendLine("    Memory::Write32(base + 16, 0xDEADBEEFu);");
        harness.AppendLine("    PPC_PsqStInline<1u, 0u>(base + 16, source.d);");
        harness.AppendLine("    if (Memory::Read32(base + 16) != Bits(3.5f)) return 5;");
        harness.AppendLine("    Memory::Write32(base + 20, Bits(22.0f));");
        harness.AppendLine("    loaded.d = PPC_PsqLInline<1u, 0u>(base + 20);");
        harness.AppendLine("    if (loaded.paired.ps0 != 22.0f) return 6;");
        harness.AppendLine("    if (loaded.paired.ps1 != 1.0f) return 7;");
        harness.AppendLine("    const uint32_t gqrs[] = { 0x00040004u, 0x00050005u, 0x00060006u, 0x00070007u, 0x3D043D04u };");
        harness.AppendLine("    for (uint32_t n = 0; n < 5; ++n) {");
        harness.AppendLine("        ctx.gqr[0] = gqrs[n];");
        harness.AppendLine("        const uint32_t input = base + 40 + n * 8;");
        harness.AppendLine("        Memory::Write64(input, 0x81FE1234A5C35A7Eu + n);");
        harness.AppendLine("        const double ordinary = PPC_PsqLInline<0u, 0u>(&ctx, input);");
        harness.AppendLine("        const double resolved = PPC_PsqLResolvedInline<0u, 0u>(&ctx, range, 40 + n * 8, input);");
        harness.AppendLine("        if (DBits(ordinary) != DBits(resolved)) return 30 + n;");
        harness.AppendLine("        const uint32_t a = base + 88 + n * 8; const uint32_t b = base + 128 + n * 8;");
        harness.AppendLine("        Memory::Write64(a, 0xCCCCCCCCCCCCCCCCull); Memory::Write64(b, 0xCCCCCCCCCCCCCCCCull);");
        harness.AppendLine("        PPC_PsqStInline<0u, 0u>(&ctx, a, source.d);");
        harness.AppendLine("        PPC_PsqStResolvedInline<0u, 0u>(&ctx, range, 128 + n * 8, b, source.d);");
        harness.AppendLine("        if (Memory::Read64(a) != Memory::Read64(b)) return 40 + n;");
        harness.AppendLine("    }");
        harness.AppendLine("    if (const int e = CheckKnown<0x00000000u>(ctx, range, base, source.d, 0)) return 100 + e;");
        harness.AppendLine("    if (const int e = CheckKnown<0x00040004u>(ctx, range, base, source.d, 1)) return 110 + e;");
        harness.AppendLine("    if (const int e = CheckKnown<0x00050005u>(ctx, range, base, source.d, 2)) return 120 + e;");
        harness.AppendLine("    if (const int e = CheckKnown<0x00060006u>(ctx, range, base, source.d, 3)) return 130 + e;");
        harness.AppendLine("    if (const int e = CheckKnown<0x00070007u>(ctx, range, base, source.d, 4)) return 140 + e;");
        harness.AppendLine("    if (const int e = CheckKnown<0x3D043D04u>(ctx, range, base, source.d, 5)) return 150 + e;");
        harness.AppendLine("    if (const int e = CheckKnown<0x05050505u>(ctx, range, base, source.d, 6)) return 160 + e;");
        harness.AppendLine("    if (const int e = CheckKnown<0x21062106u>(ctx, range, base, source.d, 7)) return 170 + e;");
        harness.AppendLine("    if (const int e = CheckKnown<0x0C070C07u>(ctx, range, base, source.d, 8)) return 180 + e;");
        harness.AppendLine("    std::cout << \"ok\";");
        harness.AppendLine("    return 0;");
        harness.AppendLine("}");

        var harnessPath = Path.Combine(tempRoot, "harness_psq_lane_order.cpp");
        File.WriteAllText(harnessPath, harness.ToString());

        var runnerBasePath = Path.Combine(tempRoot, "runner_psq_lane_order");
        var args = TranslatorCppTestHarness.BuildCompileArguments(
            root,
            tempRoot,
            Array.Empty<string>(),
            harnessPath,
            runnerBasePath,
            includeDataSections: false);

        var compiler = CppCompiler();
        var (compileExitCode, compileOutput) = RunProcess(compiler, args, TimeSpan.FromSeconds(60));
        Assert.True(compileExitCode == 0, $"{compiler} failed: {compileOutput}");

        var runnerPath = RunnerPath(runnerBasePath);
        var (runExitCode, runOutput) = RunProcess(runnerPath, "", TimeSpan.FromSeconds(10));
        Assert.True(runExitCode == 0, $"Runner failed ({runExitCode}). Output:\n{runOutput}");
        Assert.Equal("ok", runOutput.Trim());
    }

    [Fact]
    public void KnownU8Scale61PairedStoreMatchesExactQuantizationOnFastAndColdPaths()
    {
        var root = ProjectPaths.FindRepositoryRoot();
        var tempRoot = Path.Combine(root, "test_output", "ppc_runtime_psq_u8_scale61");
        Directory.CreateDirectory(tempRoot);

        var harness = new StringBuilder();
        harness.AppendLine("#include <cmath>");
        harness.AppendLine("#include <cstdint>");
        harness.AppendLine("#include <cstring>");
        harness.AppendLine("#include <iostream>");
        harness.AppendLine("#include <limits>");
        harness.AppendLine("#include \"memory.h\"");
        harness.AppendLine("#include \"ppc_runtime.h\"");
        harness.AppendLine("static uint16_t g_fifo16 = 0; static uint32_t g_fifo16Count = 0;");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write8(uint8_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write16(uint16_t value) { g_fifo16 = value; ++g_fifo16Count; }");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write32(uint32_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_WriteFloat(float) {}");
        harness.AppendLine("extern \"C\" void DumpRecentPcTrace(size_t) {}");
        harness.AppendLine("static uint16_t Reference(double value) {");
        harness.AppendLine("    PPC_FPR fpr{}; fpr.d = value;");
        harness.AppendLine("    return static_cast<uint16_t>((static_cast<uint16_t>(PpcQuantizePsqU8Scale61Inline(fpr.paired.ps0)) << 8) | PpcQuantizePsqU8Scale61Inline(fpr.paired.ps1));");
        harness.AppendLine("}");
        harness.AppendLine("static float FromBits(uint32_t bits) { float value; std::memcpy(&value, &bits, sizeof(value)); return value; }");
        harness.AppendLine("int main() {");
        harness.AppendLine("    const float edges[] = { -std::numeric_limits<float>::infinity(), -2040.0f, -1.0f, -0.0f, 0.0f,");
        harness.AppendLine("        std::nextafter(0.0f, 1.0f), 7.999f, 8.0f, 2039.0f, std::nextafter(2040.0f, 0.0f), 2040.0f,");
        harness.AppendLine("        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), FromBits(0x7F800001u) };");
        harness.AppendLine("    for (float ps0 : edges) for (float ps1 : edges) {");
        harness.AppendLine("        PPC_FPR pair{}; pair.paired.ps0 = ps0; pair.paired.ps1 = ps1;");
        harness.AppendLine("        if (PpcQuantizePairPsqU8Scale61PackedInline(pair.d) != Reference(pair.d)) return 1;");
        harness.AppendLine("    }");
        harness.AppendLine("    uint32_t state = 0xC001D00Du;");
        harness.AppendLine("    for (uint32_t i = 0; i < 1000000u; ++i) {");
        harness.AppendLine("        state = state * 1664525u + 1013904223u; const uint32_t a = state;");
        harness.AppendLine("        state = state * 1664525u + 1013904223u; const uint32_t b = state;");
        harness.AppendLine("        PPC_FPR pair{}; pair.paired.ps0 = FromBits(a); pair.paired.ps1 = FromBits(b);");
        harness.AppendLine("        if (PpcQuantizePairPsqU8Scale61PackedInline(pair.d) != Reference(pair.d)) return 2;");
        harness.AppendLine("    }");
        harness.AppendLine("    Memory::Init(Memory::Config::WiiDefaults()); CpuContext ctx{}; CpuContextScope scope(&ctx);");
        harness.AppendLine("    constexpr uint32_t base = 0x80010000u; PPC_FPR pair{}; pair.paired.ps0 = 2039.0f; pair.paired.ps1 = 15.9f;");
        harness.AppendLine("    const uint16_t expected = Reference(pair.d);");
        harness.AppendLine("    PPC_PsqStKnownInline<0u, 6u, 0x3D043D04u>(&ctx, base, pair.d);");
        harness.AppendLine("    if (Memory::Read16(base) != expected) return 3;");
        harness.AppendLine("    const uint32_t page = base >> MemoryInline::kPageShift;");
        harness.AppendLine("    const uintptr_t savedBias = MemoryInline::g_fullWritablePageBias[page];");
        harness.AppendLine("    if (savedBias == 0) return 4;");
        harness.AppendLine("    MemoryInline::g_fullWritablePageBias[page] = 0;");
        harness.AppendLine("    PPC_PsqStKnownInline<0u, 6u, 0x3D043D04u>(&ctx, base + 2, pair.d);");
        harness.AppendLine("    MemoryInline::g_fullWritablePageBias[page] = savedBias;");
        harness.AppendLine("    if (Memory::Read16(base + 2) != expected) return 5;");
        harness.AppendLine("    uint8_t* range = MemoryInline::ResolveRangeHost(base, 0, 64, false, true);");
        harness.AppendLine("    if (!range) return 6;");
        harness.AppendLine("    PPC_PsqStKnownResolvedInline<0u, 6u, 0x3D043D04u>(&ctx, range, 4, base + 4, pair.d);");
        harness.AppendLine("    if (Memory::Read16(base + 4) != expected) return 7;");
        harness.AppendLine("    PPC_PsqStKnownStackInline<0u, 6u, 0x3D043D04u>(&ctx, base + 6, pair.d);");
        harness.AppendLine("    if (Memory::Read16(base + 6) != expected) return 8;");
        harness.AppendLine("    PPC_FPR exceptional{}; exceptional.paired.ps0 = std::numeric_limits<float>::quiet_NaN(); exceptional.paired.ps1 = std::numeric_limits<float>::infinity();");
        harness.AppendLine("    PPC_PsqStKnownInline<0u, 6u, 0x3D043D04u>(&ctx, 0xCC008000u, exceptional.d);");
        harness.AppendLine("    if (g_fifo16Count != 1 || g_fifo16 != Reference(exceptional.d)) return 9;");
        harness.AppendLine("    std::cout << \"ok\"; return 0;");
        harness.AppendLine("}");

        var harnessPath = Path.Combine(tempRoot, "harness_psq_u8_scale61.cpp");
        File.WriteAllText(harnessPath, harness.ToString());

        var runnerBasePath = Path.Combine(tempRoot, "runner_psq_u8_scale61");
        var args = TranslatorCppTestHarness.BuildCompileArguments(
            root,
            tempRoot,
            Array.Empty<string>(),
            harnessPath,
            runnerBasePath,
            includeDataSections: false);

        var compiler = CppCompiler();
        var (compileExitCode, compileOutput) = RunProcess(compiler, args, TimeSpan.FromSeconds(60));
        Assert.True(compileExitCode == 0, $"{compiler} failed: {compileOutput}");

        var runnerPath = RunnerPath(runnerBasePath);
        var (runExitCode, runOutput) = RunProcess(runnerPath, "", TimeSpan.FromSeconds(10));
        Assert.True(runExitCode == 0, $"Runner failed ({runExitCode}). Output:\n{runOutput}");
        Assert.Equal("ok", runOutput.Trim());
    }

    [Fact]
    public void InlinePairedSingleArithmeticPreservesLaneOrder()
    {
        var root = ProjectPaths.FindRepositoryRoot();
        var tempRoot = Path.Combine(root, "test_output", "ppc_runtime_ps_arithmetic");
        Directory.CreateDirectory(tempRoot);

        var harness = new StringBuilder();
        harness.AppendLine("#include <cmath>");
        harness.AppendLine("#include <cstdint>");
        harness.AppendLine("#include <iostream>");
        harness.AppendLine("#include <limits>");
        harness.AppendLine("#include \"ppc_runtime.h\"");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write8(uint8_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write16(uint16_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write32(uint32_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_WriteFloat(float) {}");
        harness.AppendLine("extern \"C\" void DumpRecentPcTrace(size_t) {}");
        harness.AppendLine("static bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }");
        harness.AppendLine("static int Check(PPC_FPR f, float ps0, float ps1, int code) {");
        harness.AppendLine("    if (!Near(f.paired.ps0, ps0)) return code;");
        harness.AppendLine("    if (!Near(f.paired.ps1, ps1)) return code + 1;");
        harness.AppendLine("    return 0;");
        harness.AppendLine("}");
        harness.AppendLine("static uint64_t Bits(double value) {");
        harness.AppendLine("    uint64_t bits = 0;");
        harness.AppendLine("    std::memcpy(&bits, &value, sizeof(bits));");
        harness.AppendLine("    return bits;");
        harness.AppendLine("}");
        harness.AppendLine("static double FromBits(uint64_t bits) {");
        harness.AppendLine("    double value = 0.0;");
        harness.AppendLine("    std::memcpy(&value, &bits, sizeof(value));");
        harness.AppendLine("    return value;");
        harness.AppendLine("}");
        harness.AppendLine("static float FloatFromBits(uint32_t bits) {");
        harness.AppendLine("    float value = 0.0f;");
        harness.AppendLine("    std::memcpy(&value, &bits, sizeof(value));");
        harness.AppendLine("    return value;");
        harness.AppendLine("}");
        harness.AppendLine("int main() {");
        harness.AppendLine("    PPC_FPR a{}; a.paired.ps0 = 2.0f; a.paired.ps1 = -4.0f;");
        harness.AppendLine("    PPC_FPR b{}; b.paired.ps0 = 8.0f; b.paired.ps1 = 0.5f;");
        harness.AppendLine("    PPC_FPR c{}; c.paired.ps0 = 3.0f; c.paired.ps1 = 10.0f;");
        harness.AppendLine("    PPC_FPR out{};");
        harness.AppendLine("    out.d = PPC_PsAddInline(a.d, b.d); if (int rc = Check(out, 10.0f, -3.5f, 1)) return rc;");
        harness.AppendLine("    out.d = PPC_PsSubInline(a.d, b.d); if (int rc = Check(out, -6.0f, -4.5f, 3)) return rc;");
        harness.AppendLine("    out.d = PPC_PsDivInline(a.d, b.d); if (int rc = Check(out, 0.25f, -8.0f, 21)) return rc;");
        harness.AppendLine("    out.d = PPC_PsMulInline(a.d, b.d); if (int rc = Check(out, 16.0f, -2.0f, 5)) return rc;");
        harness.AppendLine("    out.d = PPC_PsMaddInline(a.d, b.d, c.d); if (int rc = Check(out, 19.0f, 8.0f, 7)) return rc;");
        harness.AppendLine("    out.d = PPC_PsMsubInline(a.d, b.d, c.d); if (int rc = Check(out, 13.0f, -12.0f, 9)) return rc;");
        harness.AppendLine("    out.d = PPC_PsNmsubInline(a.d, b.d, c.d); if (int rc = Check(out, -13.0f, 12.0f, 11)) return rc;");
        harness.AppendLine("    out.d = PPC_PsMuls0Inline(a.d, b.d); if (int rc = Check(out, 16.0f, -32.0f, 13)) return rc;");
        harness.AppendLine("    out.d = PPC_PsMuls1Inline(a.d, b.d); if (int rc = Check(out, 1.0f, -2.0f, 15)) return rc;");
        harness.AppendLine("    if (PPC_Divwu(0x12345678u, 0u) != 0u) return 17;");
        harness.AppendLine("    if (PPC_Divw(123, 0) != 0) return 18;");
        harness.AppendLine("    if (PPC_Divw(-123, 0) != -1) return 19;");
        harness.AppendLine("    if (PPC_Divw(INT32_MIN, -1) != -1) return 20;");
        harness.AppendLine("    // Exact Gekko estimate bits, including interpolation and exponent parity.");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(1.0)) != 0x3FEFFE8000000000ULL) return 23;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(2.0)) != 0x3FE69FA000000000ULL) return 24;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(4.0)) != 0x3FDFFE8000000000ULL) return 25;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(3.14159265358979323846)) != 0x3FE20DD740000000ULL) return 26;");
        harness.AppendLine("    // Subnormal normalization must extend the encoded exponent below zero.");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(FromBits(0x0000000000000001ULL))) != 0x617FFE8000000000ULL) return 27;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(FromBits(0x000FFFFFFFFFFFFFULL))) != 0x5FE000082C000000ULL) return 28;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(FromBits(0x0010000000000000ULL))) != 0x5FDFFE8000000000ULL) return 29;");
        harness.AppendLine("    // These values drive the architectural zero/invalid/NaN paths; the helper's");
        harness.AppendLine("    // contract is result bits, while instruction emission owns FPSCR effects.");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(0.0)) != 0x7FF0000000000000ULL) return 30;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(-0.0)) != 0xFFF0000000000000ULL) return 31;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(std::numeric_limits<double>::infinity())) != 0x0000000000000000ULL) return 32;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(-std::numeric_limits<double>::infinity())) != 0x7FF8000000000000ULL) return 33;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(-4.0)) != 0x7FF8000000000000ULL) return 34;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(FromBits(0x7FF0000000000001ULL))) != 0x7FF8000000000001ULL) return 35;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(FromBits(0xFFF0000000000123ULL))) != 0xFFF8000000000123ULL) return 36;");
        harness.AppendLine("    if (Bits(PPC_Frsqrte(FromBits(0x7FF8000000001234ULL))) != 0x7FF8000000001234ULL) return 37;");
        harness.AppendLine("    // Paired multiply-add has one float32 rounding point, not a rounded");
        harness.AppendLine("    // float multiply followed by a second rounded add.");
        harness.AppendLine("    PPC_FPR edgeA{}; edgeA.paired.ps0 = FloatFromBits(0x42480000u); edgeA.paired.ps1 = edgeA.paired.ps0;");
        harness.AppendLine("    PPC_FPR edgeC{}; edgeC.paired.ps0 = FloatFromBits(0xBC88CC38u); edgeC.paired.ps1 = edgeC.paired.ps0;");
        harness.AppendLine("    PPC_FPR edgeB{}; edgeB.paired.ps0 = FloatFromBits(0x1B1C72A0u); edgeB.paired.ps1 = edgeB.paired.ps0;");
        harness.AppendLine("    out.d = PPC_PsMaddInline(edgeA.d, edgeC.d, edgeB.d);");
        harness.AppendLine("    if (PpcBitCastToU32Inline(out.paired.ps0) != 0xBF55BF17u || PpcBitCastToU32Inline(out.paired.ps1) != 0xBF55BF17u) return 38;");
        harness.AppendLine("    if (PpcBitCastToU32Inline(static_cast<float>(PpcFmaddsInline(edgeA.paired.ps0, edgeC.paired.ps0, edgeB.paired.ps0))) != 0xBF55BF17u) return 39;");
        harness.AppendLine("    // Double fused arithmetic must retain the product error cancelled by B.");
        harness.AppendLine("    const double fmaA = 1.0 + 0x1p-27;");
        harness.AppendLine("    const double fmaC = 1.0 - 0x1p-27;");
        harness.AppendLine("    if (Bits(PpcFmaddInline(fmaA, fmaC, -1.0)) != 0xBC90000000000000ULL) return 40;");
        harness.AppendLine("    // Exact Broadway reciprocal estimates, including lane replication.");
        harness.AppendLine("    PPC_FPR estimateIn{}; estimateIn.paired.ps0 = 1.0f; estimateIn.paired.ps1 = 2.0f;");
        harness.AppendLine("    if (Bits(PPC_Fres(estimateIn.d)) != 0x3F7FF8003F7FF800ULL) return 41;");
        harness.AppendLine("    if (Bits(PPC_PsRes(estimateIn.d)) != 0x3F7FF8003EFFF800ULL) return 42;");
        harness.AppendLine("    estimateIn.paired.ps0 = 1.0f; estimateIn.paired.ps1 = 4.0f;");
        harness.AppendLine("    if (Bits(PPC_PsRsqrte(estimateIn.d)) != 0x3F7FF4003EFFF400ULL) return 43;");
        harness.AppendLine("    estimateIn.paired.ps0 = -0.0f; estimateIn.paired.ps1 = -4.0f;");
        harness.AppendLine("    if (Bits(PPC_PsRsqrte(estimateIn.d)) != 0xFF8000007FC00000ULL) return 44;");
        harness.AppendLine("    // stfs narrows the FPR payload with Gekko's bit conversion, not a host cast.");
        harness.AppendLine("    if (MemoryInline::ConvertPpcDoubleToSingleBits(1.0 + 0x1.8p-23) != 0x3F800001u) return 45;");
        harness.AppendLine("    if (MemoryInline::ConvertPpcDoubleToSingleBits(1.0 + 0x1.8p-24) != 0x3F800000u) return 46;");
        harness.AppendLine("    // FPSCR[NI] flushes single subnormal results while preserving their sign.");
        harness.AppendLine("    CpuContext niCpu{}; niCpu.fpscr = 4u;");
        harness.AppendLine("    { CpuContextScope niScope(&niCpu);");
        harness.AppendLine("      if (PpcBitCastToU32Inline(PpcForceSingleValueInline(0x1p-149)) != 0u) return 47;");
        harness.AppendLine("      if (PpcBitCastToU32Inline(PpcForceSingleValueInline(-0x1p-149)) != 0x80000000u) return 48;");
        harness.AppendLine("    }");
        harness.AppendLine("    std::cout << \"ok\";");
        harness.AppendLine("    return 0;");
        harness.AppendLine("}");

        var harnessPath = Path.Combine(tempRoot, "harness_ps_arithmetic.cpp");
        File.WriteAllText(harnessPath, harness.ToString());

        var runnerBasePath = Path.Combine(tempRoot, "runner_ps_arithmetic");
        var args = TranslatorCppTestHarness.BuildCompileArguments(
            root,
            tempRoot,
            Array.Empty<string>(),
            harnessPath,
            runnerBasePath,
            includeDataSections: false);

        var compiler = CppCompiler();
        var (compileExitCode, compileOutput) = RunProcess(compiler, args, TimeSpan.FromSeconds(60));
        Assert.True(compileExitCode == 0, $"{compiler} failed: {compileOutput}");

        var runnerPath = RunnerPath(runnerBasePath);
        var (runExitCode, runOutput) = RunProcess(runnerPath, "", TimeSpan.FromSeconds(10));
        Assert.True(runExitCode == 0, $"Runner failed ({runExitCode}). Output:\n{runOutput}");
        Assert.Equal("ok", runOutput.Trim());
    }

    [Fact]
    public void NonvolatileFprGuardPreservesPackedPairedAndScalarFprs()
    {
        var root = ProjectPaths.FindRepositoryRoot();
        var tempRoot = Path.Combine(root, "test_output", "ppc_nonvolatile_fpr_guard");
        Directory.CreateDirectory(tempRoot);

        var harness = new StringBuilder();
        harness.AppendLine("#include <cstdint>");
        harness.AppendLine("#include <iostream>");
        harness.AppendLine("#include \"abi_bridge.h\"");
        harness.AppendLine("extern \"C\" void ClobberNonvolatileFprs(CpuContext* ctx) {");
        harness.AppendLine("    ctx->fpr[14].raw = 0x0102030405060708ull;");
        harness.AppendLine("    ctx->fpr[31].raw = 0x8877665544332211ull;");
        harness.AppendLine("}");
        harness.AppendLine("int main() {");
        harness.AppendLine("    CpuContext ctx{};");
        harness.AppendLine("    PPC_FPR saved14{}; saved14.paired.ps0 = 12.5f; saved14.paired.ps1 = -7.25f;");
        harness.AppendLine("    PPC_FPR saved31{}; saved31.d = 3.141592653589793;");
        harness.AppendLine("    PPC_FPR volatile13{}; volatile13.paired.ps0 = 1.0f; volatile13.paired.ps1 = 2.0f;");
        harness.AppendLine("    ctx.fpr[14] = saved14;");
        harness.AppendLine("    ctx.fpr[31] = saved31;");
        harness.AppendLine("    ctx.fpr[13] = volatile13;");
        harness.AppendLine("    {");
        harness.AppendLine("        PpcNonvolatileFprGuard guard(&ctx);");
        harness.AppendLine("        ctx.fpr[14].paired.ps0 = 1000.0f;");
        harness.AppendLine("        ctx.fpr[14].paired.ps1 = 2000.0f;");
        harness.AppendLine("        ctx.fpr[31].raw = 0x123456789ABCDEF0ull;");
        harness.AppendLine("        ctx.fpr[13].paired.ps0 = 9.0f;");
        harness.AppendLine("        ctx.fpr[13].paired.ps1 = 10.0f;");
        harness.AppendLine("    }");
        harness.AppendLine("    if (ctx.fpr[14].raw != saved14.raw) return 1;");
        harness.AppendLine("    if (ctx.fpr[31].raw != saved31.raw) return 2;");
        harness.AppendLine("    if (ctx.fpr[13].raw == volatile13.raw) return 3;");
        harness.AppendLine("    {");
        harness.AppendLine("        PpcNonvolatileFprGuard guard(&ctx, 1u << 31);");
        harness.AppendLine("        ctx.fpr[14].raw = 0x1111111111111111ull;");
        harness.AppendLine("        ctx.fpr[31].raw = 0x2222222222222222ull;");
        harness.AppendLine("    }");
        harness.AppendLine("    if (ctx.fpr[14].raw == saved14.raw) return 4;");
        harness.AppendLine("    if (ctx.fpr[31].raw != saved31.raw) return 5;");
        harness.AppendLine("    ctx.fpr[14] = saved14;");
        harness.AppendLine("    TranslatedFunctionInfo info{};");
        harness.AppendLine("    info.entryPoint = reinterpret_cast<void*>(&ClobberNonvolatileFprs);");
        harness.AppendLine("    info.nonvolatileFprWriteMask = 1u << 31;");
        harness.AppendLine("    if (!TryDispatchResolvedCpuTarget(&info, &ctx)) return 6;");
        harness.AppendLine("    if (ctx.fpr[14].raw == saved14.raw) return 7;");
        harness.AppendLine("    if (ctx.fpr[31].raw != saved31.raw) return 8;");
        harness.AppendLine("    ctx.fpr[14] = saved14;");
        harness.AppendLine("    ctx.fpr[31] = saved31;");
        harness.AppendLine("    info.nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;");
        harness.AppendLine("    if (!TryDispatchResolvedCpuTarget(&info, &ctx)) return 9;");
        harness.AppendLine("    if (ctx.fpr[14].raw != saved14.raw) return 10;");
        harness.AppendLine("    if (ctx.fpr[31].raw != saved31.raw) return 11;");
        harness.AppendLine("    info.nonvolatileFprWriteMask = 0;");
        harness.AppendLine("    if (!TryDispatchResolvedCpuTarget(&info, &ctx)) return 12;");
        harness.AppendLine("    if (ctx.fpr[14].raw == saved14.raw) return 13;");
        harness.AppendLine("    if (ctx.fpr[31].raw == saved31.raw) return 14;");
        harness.AppendLine("    std::cout << \"ok\";");
        harness.AppendLine("    return 0;");
        harness.AppendLine("}");

        var harnessPath = Path.Combine(tempRoot, "harness_nonvolatile_fpr_guard.cpp");
        File.WriteAllText(harnessPath, harness.ToString());

        var runnerBasePath = Path.Combine(tempRoot, "runner_nonvolatile_fpr_guard");
        var args =
            "-std=c++17 " +
            $"\"{harnessPath}\" " +
            $"-I\"{Path.Combine(root, "runtime", "include")}\" " +
            $"-o \"{runnerBasePath}\"";

        var compiler = CppCompiler();
        var (compileExitCode, compileOutput) = RunProcess(compiler, args, TimeSpan.FromSeconds(60));
        Assert.True(compileExitCode == 0, $"{compiler} failed: {compileOutput}");

        var runnerPath = RunnerPath(runnerBasePath);
        var (runExitCode, runOutput) = RunProcess(runnerPath, "", TimeSpan.FromSeconds(10));
        Assert.True(runExitCode == 0, $"Runner failed ({runExitCode}). Output:\n{runOutput}");
        Assert.Equal("ok", runOutput.Trim());
    }

    [Fact]
    public void DirectCpuDispatchSeesModOverrideRegisteredAfterFirstBaseCall()
    {
        var root = ProjectPaths.FindRepositoryRoot();
        var tempRoot = Path.Combine(root, "test_output", "ppc_direct_mod_override_refresh");
        Directory.CreateDirectory(tempRoot);

        var harness = new StringBuilder();
        harness.AppendLine("#include <cstdint>");
        harness.AppendLine("#include <iostream>");
        harness.AppendLine("#include <utility>");
        harness.AppendLine("#include \"abi_bridge.h\"");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write8(uint8_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write16(uint16_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write32(uint32_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_WriteFloat(float) {}");
        harness.AppendLine("extern \"C\" void BaseTranslated(CpuContext* ctx) { ctx->gpr[3] = 0xBACE0001u; }");
        harness.AppendLine("extern \"C\" void ModTranslated(CpuContext* ctx) { ctx->gpr[3] = 0xC0DE0002u; }");
        harness.AppendLine("template <> struct KnownTranslatedCpuCall<0x81234568u> {");
        harness.AppendLine("    static constexpr bool kAvailable = true;");
        harness.AppendLine("    static constexpr uint32_t kNonvolatileFprWriteMask = 0;");
        harness.AppendLine("    static constexpr bool kMayBeOverridden = true;");
        harness.AppendLine("    static constexpr void (*Entry)(CpuContext*) = &BaseTranslated;");
        harness.AppendLine("};");
        harness.AppendLine("static void Register(uint32_t priority, FunctionKind kind, const char* name, void (*entry)(CpuContext*)) {");
        harness.AppendLine("    TranslatedFunctionInfo info{};");
        harness.AppendLine("    info.address = 0x81234568u;");
        harness.AppendLine("    info.name = name;");
        harness.AppendLine("    info.kind = kind;");
        harness.AppendLine("    info.priority = priority;");
        harness.AppendLine("    info.entryPoint = reinterpret_cast<void*>(entry);");
        harness.AppendLine("    info.nonvolatileFprWriteMask = 0;");
        harness.AppendLine("    TranslatedFunctionRegistry::Register(std::move(info));");
        harness.AppendLine("}");
        harness.AppendLine("int main() {");
        harness.AppendLine("    CpuContext ctx{};");
        harness.AppendLine("    Register(0, FunctionKind::BaseTranslated, \"base\", &BaseTranslated);");
        harness.AppendLine("    InvokeDirectCpu<0x81234568u>(&ctx);");
        harness.AppendLine("    if (ctx.gpr[3] != 0xBACE0001u) return 1;");
        harness.AppendLine("    Register(100, FunctionKind::ModTranslated, \"mod\", &ModTranslated);");
        harness.AppendLine("    ctx.gpr[3] = 0;");
        harness.AppendLine("    InvokeDirectCpu<0x81234568u>(&ctx);");
        harness.AppendLine("    if (ctx.gpr[3] != 0xC0DE0002u) return 2;");
        harness.AppendLine("    TranslatedFunctionRegistry::Finalize();");
        harness.AppendLine("    ctx.gpr[3] = 0;");
        harness.AppendLine("    InvokeDirectCpu<0x81234568u>(&ctx);");
        harness.AppendLine("    if (ctx.gpr[3] != 0xC0DE0002u) return 3;");
        harness.AppendLine("    std::cout << \"ok\";");
        harness.AppendLine("    return 0;");
        harness.AppendLine("}");

        var harnessPath = Path.Combine(tempRoot, "harness_direct_mod_override_refresh.cpp");
        File.WriteAllText(harnessPath, harness.ToString());

        var runnerBasePath = Path.Combine(tempRoot, "runner_direct_mod_override_refresh");
        var args = TranslatorCppTestHarness.BuildCompileArguments(
            root,
            tempRoot,
            Array.Empty<string>(),
            harnessPath,
            runnerBasePath,
            includeDataSections: false);

        var compiler = CppCompiler();
        var (compileExitCode, compileOutput) = RunProcess(compiler, args, TimeSpan.FromSeconds(60));
        Assert.True(compileExitCode == 0, $"{compiler} failed: {compileOutput}");

        var runnerPath = RunnerPath(runnerBasePath);
        var (runExitCode, runOutput) = RunProcess(runnerPath, "", TimeSpan.FromSeconds(10));
        Assert.True(runExitCode == 0, $"Runner failed ({runExitCode}). Output:\n{runOutput}");
        Assert.Contains("ok", runOutput);
    }
}
