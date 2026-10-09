using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class CodeGenInlineCoverageTests
{
    private static readonly Type GeneratorType = typeof(CxxLinearCodeGenerator);

    private static bool InvokeTryEmitInline(
        IrCall call,
        StringBuilder sb,
        RepresentationEnvironment types,
        Dictionary<string, bool> localPaired)
    {
        var method = GeneratorType.GetMethod("TryEmitInlinePpc", BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);
        return (bool)method!.Invoke(null, new object?[] { call, sb, "    ", types, localPaired })!;
    }

    [Fact]
    public void TryEmitInlinePpc_EmitsSupportedHelperBodies()
    {
        var types = new RepresentationEnvironment();
        var paired = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase)
        {
            ["f1"] = true,
            ["f2"] = false,
            ["f3"] = true
        };

        var supported = new (IrCall Call, string Fragment)[]
        {
            (new IrCall("f5", "PPC_PsFromScalar", new[] { IrValue.Register("f2") }), "PPC_PsFromScalarInline"),
            (new IrCall("f6", "PPC_PsToScalar", new[] { IrValue.Register("f1") }), "PPC_PsToScalarInline"),
            (new IrCall("f7", "PPC_PsNeg", new[] { IrValue.Imm(7) }), "PPC_PsNegInline"),
            (new IrCall("f8", "PPC_PsAbs", new[] { IrValue.Register("f2") }), "PPC_PsAbsInline"),
            (new IrCall("f9", "PPC_PsSum0", new[] { IrValue.Register("f2"), IrValue.Register("f3"), IrValue.Register("f1") }), "PPC_PsSum0Inline"),
            (new IrCall("f10", "PPC_PsSum1", new[] { IrValue.Register("f2"), IrValue.Register("f3"), IrValue.Register("f1") }), "PPC_PsSum1Inline"),
            (new IrCall("f21", "PPC_PsDiv", new[] { IrValue.Register("f1"), IrValue.Register("f3") }), "PPC_PsDivInline"),
            (new IrCall("f11", "PPC_Fadds", new[] { IrValue.Register("f1"), IrValue.Register("f2") }), "PpcForceSingleValueInline"),
            (new IrCall("f12", "PPC_Fsubs", new[] { IrValue.Register("f2"), IrValue.Register("f1") }), "PpcForceSingleValueInline"),
            (new IrCall("f13", "PPC_Fmuls", new[] { IrValue.Register("f1"), IrValue.Register("f2") }), "PpcFmulsInline"),
            (new IrCall("f14", "PPC_Fdivs", new[] { IrValue.Register("f2"), IrValue.Register("f1") }), "PpcForceSingleValueInline"),
            (new IrCall("f15", "PPC_Fsqrt", new[] { IrValue.Register("f1") }), "std::sqrt"),
            (new IrCall("f16", "PPC_Fmadd", new[] { IrValue.Register("f2"), IrValue.Register("f1"), IrValue.Register("f3") }), "PpcFmaddInline"),
            (new IrCall("f17", "PPC_Fmsub", new[] { IrValue.Register("f2"), IrValue.Register("f1"), IrValue.Register("f3") }), "PpcFmsubInline"),
            (new IrCall("f18", "PPC_Fnmadd", new[] { IrValue.Register("f2"), IrValue.Register("f1"), IrValue.Register("f3") }), "PpcFnmaddInline"),
            (new IrCall("f19", "PPC_Fnmsub", new[] { IrValue.Register("f2"), IrValue.Register("f1"), IrValue.Register("f3") }), "PpcFnmsubInline"),
            (new IrCall("f20", "PPC_PsqL", new[] { IrValue.Register("r3"), IrValue.Imm(1), IrValue.Imm(5) }), "PPC_PsqLInline<1u, 5u>"),
            (new IrCall(string.Empty, "PPC_PsqSt", new[] { IrValue.Register("r3"), IrValue.Register("f1"), IrValue.Imm(0), IrValue.Imm(4) }), "PPC_PsqStInline<0u, 4u>")
        };

        foreach (var (call, fragment) in supported)
        {
            var sb = new StringBuilder();
            Assert.True(InvokeTryEmitInline(call, sb, types, paired), call.Target);
            Assert.Contains(fragment, sb.ToString(), StringComparison.Ordinal);
        }
    }

    [Fact]
    public void TryEmitInlinePpc_CoversFallbacksAndMissingArguments()
    {
        var types = new RepresentationEnvironment();
        var paired = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);

        Assert.False(InvokeTryEmitInline(new IrCall("f1", "", Array.Empty<IrValue>()), new StringBuilder(), types, paired));

        foreach (var target in new[]
        {
            "unknown_helper"
        })
        {
            var sb = new StringBuilder();
            Assert.False(InvokeTryEmitInline(new IrCall("f1", target, new[] { IrValue.Register("f1") }), sb, types, paired), target);
            Assert.Equal(0, sb.Length);
        }

        var psqMissingImmediates = new StringBuilder();
        Assert.False(
            InvokeTryEmitInline(
                new IrCall("f1", "PPC_PsqL", new[] { IrValue.Register("r3"), IrValue.Register("r4"), IrValue.Imm(0) }),
                psqMissingImmediates,
                types,
                paired));
        Assert.Equal(0, psqMissingImmediates.Length);

        var missingArgs = new StringBuilder();
        Assert.True(InvokeTryEmitInline(new IrCall("f3", "PPC_Fsqrt", Array.Empty<IrValue>()), missingArgs, types, paired));
        Assert.Contains("std::sqrt(0);", missingArgs.ToString(), StringComparison.Ordinal);
    }
}
