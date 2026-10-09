using System;
using System.Collections.Generic;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class LeafFprStackElisionCodeGenTests
{
    [Fact]
    public void ProvenScalarAndPairedAbiSpillUsesNativeLocal()
    {
        var function = new IrFunction(
            "leaf_fpr_abi_spill_elision",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBinary("r1", IrValue.Register("r1"), IrValue.Imm(-128), "add"),
                    new IrStore(new IrAddress("r1", 112), IrValue.Register("f31"), 8),
                    new IrBinary("save_ps1", IrValue.Register("r1"), IrValue.Imm(120), "add"),
                    new IrCall(string.Empty, "PPC_PsqSt", new[]
                    {
                        IrValue.Register("save_ps1"), IrValue.Register("f31"), IrValue.Imm(0), IrValue.Imm(0)
                    }),
                    new IrCall("f31", "PPC_PsAdd", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                    new IrBinary("restore_ps1", IrValue.Register("r1"), IrValue.Imm(120), "add"),
                    new IrCall("f31", "PPC_PsqL", new[]
                    {
                        IrValue.Register("restore_ps1"), IrValue.Imm(0), IrValue.Imm(0)
                    }),
                    new IrLoad("f31", new IrAddress("r1", 112), 8),
                    new IrBinary("r1", IrValue.Register("r1"), IrValue.Imm(128), "add"),
                    new IrReturn(null)
                })
            });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r1"] = ValueRepresentation.UInt32,
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64,
            ["save_ps1"] = ValueRepresentation.UInt32,
            ["restore_ps1"] = ValueRepresentation.UInt32
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x80006221,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("leaf_fpr_abi_spill_elision", ValueRepresentation.Void),
            types,
            enableLeafAbiSpillElision: true);

        Assert.Contains("double leaf_stack_saved_f31_entry", code, StringComparison.Ordinal);
        Assert.Contains("leaf_stack_saved_f31_entry = f31.d;", code, StringComparison.Ordinal);
        Assert.Contains("f31.d = leaf_stack_saved_f31_entry;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("WriteStackFloat64", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ReadStackFloat64", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsqSt", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsqL", code, StringComparison.Ordinal);
        Assert.Contains(
            "// RECOMP_REGISTRATION base 0x80006221 leaf_fpr_abi_spill_elision preserves=true fpr_mask=0x00000000",
            code,
            StringComparison.Ordinal);
    }
}
