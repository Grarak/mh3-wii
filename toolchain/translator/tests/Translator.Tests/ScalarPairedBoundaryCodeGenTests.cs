using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class ScalarPairedBoundaryCodeGenTests
{
    [Theory]
    [InlineData("add", true)]
    [InlineData("sub", true)]
    [InlineData("mul", true)]
    [InlineData("fdiv", true)]
    [InlineData("fcmp", true)]
    [InlineData("fabs", false)]
    [InlineData("fneg", false)]
    [InlineData("frsp", false)]
    [InlineData("fsqrt", false)]
    [InlineData("fctiw", false)]
    [InlineData("fctiwz", false)]
    public void NormalizesPairedOperandsForEveryScalarFloatOperator(
        string operation,
        bool consumesRightOperand)
    {
        var code = Emit(
            new IrCall("f2", "PPC_PsSum0", new[]
            {
                IrValue.Register("f5"), IrValue.Register("f6"), IrValue.Register("f7")
            }),
            new IrCall("f3", "PPC_PsSum0", new[]
            {
                IrValue.Register("f8"), IrValue.Register("f9"), IrValue.Register("f10")
            }),
            new IrBinary(
                "f4",
                IrValue.Register("f2"),
                consumesRightOperand ? IrValue.Register("f3") : IrValue.Imm(0),
                operation));

        Assert.Contains("PPC_PsToScalarInline(f2.d)", code, StringComparison.Ordinal);
        if (consumesRightOperand)
            Assert.Contains("PPC_PsToScalarInline(f3.d)", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("PPC_PsFromScalar", 1)]
    [InlineData("PPC_Fadds", 2)]
    [InlineData("PPC_Fsubs", 2)]
    [InlineData("PPC_Fmuls", 2)]
    [InlineData("PPC_Fdivs", 2)]
    [InlineData("PPC_Fsqrt", 1)]
    [InlineData("PPC_Frsqrte", 1)]
    [InlineData("PPC_Fmadd", 3)]
    [InlineData("PPC_Fmsub", 3)]
    [InlineData("PPC_Fnmadd", 3)]
    [InlineData("PPC_Fnmsub", 3)]
    public void NormalizesEveryPairedScalarHelperOperand(string target, int argumentCount)
    {
        var arguments = Enumerable.Repeat(IrValue.Register("f1"), argumentCount).ToArray();
        var code = Emit(
            new IrCall("f1", "PPC_PsMul", new[]
            {
                IrValue.Register("f5"), IrValue.Register("f6")
            }),
            new IrCall("f2", target, arguments));

        const string conversion = "PPC_PsToScalarInline(f1.d)";
        Assert.Equal(
            argumentCount,
            code.Split(conversion, StringSplitOptions.None).Length - 1);
    }

    [Fact]
    public void PreservesScalarDoubleInputForFrsqrte()
    {
        var code = Emit(new IrCall("f2", "PPC_Frsqrte", new[] { IrValue.Register("f1") }));

        Assert.Contains("PPC_Frsqrte(f1.d)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsFromScalarInline(f1.d)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void PreservesPairedArgumentsAtUnprovenIndirectCallBoundary()
    {
        var code = Emit(
            new IrCall("f1", "PPC_PsMul", new[]
            {
                IrValue.Register("f5"), IrValue.Register("f6")
            }),
            new IrIndirectCall(string.Empty, IrValue.Register("ctr"), new[]
            {
                IrValue.Register("r3"), IrValue.Register("f1")
            }),
            new IrCall("f2", "PPC_PsNeg", new[] { IrValue.Register("f1") }));
        var call = code.IndexOf("InvokeIndirectCpu(ctr, ctx);", StringComparison.Ordinal);

        Assert.True(call >= 0, code);
        Assert.DoesNotContain(
            "f1.d = PPC_PsToScalarInline(f1.d);",
            code[..call],
            StringComparison.Ordinal);
        Assert.Contains(
            "PPC_PsNegInline(PPC_PsFromScalarInline(f1.d))",
            code[call..],
            StringComparison.Ordinal);
    }

    [Fact]
    public void PreservesUntouchedPairedPayloadAcrossIndirectCall()
    {
        var code = Emit(
            new IrCall("f2", "PPC_PsMul", new[]
            {
                IrValue.Register("f5"), IrValue.Register("f6")
            }),
            new IrIndirectCall(string.Empty, IrValue.Register("ctr"), new[]
            {
                IrValue.Register("r3")
            }),
            new IrCall("f3", "PPC_PsNeg", new[] { IrValue.Register("f2") }));
        var call = code.IndexOf("InvokeIndirectCpu(ctr, ctx);", StringComparison.Ordinal);

        Assert.True(call >= 0, code);
        Assert.Contains(
            "PPC_PsNegInline(f2.d)",
            code[call..],
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "PPC_PsFromScalarInline(f2.d)",
            code[call..],
            StringComparison.Ordinal);
    }

    [Fact]
    public void UnprovenIndirectF1RemainsConventionalScalarForMixedConsumers()
    {
        var code = Emit(
            new IrIndirectCall(string.Empty, IrValue.Register("ctr"), new[]
            {
                IrValue.Register("r3")
            }),
            new IrSetCrField(0, IrValue.Register("f1"), IrValue.Register("f14"), false),
            new IrCall("f2", "PPC_PsNeg", new[] { IrValue.Register("f1") }));
        var call = code.IndexOf("InvokeIndirectCpu(ctr, ctx);", StringComparison.Ordinal);

        Assert.True(call >= 0, code);
        Assert.Contains(
            "SetCRFloatResident(cr, 0, f1.d, f14.d);",
            code[call..],
            StringComparison.Ordinal);
        Assert.Contains(
            "PPC_PsNegInline(PPC_PsFromScalarInline(f1.d))",
            code[call..],
            StringComparison.Ordinal);
    }

    [Fact]
    public void UnprovenDirectF1RemainsConventionalScalarForMixedConsumers()
    {
        var code = Emit(
            new IrCall(string.Empty, "func_80002000", Array.Empty<IrValue>()),
            new IrSetCrField(0, IrValue.Register("f1"), IrValue.Register("f14"), false),
            new IrCall("f2", "PPC_PsNeg", new[] { IrValue.Register("f1") }));
        var call = code.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);

        Assert.True(call >= 0, code);
        Assert.Contains(
            "SetCRFloatResident(cr, 0, f1.d, f14.d);",
            code[call..],
            StringComparison.Ordinal);
        Assert.Contains(
            "PPC_PsNegInline(PPC_PsFromScalarInline(f1.d))",
            code[call..],
            StringComparison.Ordinal);
    }

    [Fact]
    public void PacksImmediateForPairedConsumer()
    {
        var code = Emit(new IrCall("f2", "PPC_PsNeg", new[] { IrValue.Imm(7) }));

        Assert.Contains(
            "PPC_PsNegInline(PPC_PsFromScalarInline(7))",
            code,
            StringComparison.Ordinal);
    }

    [Fact]
    public void PairedMoveCopiesBothArchitecturalLanes()
    {
        var code = Emit(new IrCall("f2", "PPC_PsMr", new[] { IrValue.Register("f7") }));

        Assert.Contains(
            "PpcSetPairedFprInline(f2, f7.d);",
            code,
            StringComparison.Ordinal);
        Assert.DoesNotContain("f2.d = f7.d", code, StringComparison.Ordinal);
    }

    [Fact]
    public void EmitsArchitecturallyAccurateScalarFloatHelpers()
    {
        var code = Emit(
            new IrCall("f2", "PPC_Fmuls", new[]
            {
                IrValue.Register("f3"), IrValue.Register("f4")
            }),
            new IrCall("f5", "PPC_Fmadd", new[]
            {
                IrValue.Register("f6"), IrValue.Register("f7"), IrValue.Register("f8")
            }),
            new IrCall("f9", "PPC_Fmsub", new[]
            {
                IrValue.Register("f10"), IrValue.Register("f11"), IrValue.Register("f12")
            }),
            new IrCall("f13", "PPC_Fnmadd", new[]
            {
                IrValue.Register("f14"), IrValue.Register("f15"), IrValue.Register("f16")
            }),
            new IrCall("f17", "PPC_Fnmsub", new[]
            {
                IrValue.Register("f18"), IrValue.Register("f19"), IrValue.Register("f20")
            }),
            new IrBinary(
                "f21",
                IrValue.Register("f22"),
                IrValue.Imm(0),
                "frsp"));

        Assert.Contains("PpcFmulsInline(", code, StringComparison.Ordinal);
        Assert.Contains("PpcFmaddInline(", code, StringComparison.Ordinal);
        Assert.Contains("PpcFmsubInline(", code, StringComparison.Ordinal);
        Assert.Contains("PpcFnmaddInline(", code, StringComparison.Ordinal);
        Assert.Contains("PpcFnmsubInline(", code, StringComparison.Ordinal);
        Assert.Contains("PpcForceSingleValueInline(", code, StringComparison.Ordinal);
    }

    private static string Emit(params IrInstruction[] instructions)
    {
        var body = instructions.Concat(new[] { new IrReturn(null) }).ToArray();
        var function = new IrFunction(
            "scalar_paired_boundary",
            "entry",
            new[] { new IrBasicBlock("entry", body) });
        var types = Enumerable.Range(0, 32).ToDictionary(
            index => $"f{index}",
            _ => (ValueRepresentation)ValueRepresentation.Float64,
            StringComparer.OrdinalIgnoreCase);
        types["r3"] = ValueRepresentation.UInt32;
        types["ctr"] = ValueRepresentation.UInt32;
        return new CxxLinearCodeGenerator().Emit(
            0x80006200,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(
                "scalar_paired_boundary",
                ValueRepresentation.Void),
            new RepresentationEnvironment(types));
    }
}
