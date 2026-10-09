using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Tests;

public sealed class LocalCodeGenOptimizationTests
{
    [Fact]
    public void DeadPureLocalAssignmentsAreRemovedButMemoryReadsRemainObservable()
    {
        var function = new IrFunction("dead_pure_locals", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("dead0", IrValue.Imm(7)),
                new IrBinary("dead1", IrValue.Register("dead0"), IrValue.Imm(5), "add"),
                new IrLoad("dead_load", new IrAddress("r3", 0), 4),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["dead0"] = ValueRepresentation.UInt32,
            ["dead1"] = ValueRepresentation.UInt32,
            ["dead_load"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32
        });

        var code = Emit(function, types, 0x80008010);

        Assert.DoesNotContain("dead0", code, StringComparison.Ordinal);
        Assert.DoesNotContain("dead1", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatRead32(r3)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void FctiwLowWordLoadUsesNativeExtractionAndElidesPrivateStackStore()
    {
        var function = FctiwFunction(includeGuestCallAfterLoad: false);
        var code = Emit(function, FctiwTypes(), 0x80008014);

        Assert.Contains("PPC_FprLowWordInline(f0.d)", code, StringComparison.Ordinal);
        Assert.Contains("r3 = fctiwzword0;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("MemoryInline::FlatWriteFloat64((r1 + -16)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("MemoryInline::FlatRead32((r1 + -12))", code, StringComparison.Ordinal);
    }

    [Fact]
    public void FctiwPrivateStackStoreRemainsVisibleAcrossGuestCall()
    {
        var function = FctiwFunction(includeGuestCallAfterLoad: true);
        var code = Emit(function, FctiwTypes(), 0x80008018);

        Assert.Contains("PPC_FprLowWordInline(f0.d)", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWriteRamFloat64((r1 + -16)", code, StringComparison.Ordinal);
        Assert.Contains("InvokeDirectCpu<0x80001234u>(ctx);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void OverwrittenPairedStackRestoreIsRemoved()
    {
        var function = PairedRestoreFunction(usePairedValue: false);
        var code = Emit(function, PairedRestoreTypes(), 0x8000801C);

        Assert.DoesNotContain("PPC_PsqL", code, StringComparison.Ordinal);
        Assert.DoesNotContain("tmp_psq_load", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatReadFloat64((r1 + 112))", code, StringComparison.Ordinal);
    }

    [Fact]
    public void PairedStackRestoreIsKeptWhenConsumedBeforeScalarRestore()
    {
        var function = PairedRestoreFunction(usePairedValue: true);
        var code = Emit(function, PairedRestoreTypes(), 0x80008020);

        Assert.Contains("PPC_PsqLStackInline<0u, 0u>", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWriteRamFloat64((r1 + 40), f31.d)", code, StringComparison.Ordinal);
    }

    private static IrFunction FctiwFunction(bool includeGuestCallAfterLoad)
    {
        var instructions = new List<IrInstruction>
        {
            new IrBinary("f0", IrValue.Register("f1"), IrValue.Imm(0), "fctiwz"),
            new IrStore(new IrAddress("r1", -16), IrValue.Register("f0"), 8),
            new IrLoad("r3", new IrAddress("r1", -12), 4)
        };
        if (includeGuestCallAfterLoad)
            instructions.Add(new IrCall(string.Empty, "func_80001234", Array.Empty<IrValue>()));
        instructions.Add(new IrReturn(null));
        return new IrFunction("fctiw_stack", "entry", new[]
        {
            new IrBasicBlock("entry", instructions)
        });
    }

    private static RepresentationEnvironment FctiwTypes() => new(new Dictionary<string, ValueRepresentation>
    {
        ["r1"] = ValueRepresentation.UInt32,
        ["r3"] = ValueRepresentation.UInt32,
        ["f0"] = ValueRepresentation.Float64,
        ["f1"] = ValueRepresentation.Float64
    });

    private static IrFunction PairedRestoreFunction(bool usePairedValue)
    {
        var instructions = new List<IrInstruction>
        {
            new IrBinary("tmp_psq_load", IrValue.Register("r1"), IrValue.Imm(120), "add"),
            new IrCall("f31", "PPC_PsqL", new[]
            {
                IrValue.Register("tmp_psq_load"), IrValue.Imm(0), IrValue.Imm(0)
            })
        };
        if (usePairedValue)
            instructions.Add(new IrStore(new IrAddress("r1", 40), IrValue.Register("f31"), 8));
        instructions.Add(new IrLoad("f31", new IrAddress("r1", 112), 8));
        instructions.Add(new IrReturn(null));
        return new IrFunction("paired_stack_restore", "entry", new[]
        {
            new IrBasicBlock("entry", instructions)
        });
    }

    private static RepresentationEnvironment PairedRestoreTypes() => new(new Dictionary<string, ValueRepresentation>
    {
        ["r1"] = ValueRepresentation.UInt32,
        ["f31"] = ValueRepresentation.Float64,
        ["tmp_psq_load"] = ValueRepresentation.UInt32
    });

    private static string Emit(
        IrFunction function,
        RepresentationEnvironment types,
        uint address)
    {
        var ssa = new SsaTransformer().Convert(function);
        var signature = new FunctionAbiClassification(function.Name, ValueRepresentation.Void);
        return new CxxLinearCodeGenerator().Emit(address, ssa, signature, types);
    }
}
