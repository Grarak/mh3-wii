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
/// Pins that a resident call's fast arm carries no CpuContext traffic, while the
/// <c>InvokeDirectCpu</c> arm (used when a mod overlay replaces the callee) keeps
/// its own flush/reload pair.
/// </summary>
public class StateFreeResidentMarshallingTests
{
    private const uint CallerAddress = 0x80001000u;
    private const uint CalleeAddress = 0x80002000u;

    private static GuestAbiContract Contract(
        uint gprRead = 0,
        uint gprWrite = 0,
        byte crRead = 0,
        byte crWrite = 0,
        bool readsXer = false,
        bool writesXer = false) =>
        new(
            GprReadBeforeWriteMask: gprRead,
            GprPossibleWriteMask: gprWrite,
            GprReturnMask: 0,
            FprReadBeforeWriteMask: 0,
            FprPossibleWriteMask: 0,
            FprReturnMask: 0,
            CrReadBeforeWriteMask: crRead,
            CrPossibleWriteMask: crWrite,
            ReadsXerBeforeWrite: readsXer,
            MayWriteXer: writesXer,
            ReadsCtrBeforeWrite: false,
            MayWriteCtr: false,
            ReadsLrBeforeWrite: false,
            MayWriteLr: false,
            BoundaryFlags: GuestCallBoundaryFlags.None,
            DirectCallTargets: Array.Empty<uint>());

    private static string Emit(IrFunction function, GuestAbiContract calleeContract)
    {
        return new CxxLinearCodeGenerator().Emit(
            CallerAddress,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract> { [CalleeAddress] = calleeContract },
            stateFreeCallSymbols: new Dictionary<uint, string> { [CalleeAddress] = "callee_native" });
    }

    private static IrFunction Caller() =>
        new("state_free_caller", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrReturn(null)
            })
        });

    [Fact]
    public void FastArmPassesResidentLocalsAndWritesResultsBackIntoThem()
    {
        var code = Emit(Caller(), Contract(gprRead: 1u << 3, gprWrite: 1u << 3));

        var fastArm = code.IndexOf("callee_native(r3)", StringComparison.Ordinal);
        Assert.True(fastArm > 0, $"expected the fast arm to pass the resident local:\n{code}");
        Assert.DoesNotContain("callee_native(ctx->gpr[3])", code, StringComparison.Ordinal);

        // One output packs into a bare uint64_t, unpacked straight into the local.
        var resultStore = code.IndexOf("    r3 = static_cast<uint32_t>(state_free_result_80002000_", StringComparison.Ordinal);
        Assert.True(resultStore > fastArm, $"expected the result to land in the local:\n{code}");
    }

    [Fact]
    public void SlowArmKeepsItsOwnCompleteFlushAndReload()
    {
        var code = Emit(Caller(), Contract(gprRead: 1u << 3, gprWrite: 1u << 3));
        var elseArm = code.IndexOf("} else {", StringComparison.Ordinal);
        var call = code.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        var flush = code.IndexOf("        ctx->gpr[3] = r3;", StringComparison.Ordinal);
        var reload = code.IndexOf("        r3 = ctx->gpr[3];", StringComparison.Ordinal);

        Assert.True(elseArm > 0 && flush > elseArm && call > flush && reload > call,
            $"the InvokeDirectCpu arm must publish and re-read the locals itself:\n{code}");
    }

    /// <summary>
    /// The flush that used to bracket the whole call site is gone; only the two
    /// registers an HLE interrupt handler reads out of the architectural copy
    /// survive, and only when this frame actually wrote them.
    /// </summary>
    [Fact]
    public void FastArmEmitsNoBoundaryFlushOrReload()
    {
        var code = Emit(Caller(), Contract(gprRead: 1u << 3, gprWrite: 1u << 3));
        var fastArm = code.IndexOf("if (MkwStateFreeAbiEnabled(", StringComparison.Ordinal);
        Assert.True(fastArm > 0);

        // Nothing between the last body statement and the guard.
        var beforeGuard = code[..fastArm];
        Assert.DoesNotContain("ctx->gpr[3] = r3;", beforeGuard, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->gpr[4] = r4;", beforeGuard, StringComparison.Ordinal);
        // r4 is only read by this frame and is not part of the callee interface,
        // so it is neither passed nor reloaded on the fast arm.
        Assert.DoesNotContain("callee_native(r3, r4)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CrAndXerInterfaceRegistersUseTheResidentLocals()
    {
        var contract = Contract(gprRead: 1u << 3, crRead: 0x80, crWrite: 0x80, readsXer: true);
        var function = new IrFunction("state_free_cr_caller", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), true),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var code = Emit(function, contract);

        Assert.Contains("callee_native(r3, cr, xer)", code, StringComparison.Ordinal);
        Assert.Contains("    cr = static_cast<uint32_t>(state_free_result_80002000_", code, StringComparison.Ordinal);
        // XER is published for the interrupt-handler invariant, never reloaded.
        Assert.DoesNotContain("    xer = ctx->xer;\n    r3", code, StringComparison.Ordinal);
    }

}
