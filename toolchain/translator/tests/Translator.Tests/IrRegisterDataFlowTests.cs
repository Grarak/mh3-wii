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

public sealed class IrRegisterDataFlowTests
{
    [Fact]
    public void ResolvedInstructionsExposeAllRegisterOperandsAndDestinations()
    {
        var instructions = new IrInstruction[]
        {
            new IrResolvedLoad(
                "r3", "range", new IrAddress("r4", 4), 0, 4),
            new IrResolvedStore(
                "range", new IrAddress("r5", 8), 0, IrValue.Register("r6"), 4),
            new IrResolvedPsqLoad(
                "f1", "range", IrValue.Register("r7"), 0, 0, 2, KnownGqr: null),
            new IrResolvedPsqStore(
                "range", IrValue.Register("r8"), 0, IrValue.Register("r9"),
                0, 3, KnownGqr: 0x1234u),
            new IrResolvedLoadPair(
                "r10", "r11", "range", new IrAddress("r12", 0),
                new IrAddress("r13", 4), 0, 4),
            new IrResolvedStorePair(
                "range", new IrAddress("r14", 0), new IrAddress("r15", 4),
                0, IrValue.Register("r16"), IrValue.Register("r17"), 4)
        };

        var uses = instructions.SelectMany(IrRegisterDataFlow.Uses).ToArray();
        var definitions = instructions.SelectMany(IrRegisterDataFlow.Definitions).ToArray();

        Assert.Equal(
            new[] { "r4", "r5", "r6", "r7", "gqr2", "r8", "r9", "r12", "r13", "r14", "r15", "r16", "r17" },
            uses);
        Assert.Equal(new[] { "r3", "f1", "r10", "r11" }, definitions);
    }

    [Fact]
    public void JumpTableSelectorsAndRepeatedOperandsRemainVisible()
    {
        var selector = new IrJumpTable(
            "r9_jump_selector",
            new[] { new IrJumpTableCase(0x80001000u, "case_0") });
        var duplicateUse = new IrBinary(
            "temporary", IrValue.Register("r3"), IrValue.Register("r3"), "add");

        Assert.Equal(new[] { "r9_jump_selector" }, IrRegisterDataFlow.Uses(selector));
        Assert.Equal(new[] { "r3", "r3" }, IrRegisterDataFlow.Uses(duplicateUse));
    }

    [Fact]
    public void NumericSsaSuffixIsArchitecturalButNamedLifterSuffixIsTemporary()
    {
        Assert.Equal("r3", IrRegisterDataFlow.BaseName("r3_7"));
        Assert.Equal("r3_addc_left", IrRegisterDataFlow.BaseName("r3_addc_left"));
        Assert.True(IrRegisterDataFlow.IsRegisterName("r3_7"));
        Assert.False(IrRegisterDataFlow.IsRegisterName("r3_addc_left"));
    }

    [Fact]
    public void RangePointerDestinationIsNotDeclaredAsAValueLocal()
    {
        var function = new IrFunction(
            "range_pointer_local_policy",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrResolveGuestMemoryRange(
                        "range", IrValue.Register("r13"), 0, 64,
                        NeedsReadAccess: true, NeedsWriteAccess: false),
                    new IrResolvedLoad(
                        "r3", "range", new IrAddress("r2", 4), 4, 4),
                    new IrReturn(IrValue.Register("r3"))
                })
            });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r2"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32,
            ["r13"] = ValueRepresentation.UInt32
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x80006203,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("range_pointer_local_policy", ValueRepresentation.UInt32),
            types);

        Assert.Equal(new[] { "range" }, IrRegisterDataFlow.Definitions(function.Blocks[0].Instructions[0]));
        Assert.Contains("uint8_t* range = nullptr;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("uint32_t range", code, StringComparison.Ordinal);
    }
}
