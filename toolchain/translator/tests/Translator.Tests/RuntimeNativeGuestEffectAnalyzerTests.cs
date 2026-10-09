using System;
using System.IO;
using Translator.Core.Analysis;
using Xunit;

namespace Translator.Tests;

public sealed class RuntimeNativeGuestEffectAnalyzerTests
{
    [Fact]
    public void TypedStubDerivesArgumentsAndReturnWithoutContextFence()
    {
        using var fixture = new SourceFixture("""
            extern "C" uint32_t Plain(uint32_t first, float second) { return first; }
            PPC_NATIVE_OVERRIDE(80001234, Plain, uint32_t, (uint32_t first, float second), (first, second));
            """);

        var effects = RuntimeNativeGuestEffectAnalyzer.AnalyzeDirectory(fixture.Directory);
        var contract = effects.Contracts[0x80001234u];
        Assert.Equal(1u << 3, contract.GprReadBeforeWriteMask);
        Assert.Equal(1u << 1, contract.FprReadBeforeWriteMask);
        Assert.Equal(1u << 3, contract.GprPossibleWriteMask);
        Assert.False(contract.HasFullSynchronizationFence);
        Assert.Contains(0x80001234u, effects.PreciseContracts);
    }

    [Fact]
    public void DirectContextNativeDerivesConstantRegisterReadsAndWrites()
    {
        using var fixture = new SourceFixture("""
            extern "C" void Matrix(CpuContext* ctx) {
                const auto a = ctx->gpr[3];
                const auto b = ctx->gpr[4];
                ctx->gpr[6] = a + b;
            }
            REGISTER_NATIVE_FUNCTION(0x80005678, Matrix);
            """);

        var contract = RuntimeNativeGuestEffectAnalyzer.AnalyzeDirectory(fixture.Directory).Contracts[0x80005678u];
        Assert.Equal((1u << 3) | (1u << 4), contract.GprReadBeforeWriteMask);
        Assert.Equal(1u << 6, contract.GprPossibleWriteMask);
        Assert.False(contract.HasFullSynchronizationFence);
    }

    [Fact]
    public void EscapedContextAndSchedulerCallsRemainExplicitFullBoundaries()
    {
        using var fixture = new SourceFixture("""
            extern "C" void Sleep(CpuContext* ctx) {
                Fiber::YieldToScheduler(ctx);
            }
            REGISTER_NATIVE_FUNCTION(0x80009ABC, Sleep);
            """);

        var contract = RuntimeNativeGuestEffectAnalyzer.AnalyzeDirectory(fixture.Directory).Contracts[0x80009ABCu];
        Assert.True(contract.HasFullSynchronizationFence);
        Assert.True((contract.BoundaryFlags & GuestCallBoundaryFlags.CanSuspend) != 0);
        Assert.Contains(0x80009ABCu,
            RuntimeNativeGuestEffectAnalyzer.AnalyzeDirectory(fixture.Directory).ConservativeContracts);
    }

    [Fact]
    public void TypedStubReachingForAmbientContextIsAFullFence()
    {
        // A typed stub's contract comes from its host signature alone. This one
        // is shaped exactly like 0x801AAD7C (__OSGetSystemTime): declared to
        // return a single uint32_t, but it publishes a 64-bit result by storing
        // r3 and r4 through the ambient context. Deriving "writes r3" from the
        // signature would let a narrowed call boundary skip reloading r4.
        using var fixture = new SourceFixture("""
            extern "C" uint32_t GetSystemTime(uint32_t low, uint32_t high) {
                const uint64_t now = ReadSystemTime();
                if (CpuContext* ctx = CurrentCpuContext()) {
                    ctx->gpr[3] = static_cast<uint32_t>(now >> 32);
                    ctx->gpr[4] = static_cast<uint32_t>(now);
                }
                return static_cast<uint32_t>(now >> 32);
            }
            PPC_NATIVE_OVERRIDE(801AAD7C, GetSystemTime, uint32_t, (uint32_t low, uint32_t high), (low, high));
            """);

        var effects = RuntimeNativeGuestEffectAnalyzer.AnalyzeDirectory(fixture.Directory);
        var contract = effects.Contracts[0x801AAD7Cu];
        Assert.True(contract.HasFullSynchronizationFence);
        Assert.Equal(uint.MaxValue, contract.GprPossibleWriteMask);
        Assert.Contains(0x801AAD7Cu, effects.ConservativeContracts);
    }

    private sealed class SourceFixture : IDisposable
    {
        public SourceFixture(string source)
        {
            Directory = Path.Combine(Path.GetTempPath(), $"mkw-native-effects-{Guid.NewGuid():N}");
            System.IO.Directory.CreateDirectory(Directory);
            File.WriteAllText(Path.Combine(Directory, "fixture.cpp"), source);
        }

        public string Directory { get; }

        public void Dispose() => System.IO.Directory.Delete(Directory, recursive: true);
    }
}
