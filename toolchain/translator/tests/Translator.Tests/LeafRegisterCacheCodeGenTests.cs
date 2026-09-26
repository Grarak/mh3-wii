using System.Collections.Generic;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class LeafRegisterCacheCodeGenTests
{
    // Text-rewritten "cached_*" locals survive only for the state-free clone.
    // Register residency owns every primary body and achieves the same property
    // (a CR access promoted once to a local, reused for every subsequent
    // read/write in the function) directly at emission time, under its own
    // plain-named locals - so that is what these tests exercise.
    private static string Emit(IrFunction function, RepresentationEnvironment types, uint entryPoint) =>
        new CxxLinearCodeGenerator().Emit(
            entryPoint,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            types);

    [Fact]
    public void LeafCachedConditionRegisterBitReadUsesTheResidentHelper()
    {
        // PpcLifter builds the general bc/bcctr condition as raw C++ text
        // containing GetCRBit(ctx, field, bit). When the leaf register cache owns
        // CR, that read has to move onto the local as well: otherwise the branch
        // observes the stale architectural CR while SetCRResident is updating
        // cached_cr, which is a live miscompile in shipped shards.
        var function = new IrFunction(
            "leaf_cached_cr_bit",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrSetCrField(0, IrValue.Register("r3"), IrValue.Register("r4"), false),
                    new IrBranch("raw", "taken", "exit", "((true) && ((GetCRBit(ctx, 0, 0) == true)))")
                }),
                new IrBasicBlock("taken", new IrInstruction[]
                {
                    new IrBinary("r3", IrValue.Register("r3"), IrValue.Imm(1), "add"),
                    new IrReturn(null)
                }),
                new IrBasicBlock("exit", new IrInstruction[] { new IrReturn(null) })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.UInt32,
            ["r4"] = ValueRepresentation.UInt32
        });

        var code = Emit(function, types, 0x8000622Au);

        Assert.Contains("uint32_t cr = ctx->cr;", code, StringComparison.Ordinal);
        Assert.Contains("SetCRResident(cr, xer, 0,", code, StringComparison.Ordinal);
        Assert.Contains("GetCRBitResident(cr, 0, 0)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("GetCRBit(ctx,", code, StringComparison.Ordinal);
        Assert.Contains("    ctx->cr = cr;", code, StringComparison.Ordinal);
    }

    [Fact]
    public void LeafCachedConditionRegisterBitReadWithoutAnyCrWriteStillLocalizesTheRead()
    {
        // A body whose only CR access is the raw condition read must declare the
        // local, use it, and not write CR back: cached_cr is a pure copy there.
        var function = new IrFunction(
            "leaf_cached_cr_bit_read_only",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBranch("raw", "taken", "exit", "((true) && ((GetCRBit(ctx, 1, 2) == true)))")
                }),
                new IrBasicBlock("taken", new IrInstruction[]
                {
                    new IrBinary("r3", IrValue.Register("r3"), IrValue.Imm(1), "add"),
                    new IrReturn(null)
                }),
                new IrBasicBlock("exit", new IrInstruction[] { new IrReturn(null) })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.UInt32
        });

        var code = Emit(function, types, 0x8000622Cu);

        Assert.Contains("uint32_t cr = ctx->cr;", code, StringComparison.Ordinal);
        Assert.Contains("GetCRBitResident(cr, 1, 2)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("GetCRBit(ctx,", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->cr = cr;", code, StringComparison.Ordinal);
    }
}
