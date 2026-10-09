using System;
using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

/// <summary>
/// f14-f31 a function saves and reloads raw from its own frame keep the caller's value, so the
/// dispatch need not guard them (the thunk addresses: GuestSaveRestoreThunkTestDefaults).
/// </summary>
public class NonvolatileFprPreservationTests
{
    private static uint Preserved(params IrBasicBlock[] blocks) =>
        NonvolatileFprPreservationAnalyzer.PreservedMask(new SsaTransformer().Convert(new IrFunction("f", blocks[0].Label, blocks)).Function);

    private static IrInstruction SaveF31 => new IrStore(new IrAddress("r1", -8), IrValue.Register("f31"), 8);

    private static IrInstruction RestoreF31 => new IrLoad("f31", new IrAddress("r1", -8), 8);

    private static IrInstruction WriteF31 => new IrBinary("f31", IrValue.Register("f1"), IrValue.Register("f2"), "fmul");

    [Fact]
    public void StfdLfdRoundTripPreservesTheRegister()
    {
        var mask = Preserved(new IrBasicBlock("entry", new[] { SaveF31, WriteF31, RestoreF31, new IrReturn(null) }));
        Assert.Equal(NonvolatileFprPreservationAnalyzer.NonvolatileFprMask, mask);
    }

    [Fact]
    public void OverwrittenSaveSlotDoesNotPreserve()
    {
        var mask = Preserved(new IrBasicBlock("entry", new[]
        {
            SaveF31,
            WriteF31,
            new IrStore(new IrAddress("r1", -4), IrValue.Register("r3"), 4),
            RestoreF31,
            new IrReturn(null)
        }));
        Assert.Equal(0u, mask & 0x80000000u);
    }

    [Fact]
    public void SingleLoadFromTheSaveSlotDoesNotRestore()
    {
        var mask = Preserved(new IrBasicBlock("entry", new[]
        {
            SaveF31,
            WriteF31,
            new IrLoad("f31", new IrAddress("r1", -8), 4),
            new IrReturn(null)
        }));
        Assert.Equal(0u, mask & 0x80000000u);
    }

    [Fact]
    public void ExitWithoutTheReloadDoesNotPreserve()
    {
        var mask = Preserved(
            new IrBasicBlock("entry", new IrInstruction[]
            {
                SaveF31,
                WriteF31,
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrBranch("eq", "early", "late")
            }),
            new IrBasicBlock("early", new IrInstruction[] { new IrReturn(null) }),
            new IrBasicBlock("late", new[] { RestoreF31, new IrReturn(null) }));
        Assert.Equal(0u, mask & 0x80000000u);
    }

    [Fact]
    public void ReloadOfAnotherRegistersSlotDoesNotRestore()
    {
        var mask = Preserved(new IrBasicBlock("entry", new[]
        {
            SaveF31,
            new IrStore(new IrAddress("r1", -16), IrValue.Register("f30"), 8),
            WriteF31,
            new IrLoad("f31", new IrAddress("r1", -16), 8),
            new IrReturn(null)
        }));
        Assert.Equal(0u, mask & 0x80000000u);
        Assert.NotEqual(0u, mask & 0x40000000u);
    }

    [Fact]
    public void PsqStoreOverTheSaveSlotDoesNotPreserve()
    {
        var mask = Preserved(new IrBasicBlock("entry", new IrInstruction[]
        {
            SaveF31,
            WriteF31,
            new IrBinary("r5", IrValue.Register("r1"), IrValue.Imm(-8), "add"),
            new IrCall(string.Empty, "PPC_PsqSt", new[] { IrValue.Register("r5"), IrValue.Register("f2"), IrValue.Imm(0), IrValue.Imm(0) }),
            RestoreF31,
            new IrReturn(null)
        }));
        Assert.Equal(0u, mask & 0x80000000u);
    }

    [Fact]
    public void SaveAndRestoreThunksPreserveTheirRange()
    {
        var mask = Preserved(new IrBasicBlock("entry", new IrInstruction[]
        {
            new IrBinary("r11", IrValue.Register("r1"), IrValue.Imm(64), "add"),
            new IrCall(string.Empty, "func_80021504", Array.Empty<IrValue>()),
            new IrAssign("f26", IrValue.Register("f1")),
            new IrAssign("f31", IrValue.Register("f2")),
            new IrBinary("r11", IrValue.Register("r1"), IrValue.Imm(64), "add"),
            new IrCall(string.Empty, "func_80021550", Array.Empty<IrValue>()),
            new IrReturn(null)
        }));
        Assert.Equal(NonvolatileFprPreservationAnalyzer.NonvolatileFprMask, mask);
    }

    [Fact]
    public void RestoreThunkFromAnotherFrameDoesNotRestore()
    {
        var mask = Preserved(new IrBasicBlock("entry", new IrInstruction[]
        {
            new IrBinary("r11", IrValue.Register("r1"), IrValue.Imm(64), "add"),
            new IrCall(string.Empty, "func_80021504", Array.Empty<IrValue>()),
            new IrAssign("f26", IrValue.Register("f1")),
            new IrBinary("r11", IrValue.Register("r1"), IrValue.Imm(128), "add"),
            new IrCall(string.Empty, "func_80021550", Array.Empty<IrValue>()),
            new IrReturn(null)
        }));
        Assert.Equal(0u, mask & 0xFC000000u);
    }

    [Fact]
    public void SavedAndRestoredRegistersLeaveThePublishedWriteMask()
    {
        var function = new IrFunction("save_change_restore", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r11", IrValue.Register("r1"), IrValue.Imm(64), "add"),
                new IrCall(string.Empty, "func_80021504", Array.Empty<IrValue>()),
                new IrAssign("f26", IrValue.Register("f1")),
                new IrBinary("r11", IrValue.Register("r1"), IrValue.Imm(64), "add"),
                new IrCall(string.Empty, "func_80021550", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r1"] = ValueRepresentation.Int32,
            ["r11"] = ValueRepresentation.Int32,
            ["f1"] = ValueRepresentation.Float64,
            ["f26"] = ValueRepresentation.Float64,
            ["f27"] = ValueRepresentation.Float64,
            ["f28"] = ValueRepresentation.Float64,
            ["f29"] = ValueRepresentation.Float64,
            ["f30"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("save_change_restore", ValueRepresentation.Void);
        var code = new CxxLinearCodeGenerator().Emit(0x80008028, new SsaTransformer().Convert(function), signature, types);

        Assert.Contains("// RECOMP_REGISTRATION base 0x80008028 save_change_restore preserves=true fpr_mask=0x00000000", code, StringComparison.Ordinal);
    }
}
