using Translator.Core.Analysis;
using Translator.Core.Ir;

namespace Translator.Tests;

public sealed class GuestAbiContractAnalyzerTests
{
    [Fact]
    public void TracksResolvedMemoryAddressesSourcesAndDestinations()
    {
        var function = new IrFunction("resolved_contract", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrResolveGuestMemoryRange("range", IrValue.Register("r3"), 0, 64, true, true),
                new IrResolvedLoad("r4", "range", new IrAddress("r3", 0), 0, 4),
                new IrResolvedStore("range", new IrAddress("r3", 4), 4, IrValue.Register("r5"), 4),
                new IrResolvedPsqLoad("f1", "range", IrValue.Register("r6"), 8, 0, 0, 0),
                new IrResolvedPsqStore("range", IrValue.Register("r8"), 24, IrValue.Register("f2"), 0, 0, 0),
                new IrResolvedLoadPair("r9", "f3", "range", new IrAddress("r10", 0), new IrAddress("r10", 4), 32, 4),
                new IrResolvedStorePair("range", new IrAddress("r11", 0), new IrAddress("r11", 4), 40,
                    IrValue.Register("r12"), IrValue.Register("f4"), 4),
                new IrReturn(null)
            })
        });

        var contract = GuestAbiContractAnalyzer.Analyze(function);
        var expectedGprReads = (1u << 3) | (1u << 5) | (1u << 6) |
                               (1u << 8) | (1u << 10) | (1u << 11) | (1u << 12);
        var expectedGprWrites = (1u << 4) | (1u << 9);
        var expectedFprReads = (1u << 2) | (1u << 4);
        var expectedFprWrites = (1u << 1) | (1u << 3);

        Assert.Equal(expectedGprReads, contract.GprReadBeforeWriteMask);
        Assert.Equal(expectedGprWrites, contract.GprPossibleWriteMask);
        Assert.Equal(expectedFprReads, contract.FprReadBeforeWriteMask);
        Assert.Equal(expectedFprWrites, contract.FprPossibleWriteMask);
    }

    [Fact]
    public void TracksPreciseHiddenStringAndCrHelperEffects()
    {
        var function = new IrFunction("helper_contract", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(string.Empty, "PPC_Stswi", new[]
                {
                    IrValue.Imm(30), IrValue.Register("r3"), IrValue.Imm(12)
                }),
                new IrCall(string.Empty, "PPC_Lswi", new[]
                {
                    IrValue.Imm(4), IrValue.Register("r3"), IrValue.Imm(8)
                }),
                new IrCall("cr", "PPC_CrLogical", new[]
                {
                    IrValue.Imm(0), IrValue.Imm(12), IrValue.Imm(4), IrValue.Imm(28)
                }),
                new IrReturn(null)
            })
        });

        var contract = GuestAbiContractAnalyzer.Analyze(function);

        Assert.Equal((1u << 3) | (1u << 30) | (1u << 31) | 1u, contract.GprReadBeforeWriteMask);
        Assert.Equal((1u << 4) | (1u << 5), contract.GprPossibleWriteMask);
        Assert.Equal(byte.MaxValue, contract.CrReadBeforeWriteMask);
        Assert.Equal((byte)(1 << 3), contract.CrPossibleWriteMask);
        Assert.False(contract.HasFullSynchronizationFence);
    }

    [Fact]
    public void UnknownHelperIsACompleteContextBoundary()
    {
        var function = new IrFunction("unknown_helper", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(string.Empty, "PPC_NewUncataloguedHelper", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var contract = GuestAbiContractAnalyzer.Analyze(function);

        Assert.True(contract.HasFullSynchronizationFence);
        Assert.Equal(uint.MaxValue, contract.GprReadBeforeWriteMask);
        Assert.Equal(uint.MaxValue, contract.GprPossibleWriteMask);
        Assert.Equal(uint.MaxValue, contract.FprReadBeforeWriteMask);
        Assert.Equal(uint.MaxValue, contract.FprPossibleWriteMask);
        Assert.Equal(byte.MaxValue, contract.CrReadBeforeWriteMask);
        Assert.Equal(byte.MaxValue, contract.CrPossibleWriteMask);
        Assert.True(contract.ReadsXerBeforeWrite);
        Assert.True(contract.MayWriteXer);
        Assert.True(contract.ReadsCtrBeforeWrite);
        Assert.True(contract.MayWriteCtr);
        Assert.True(contract.ReadsLrBeforeWrite);
        Assert.True(contract.MayWriteLr);
    }

    [Fact]
    public void TracksSpecialRegistersEmbeddedInRawBranchConditions()
    {
        var function = new IrFunction("raw_branch", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBranch("raw", "taken", "fallthrough",
                    "((ctx->ctr != 0) && GetCRBit(ctx, 2, 1))")
            }),
            new IrBasicBlock("taken", new IrInstruction[] { new IrReturn(null) }),
            new IrBasicBlock("fallthrough", new IrInstruction[] { new IrReturn(null) })
        });

        var contract = GuestAbiContractAnalyzer.Analyze(function);

        Assert.True(contract.ReadsCtrBeforeWrite);
        Assert.Equal(byte.MaxValue, contract.CrReadBeforeWriteMask);
    }
}
