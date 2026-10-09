using System;
using System.Collections.Generic;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public sealed class GuestThunkCodeGenTests
{
    [Fact]
    public void GprSaveAndRestoreUseTheSameInlineRangeShape()
    {
        var save = Emit(
            "gpr_save",
            0x8002156Cu,
            new Dictionary<string, ValueRepresentation>
            {
                ["r11"] = ValueRepresentation.UInt32,
                ["r14"] = ValueRepresentation.UInt32,
                ["r31"] = ValueRepresentation.UInt32
            });
        var restore = Emit(
            "gpr_restore",
            0x800215B8u,
            new Dictionary<string, ValueRepresentation>
            {
                ["r11"] = ValueRepresentation.UInt32,
                ["r14"] = ValueRepresentation.UInt32,
                ["r31"] = ValueRepresentation.UInt32
            });

        Assert.Contains(
            "MemoryInline::FlatWrite32((r11 + -72), static_cast<uint32_t>(r14));",
            save,
            StringComparison.Ordinal);
        Assert.Contains(
            "r14 = MemoryInline::FlatRead32((r11 + -72));",
            restore,
            StringComparison.Ordinal);
        Assert.DoesNotContain("InvokeDirectCpu<0x8002156Cu>(ctx);", save, StringComparison.Ordinal);
        Assert.DoesNotContain("InvokeDirectCpu<0x800215B8u>(ctx);", restore, StringComparison.Ordinal);
    }

    [Fact]
    public void FprSaveAndRestoreUseEightByteInlineHelpers()
    {
        var types = new Dictionary<string, ValueRepresentation>
        {
            ["r11"] = ValueRepresentation.UInt32,
            ["f23"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64
        };
        var save = Emit("fpr_save", 0x800214F8u, types);
        var restore = Emit("fpr_restore", 0x80021544u, types);

        Assert.Contains(
            "MemoryInline::FlatWriteFloat64((r11 + -72), f23.d);",
            save,
            StringComparison.Ordinal);
        Assert.Contains(
            "f23.d = MemoryInline::FlatReadFloat64((r11 + -72));",
            restore,
            StringComparison.Ordinal);
    }

    private static string Emit(
        string name,
        uint thunkAddress,
        IReadOnlyDictionary<string, ValueRepresentation> typeMap)
    {
        var function = new IrFunction(
            name,
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall(string.Empty, $"func_{thunkAddress:X8}", Array.Empty<IrValue>()),
                    new IrReturn(null)
                })
            });
        var code = new CxxLinearCodeGenerator().Emit(
            thunkAddress,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(name, ValueRepresentation.Void),
            new RepresentationEnvironment(typeMap));
        return code;
    }
}
