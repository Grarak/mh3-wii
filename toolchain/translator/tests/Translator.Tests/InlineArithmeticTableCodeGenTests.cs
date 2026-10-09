using System;
using System.Collections.Generic;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

/// <summary>
/// Coverage for the descriptor-driven arithmetic inline emitters, going through the public
/// C++ generator (not the private emitter directly) so lookup and output are tested together.
/// </summary>
public sealed class InlineArithmeticTableCodeGenTests
{
    public static IEnumerable<object[]> PairedCases()
    {
        yield return Case("PPC_PsMerge00", "PPC_PsMerge00Inline");
        yield return Case("PPC_PsMerge01", "PPC_PsMerge01Inline");
        yield return Case("PPC_PsMerge10", "PPC_PsMerge10Inline");
        yield return Case("PPC_PsMerge11", "PPC_PsMerge11Inline");
        yield return Case("PPC_PsAdd", "PPC_PsAddInline");
        yield return Case("PPC_PsAddNoNi", "PPC_PsAddNoNiInline");
        yield return Case("PPC_PsSub", "PPC_PsSubInline");
        yield return Case("PPC_PsSubNoNi", "PPC_PsSubNoNiInline");
        yield return Case("PPC_PsDiv", "PPC_PsDivInline");
        yield return Case("PPC_PsMul", "PPC_PsMulInline");
        yield return Case("PPC_PsMulNoNi", "PPC_PsMulNoNiInline");
        yield return Case("PPC_PsNeg", "PPC_PsNegInline");
        yield return Case("PPC_PsAbs", "PPC_PsAbsInline");
        yield return Case("PPC_PsMuls0", "PPC_PsMuls0Inline");
        yield return Case("PPC_PsMuls1", "PPC_PsMuls1Inline");
        yield return Case("PPC_PsMadd", "PPC_PsMaddInline");
        yield return Case("PPC_PsMaddNoNi", "PPC_PsMaddNoNiInline");
        yield return Case("PPC_PsMsub", "PPC_PsMsubInline");
        yield return Case("PPC_PsMsubNoNi", "PPC_PsMsubNoNiInline");
        yield return Case("PPC_PsNmsub", "PPC_PsNmsubInline");
        yield return Case("PPC_PsNmsubNoNi", "PPC_PsNmsubNoNiInline");
        yield return Case("PPC_PsNmadd", "PPC_PsNmaddInline");
        yield return Case("PPC_PsMadds0", "PPC_PsMadds0Inline");
        yield return Case("PPC_PsMadds1", "PPC_PsMadds1Inline");
        yield return Case("PPC_PsSum0", "PPC_PsSum0Inline");
        yield return Case("PPC_PsSum1", "PPC_PsSum1Inline");
    }

    public static IEnumerable<object[]> ScalarCases()
    {
        yield return Case("PPC_Fadds", "PpcForceSingleValueInline");
        yield return Case("PPC_FaddsNoNi", "static_cast<float>");
        yield return Case("PPC_Fsubs", "PpcForceSingleValueInline");
        yield return Case("PPC_FsubsNoNi", "static_cast<float>");
        yield return Case("PPC_Fmuls", "PpcFmulsInline");
        yield return Case("PPC_FmulsNoNi", "PpcFmulsNoNiInline");
        yield return Case("PPC_Fdivs", "PpcForceSingleValueInline");
        yield return Case("PPC_FdivsNoNi", "static_cast<float>");
        yield return Case("PPC_Fsqrt", "std::sqrt");
        yield return Case("PPC_Fmadd", "PpcFmaddInline");
        yield return Case("PPC_Fmsub", "PpcFmsubInline");
        yield return Case("PPC_Fnmadd", "PpcFnmaddInline");
        yield return Case("PPC_Fnmsub", "PpcFnmsubInline");
    }

    [Theory]
    [MemberData(nameof(PairedCases))]
    public void PairedArithmeticTargetsUseExpectedInlineHelper(string target, string expected)
    {
        var code = EmitCall(
            target,
            "f0",
            IrValue.Register("f1"),
            IrValue.Register("f2"),
            IrValue.Register("f3"));

        Assert.Contains(expected + "(", code, StringComparison.Ordinal);
    }

    [Theory]
    [MemberData(nameof(ScalarCases))]
    public void ScalarArithmeticTargetsUseExpectedInlineHelper(string target, string expected)
    {
        var code = EmitCall(
            target,
            "f0",
            IrValue.Register("f1"),
            IrValue.Register("f2"),
            IrValue.Register("f3"));

        Assert.Contains(expected, code, StringComparison.Ordinal);
    }

    [Fact]
    public void PairedTablePreservesOperandOrderAndNoNiPacking()
    {
        var code = EmitCall(
            "PPC_PsMaddNoNi",
            "f0",
            IrValue.Imm(1),
            IrValue.Imm(2),
            IrValue.Imm(3));

        Assert.Contains(
            "PPC_PsMaddNoNiInline(PPC_PsFromScalarNoNiInline(1), PPC_PsFromScalarNoNiInline(2), PPC_PsFromScalarNoNiInline(3))",
            code,
            StringComparison.Ordinal);
    }

    [Fact]
    public void ScalarTablePreservesFmaOperandOrder()
    {
        var code = EmitCall(
            "PPC_Fmsub",
            "f0",
            IrValue.Imm(1),
            IrValue.Imm(2),
            IrValue.Imm(3));

        Assert.Contains("PpcFmsubInline(1, 2, 3)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void TableTargetsRemainCaseInsensitive()
    {
        var code = EmitCall(
            "ppc_psaddnoni",
            "f0",
            IrValue.Imm(1),
            IrValue.Imm(2));

        Assert.Contains("PPC_PsAddNoNiInline(", code, StringComparison.Ordinal);
    }

    [Fact]
    public void TableTargetsKeepMissingDestinationSuppression()
    {
        var code = EmitCall(
            "PPC_PsAdd",
            string.Empty,
            IrValue.Imm(1),
            IrValue.Imm(2));

        Assert.DoesNotContain("PPC_PsAddInline(", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsAdd(", code, StringComparison.Ordinal);
    }

    [Fact]
    public void TableTargetsKeepMissingArgumentLiteralZeroFallback()
    {
        var code = EmitCall("PPC_PsAdd", "f0", IrValue.Imm(1));

        Assert.Contains(
            "PPC_PsAddInline(PPC_PsFromScalarInline(1), 0)",
            code,
            StringComparison.Ordinal);
    }

    private static object[] Case(string target, string expected) => new object[] { target, expected };

    private static string EmitCall(string target, string destination, params IrValue[] arguments)
    {
        var function = new IrFunction(
            "inline_table_probe",
            "entry",
            new[]
            {
                new IrBasicBlock(
                    "entry",
                    new IrInstruction[]
                    {
                        new IrCall(destination, target, arguments),
                        new IrReturn(null)
                    })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f0"] = ValueRepresentation.Float64,
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("inline_table_probe", ValueRepresentation.Void);

        return new CxxLinearCodeGenerator().Emit(
            0x80040000,
            new SsaTransformer().Convert(function),
            signature,
            types);
    }
}
