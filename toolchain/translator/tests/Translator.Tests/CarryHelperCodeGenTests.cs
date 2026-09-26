using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using Translator.Core.Analysis;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class CarryHelperCodeGenTests
{
    [Fact]
    public void CarryHelpersUseCpuContextWithoutRuntimeCalls()
    {
        var method = typeof(CxxLinearCodeGenerator).GetMethod(
            "TryEmitInlinePpc",
            BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);

        var cases = new[]
        {
            new IrCall("xer", "PPC_UpdateCarrySub", new[] { IrValue.Register("r3"), IrValue.Register("r4") }),
            new IrCall("xer", "PPC_UpdateCarryAdd", new[] { IrValue.Register("r5"), IrValue.Register("r6"), IrValue.Imm(1) }),
            new IrCall("xer", "PPC_UpdateCarryShiftRight", new[] { IrValue.Register("r7"), IrValue.Register("r8") }),
            new IrCall("r9_ca", "PPC_GetCarry", Array.Empty<IrValue>())
        };

        foreach (var call in cases)
        {
            var output = new StringBuilder();
            var emitted = (bool)method!.Invoke(null, new object?[]
            {
                call,
                output,
                "    ",
                new RepresentationEnvironment(),
                new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase),
                StackAddressFacts.Empty
            })!;

            Assert.True(emitted, call.Target);
            Assert.Contains("ctx->xer", output.ToString(), StringComparison.Ordinal);
            Assert.DoesNotContain($"{call.Target}(", output.ToString(), StringComparison.Ordinal);
            if (call.Target == "PPC_UpdateCarryAdd")
            {
                Assert.Contains("static_cast<uint64_t>(static_cast<uint32_t>", output.ToString(), StringComparison.Ordinal);
            }
        }
    }

    [Fact]
    public void CountLeadingZerosUsesInlinePrimitive()
    {
        var method = typeof(CxxLinearCodeGenerator).GetMethod(
            "TryEmitInlinePpc", BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);
        var output = new StringBuilder();
        var call = new IrCall("r4", "PPC_Cntlzw", new[] { IrValue.Register("r3") });

        var emitted = (bool)method!.Invoke(null, new object?[]
        {
            call,
            output,
            "    ",
            new RepresentationEnvironment(),
            new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase),
            StackAddressFacts.Empty
        })!;

        Assert.True(emitted);
        Assert.Contains("PPC_CntlzwInline", output.ToString(), StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_Cntlzw(", output.ToString(), StringComparison.Ordinal);
    }
}
