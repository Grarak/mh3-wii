using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

/// <summary>
/// Prologue hoisting of ctx-&gt;gqr[I] (audit T-GQR). Pins that the hoisted local is re-read after
/// anything that could write the register (mtspr to GQR0-7, unproven calls), so it stays valid on
/// entry to every block regardless of how control flow reaches it.
/// </summary>
public class GqrPrologueHoistingCodeGenTests
{
    private static string Emit(
        IrFunction function,
        IReadOnlyDictionary<uint, byte>? gqrCalleeWriteMasks = null) =>
        new CxxLinearCodeGenerator().Emit(
            0x80001000u,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            gqrCalleeWriteMasks: gqrCalleeWriteMasks);

    private static IrCall PsqLoad(int index, string address = "r4") =>
        new("f1", "PPC_PsqL", [IrValue.Register(address), IrValue.Imm(0), IrValue.Imm(index)]);

    private static IrCall PsqStore(int index, string address = "r4") =>
        new(string.Empty, "PPC_PsqSt",
            [IrValue.Register(address), IrValue.Register("f1"), IrValue.Imm(0), IrValue.Imm(index)]);

    private static IrFunction Function(string name, params IrInstruction[] instructions) =>
        new(name, "entry", [new IrBasicBlock("entry", instructions)]);

    [Fact]
    public void GenericPsqAccessesReadTheHoistedLocalInsteadOfTheContext()
    {
        var code = Emit(Function("hoisted",
            PsqLoad(0), PsqLoad(0), PsqStore(0), new IrReturn(null)));

        Assert.Contains("[[maybe_unused]] uint32_t mkw_gqr0 = ctx->gqr[0];", code, StringComparison.Ordinal);
        Assert.Equal(1, CountOccurrences(code, "ctx->gqr[0]"));
        Assert.Equal(2, CountOccurrences(code, "PPC_PsqLGqrInline<0u, 0u>(ctx, mkw_gqr0,"));
        Assert.Equal(1, CountOccurrences(code, "PPC_PsqStGqrInline<0u, 0u>(ctx, mkw_gqr0,"));
        Assert.DoesNotContain("PPC_PsqLInline<", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsqStInline<", code, StringComparison.Ordinal);
    }

    [Fact]
    public void EachUsedIndexGetsItsOwnLocalAndUnusedIndicesGetNone()
    {
        var code = Emit(Function("two_indices",
            PsqLoad(0), PsqLoad(5), new IrReturn(null)));

        Assert.Contains("[[maybe_unused]] uint32_t mkw_gqr0 = ctx->gqr[0];", code, StringComparison.Ordinal);
        Assert.Contains("[[maybe_unused]] uint32_t mkw_gqr5 = ctx->gqr[5];", code, StringComparison.Ordinal);
        Assert.DoesNotContain("mkw_gqr1", code, StringComparison.Ordinal);
    }

    [Fact]
    public void MtsprToTheGraphicsQuantizationRegisterReloadsTheLocal()
    {
        var code = Emit(Function("mtspr_reload",
            PsqLoad(3),
            new IrAssign("gqr3", IrValue.Register("r5")),
            PsqLoad(3),
            new IrReturn(null)));

        // Prologue read plus the mtspr's own write, then the reload.
        Assert.Contains("[[maybe_unused]] uint32_t mkw_gqr3 = ctx->gqr[3];", code, StringComparison.Ordinal);
        Assert.Contains("mkw_gqr3 = ctx->gqr[3];", code, StringComparison.Ordinal);
        Assert.Equal(2, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void MtsprToADifferentIndexDoesNotReloadTheOther()
    {
        var code = Emit(Function("unrelated_mtspr",
            PsqLoad(3),
            new IrAssign("gqr6", IrValue.Register("r5")),
            PsqLoad(3),
            new IrReturn(null)));

        Assert.Equal(1, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void GuestCallWithoutAWriteMaskReloadsConservatively()
    {
        var code = Emit(Function("call_reload",
            PsqLoad(3),
            new IrCall(string.Empty, "func_80002000", []),
            PsqLoad(3),
            new IrReturn(null)));

        Assert.Equal(2, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void IndirectCallAlwaysReloads()
    {
        var code = Emit(Function("indirect_reload",
            PsqLoad(3),
            new IrIndirectCall(string.Empty, IrValue.Register("ctr"), []),
            PsqLoad(3),
            new IrReturn(null)));

        Assert.Equal(2, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void CalleeWriteMaskProvingTheRegisterUntouchedSkipsTheReload()
    {
        var code = Emit(
            Function("masked_call",
                PsqLoad(3),
                new IrCall(string.Empty, "func_80002000", []),
                PsqLoad(3),
                new IrReturn(null)),
            gqrCalleeWriteMasks: new Dictionary<uint, byte> { [0x80002000u] = 0b0100_0000 });

        Assert.Equal(1, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void CalleeWriteMaskCoveringTheRegisterStillReloads()
    {
        var code = Emit(
            Function("masked_call_hit",
                PsqLoad(3),
                new IrCall(string.Empty, "func_80002000", []),
                PsqLoad(3),
                new IrReturn(null)),
            gqrCalleeWriteMasks: new Dictionary<uint, byte> { [0x80002000u] = 0b0000_1000 });

        Assert.Equal(2, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void RuntimeHelperCallsDoNotForceAReload()
    {
        var code = Emit(Function("helper_call",
            PsqLoad(3),
            new IrCall("r5", "PPC_Cntlzw", [IrValue.Register("r6")]),
            PsqLoad(3),
            new IrReturn(null)));

        Assert.Equal(1, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void GenericSprWriterTargetingAGqrForcesAReload()
    {
        var code = Emit(Function("write_spr",
            PsqLoad(3),
            new IrCall(string.Empty, "PPC_WriteSpr", [IrValue.Imm(915), IrValue.Register("r5")]),
            PsqLoad(3),
            new IrReturn(null)));

        Assert.Equal(2, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void GenericSprWriterTargetingAnUnrelatedSprDoesNotReload()
    {
        var code = Emit(Function("write_spr_other",
            PsqLoad(3),
            new IrCall(string.Empty, "PPC_WriteSpr", [IrValue.Imm(9), IrValue.Register("r5")]),
            PsqLoad(3),
            new IrReturn(null)));

        Assert.Equal(1, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void SchedulerBoundaryHelpersForceAReload()
    {
        var code = Emit(Function("scheduler_boundary",
            PsqLoad(3),
            new IrCall(string.Empty, "Fiber_YieldToScheduler", []),
            PsqLoad(3),
            new IrReturn(null)));

        Assert.Equal(2, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void AnotherPairedAccessDoesNotForceAReload()
    {
        // PPC_PsqL/PPC_PsqSt read the register; they never write it.
        var code = Emit(Function("psq_only",
            PsqLoad(3), PsqLoad(3), PsqStore(3), new IrReturn(null)));

        Assert.Equal(1, CountOccurrences(code, "mkw_gqr3 = ctx->gqr[3];"));
    }

    [Fact]
    public void StackAddressedAccessesKeepTheStackHelper()
    {
        // There is no GQR-value overload of the stack form, so those sites must
        // keep reading the register through the context.
        var code = Emit(Function("stack_psq",
            new IrBinary("tmp_psq_addr", IrValue.Register("r1"), IrValue.Imm(16), "add"),
            new IrCall("f1", "PPC_PsqL",
                [IrValue.Register("tmp_psq_addr"), IrValue.Imm(0), IrValue.Imm(0)]),
            new IrReturn(null)));

        Assert.DoesNotContain("PPC_PsqLGqrInline", code, StringComparison.Ordinal);
        Assert.DoesNotContain("mkw_gqr0", code, StringComparison.Ordinal);
    }

    [Fact]
    public void ThreeGuardedSitesAreEnoughToVersionTheFunction()
    {
        var instructions = Enumerable.Range(0, 3)
            .Select(_ => (IrInstruction)PsqLoad(5))
            .Append(new IrReturn(null))
            .ToArray();

        var code = new CxxLinearCodeGenerator().Emit(0x80001000u,
            new SsaTransformer().Convert(Function("versioned_three", instructions)),
            new FunctionAbiClassification("versioned_three", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            gqrEntryConstants: new Dictionary<string, uint> { ["gqr5"] = 0x00070007u },
            gqrConstantsRequireRuntimeGuard: true);

        Assert.Contains("template <bool gqr_entry_profile>", code, StringComparison.Ordinal);
        Assert.Contains("versioned_three_gqr_impl<true>(ctx);", code, StringComparison.Ordinal);
        Assert.Contains("versioned_three_gqr_impl<false>(ctx);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("const bool gqr_entry_5_00070007", code, StringComparison.Ordinal);
    }

    [Fact]
    public void TwoGuardedSitesStayOnTheRuntimeGuard()
    {
        var instructions = Enumerable.Range(0, 2)
            .Select(_ => (IrInstruction)PsqLoad(5))
            .Append(new IrReturn(null))
            .ToArray();

        var code = new CxxLinearCodeGenerator().Emit(0x80001000u,
            new SsaTransformer().Convert(Function("unversioned_two", instructions)),
            new FunctionAbiClassification("unversioned_two", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            gqrEntryConstants: new Dictionary<string, uint> { ["gqr5"] = 0x00070007u },
            gqrConstantsRequireRuntimeGuard: true);

        Assert.DoesNotContain("_gqr_impl", code, StringComparison.Ordinal);
        Assert.Contains("const bool gqr_entry_5_00070007", code, StringComparison.Ordinal);
    }

    private static int CountOccurrences(string text, string value)
    {
        var count = 0;
        var index = text.IndexOf(value, StringComparison.Ordinal);
        while (index >= 0)
        {
            ++count;
            index = text.IndexOf(value, index + value.Length, StringComparison.Ordinal);
        }

        return count;
    }
}
