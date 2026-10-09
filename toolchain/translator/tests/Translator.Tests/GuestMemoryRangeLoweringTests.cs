using System.Linq;
using System.Collections.Generic;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public sealed class GuestMemoryRangeLoweringTests
{
    [Fact]
    public void KeepsNonOverlappingReadAndWriteRangesSeparate()
    {
        var lowered = Lower(
            new IrLoad("r4", new IrAddress("r3", 12), 4),
            new IrLoad("r5", new IrAddress("r3", 16), 4),
            new IrStore(new IrAddress("r3", 20), IrValue.Register("r6"), 4),
            new IrStore(new IrAddress("r3", 24), IrValue.Register("r7"), 4));

        var resolves = lowered.OfType<IrResolveGuestMemoryRange>().ToArray();
        Assert.Equal(2, resolves.Length);
        Assert.Contains(resolves, resolve => resolve.NeedsReadAccess && !resolve.NeedsWriteAccess);
        Assert.Contains(resolves, resolve => !resolve.NeedsReadAccess && resolve.NeedsWriteAccess);
        Assert.Single(lowered.OfType<IrResolvedLoadPair>());
        Assert.Single(lowered.OfType<IrResolvedStorePair>());
    }

    [Fact]
    public void DoesNotSplitOverlappingReadAndWriteAliasesIntoIndependentRanges()
    {
        var lowered = Lower(
            new IrLoad("r4", new IrAddress("r3", 0), 8),
            new IrLoad("r5", new IrAddress("r3", 8), 8),
            new IrStore(new IrAddress("r3", 4), IrValue.Register("r6"), 8),
            new IrStore(new IrAddress("r3", 12), IrValue.Register("r7"), 8));

        Assert.Empty(lowered.OfType<IrResolveGuestMemoryRange>());
        Assert.Equal(2, lowered.OfType<IrLoad>().Count());
        Assert.Equal(2, lowered.OfType<IrStore>().Count());
    }

    [Fact]
    public void CanonicalizesReusedEffectiveAddressCalculations()
    {
        var lowered = Lower(
            new IrBinary("ea0", IrValue.Register("r3"), IrValue.Imm(16), "add"),
            new IrLoad("r4", new IrAddress("ea0", 0), 4),
            new IrBinary("ea1", IrValue.Register("r3"), IrValue.Imm(20), "add"),
            new IrLoad("r5", new IrAddress("ea1", 0), 4),
            new IrLoad("r6", new IrAddress("r3", 24), 4));

        var resolve = Assert.Single(lowered.OfType<IrResolveGuestMemoryRange>());
        Assert.Equal("r3", resolve.Base.RegisterName);
        Assert.Equal(16, resolve.MinOffset);
        Assert.Equal(12, resolve.Length);
    }

    [Fact]
    public void DoesNotRetainRangeAcrossCallBoundary()
    {
        var lowered = Lower(
            new IrLoad("r4", new IrAddress("r3", 0), 4),
            new IrCall("", "func_80001000", []),
            new IrLoad("r5", new IrAddress("r3", 4), 4));

        Assert.Empty(lowered.OfType<IrResolveGuestMemoryRange>());
        Assert.Equal(2, lowered.OfType<IrLoad>().Count());
    }

    [Fact]
    public void ReusesLiveInRangeAcrossCfgInsideCallFreeSuffixEpoch()
    {
        var function = new IrFunction("call_suffix", "entry",
        [
            new IrBasicBlock("entry",
            [
                new IrCall("", "func_80001000", []),
                new IrJump("body")
            ]),
            new IrBasicBlock("body",
            [
                new IrLoad("r4", new IrAddress("r3_0", 0), 4),
                new IrJump("tail")
            ]),
            new IrBasicBlock("tail",
            [
                new IrLoad("r5", new IrAddress("r3_0", 4), 4),
                new IrReturn(null)
            ])
        ]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);
        Assert.DoesNotContain(lowered.Blocks[0].Instructions,
            instruction => instruction is IrResolveGuestMemoryRange);
        Assert.IsType<IrResolveGuestMemoryRange>(lowered.Blocks[1].Instructions[0]);
        Assert.IsType<IrResolvedLoad>(lowered.Blocks[1].Instructions[1]);
        Assert.Equal(2, lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolvedLoad>().Count());
    }

    [Fact]
    public void ReusesRangeAcrossBranchesWhenAnActualAccessDominatesConsumers()
    {
        var function = new IrFunction("branch", "entry",
        [
            new IrBasicBlock("entry",
            [
                new IrLoad("r4", new IrAddress("r3_0", 0), 4),
                new IrBranch("ne", "left", "right")
            ]),
            new IrBasicBlock("left",
            [
                new IrLoad("r5", new IrAddress("r3_0", 4), 4),
                new IrReturn(null)
            ]),
            new IrBasicBlock("right",
            [
                new IrLoad("r6", new IrAddress("r3_0", 8), 4),
                new IrReturn(null)
            ])
        ]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);
        Assert.IsType<IrResolveGuestMemoryRange>(lowered.Blocks[0].Instructions[0]);
        Assert.Equal(3, lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolvedLoad>().Count());
    }

    [Fact]
    public void CallOnLoopBackedgePreventsLoopCarriedHostPointer()
    {
        var function = new IrFunction("loop_boundary", "entry",
        [
            new IrBasicBlock("entry", [new IrJump("loop")]),
            new IrBasicBlock("loop",
            [
                new IrLoad("r4", new IrAddress("r3_0", 0), 4),
                new IrCall("", "func_80001000", []),
                new IrLoad("r5", new IrAddress("r3_0", 4), 4),
                new IrJump("loop")
            ])
        ]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);
        Assert.Empty(lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolveGuestMemoryRange>());
    }

    [Fact]
    public void ArchitecturalRegisterRedefinitionOnOnePathPreventsReuseAtJoin()
    {
        var function = new IrFunction("base_redefinition", "entry",
        [
            new IrBasicBlock("entry",
            [
                new IrLoad("r4_1", new IrAddress("r3_0", 0), 4),
                new IrBranch("ne", "stable", "changed")
            ]),
            new IrBasicBlock("stable", [new IrJump("join")]),
            new IrBasicBlock("changed",
            [
                new IrAssign("r3_1", IrValue.Register("r5_0")),
                new IrJump("join")
            ]),
            new IrBasicBlock("join",
            [
                new IrLoad("r6_1", new IrAddress("r3_0", 4), 4),
                new IrReturn(null)
            ])
        ]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);
        Assert.Empty(lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolveGuestMemoryRange>());
    }

    [Fact]
    public void BaseRedefinitionAfterLoopConsumerPreventsReuseOnNextIteration()
    {
        var function = new IrFunction("loop_redefinition", "entry",
        [
            new IrBasicBlock("entry",
            [
                new IrLoad("r4_1", new IrAddress("r3_0", 0), 4),
                new IrJump("loop")
            ]),
            new IrBasicBlock("loop",
            [
                new IrLoad("r5_1", new IrAddress("r3_0", 4), 4),
                new IrAssign("r3_1", IrValue.Register("r6_0")),
                new IrJump("loop")
            ])
        ]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);
        Assert.Empty(lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolveGuestMemoryRange>());
    }

    [Fact]
    public void CatalogHiddenGprWriteEndsEpochEvenForPpcNamedHelper()
    {
        var lowered = Lower(
            new IrLoad("r4", new IrAddress("r3", 0), 4),
            new IrCall("", "PPC_Lswi", [IrValue.Imm(4), IrValue.Register("r5"), IrValue.Imm(4)]),
            new IrLoad("r5", new IrAddress("r3", 4), 4));

        Assert.Empty(lowered.OfType<IrResolveGuestMemoryRange>());
    }

    [Fact]
    public void OrdinaryCxxPipelineEnablesEightAccessScalarSuffixAfterCall()
    {
        var instructions = new List<IrInstruction>
        {
            new IrCall("", "func_80002000", [])
        };
        for (var index = 0; index < 8; index++)
            instructions.Add(new IrLoad($"r{index + 4}", new IrAddress("r3", index * 4), 4));
        instructions.Add(new IrReturn(null));
        var function = new IrFunction("ordinary_epoch", "entry",
            [new IrBasicBlock("entry", instructions)]);
        var types = new RepresentationEnvironment(Enumerable.Range(0, 12)
            .ToDictionary(index => $"r{index}", _ => ValueRepresentation.UInt32));

        var code = new CxxLinearCodeGenerator().Emit(
            0x80001000,
            new SsaResult(function, IrCfg.Build(function)),
            new FunctionAbiClassification("ordinary_epoch", ValueRepresentation.UInt32),
            types);

        Assert.Contains("MemoryInline::ResolveRangeHost", code, System.StringComparison.Ordinal);
        Assert.Equal(8, System.Text.RegularExpressions.Regex.Matches(code, "ReadResolved32").Count);
    }

    [Theory]
    [InlineData("Memory_RemapGuestPages")]
    [InlineData("Fiber_YieldToScheduler")]
    public void RemapAndSchedulerCallsAlwaysSplitMemoryEpochs(string target)
    {
        var function = new IrFunction("boundary", "entry",
        [
            new IrBasicBlock("entry",
            [
                new IrLoad("r4", new IrAddress("r3_0", 0), 4),
                new IrCall("", target, []),
                new IrJump("tail")
            ]),
            new IrBasicBlock("tail",
            [
                new IrLoad("r5", new IrAddress("r3_0", 4), 4),
                new IrReturn(null)
            ])
        ]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);
        Assert.Empty(lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolveGuestMemoryRange>());
        Assert.Equal(2, lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrLoad>().Count());
    }

    [Fact]
    public void IndirectCallAlwaysSplitsMemoryEpochs()
    {
        var function = new IrFunction("indirect_boundary", "entry",
        [
            new IrBasicBlock("entry",
            [
                new IrLoad("r4", new IrAddress("r3_0", 0), 4),
                new IrIndirectCall("", IrValue.Register("ctr_0"), []),
                new IrJump("tail")
            ]),
            new IrBasicBlock("tail",
            [
                new IrLoad("r5", new IrAddress("r3_0", 4), 4),
                new IrReturn(null)
            ])
        ]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);
        Assert.Empty(lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolveGuestMemoryRange>());
    }

    [Fact]
    public void KeepsSeparateRangesForBasesLoadedThroughAFunctionLiveInRange()
    {
        // Regression: the affine tracker must recognize IrResolvedLoad results (from the
        // function-level live-in pass) as fresh r3 definitions, or every store after the
        // first redirects through the first pointer and the minimap shadow panes lose their scale.
        var lowered = Lower(
            new IrLoad("r3", new IrAddress("r28", 440), 4),
            new IrStore(new IrAddress("r3", 68), IrValue.Register("f0"), 4),
            new IrStore(new IrAddress("r3", 72), IrValue.Register("f0"), 4),
            new IrLoad("r3", new IrAddress("r28", 444), 4),
            new IrStore(new IrAddress("r3", 68), IrValue.Register("f1"), 4),
            new IrStore(new IrAddress("r3", 72), IrValue.Register("f1"), 4),
            new IrLoad("r3", new IrAddress("r28", 448), 4),
            new IrStore(new IrAddress("r3", 68), IrValue.Register("f2"), 4),
            new IrStore(new IrAddress("r3", 72), IrValue.Register("f2"), 4));

        // One write range per pane, each spanning only that pane's scale pair.
        var writeRanges = lowered.OfType<IrResolveGuestMemoryRange>()
            .Where(static range => range.NeedsWriteAccess)
            .ToArray();
        Assert.Equal(3, writeRanges.Length);
        Assert.All(writeRanges, static range => Assert.Equal(8, range.Length));
    }

    [Fact]
    public void TracksAffineBaseUpdateBetweenAccesses()
    {
        // lwzu-style walk: the affine tracker folds the constant base update into the
        // second access's offset, so both loads share one range anchored at the original base.
        var lowered = Lower(
            new IrLoad("r4", new IrAddress("r3", 0), 4),
            new IrBinary("r3", IrValue.Register("r3"), IrValue.Imm(4), "add"),
            new IrLoad("r5", new IrAddress("r3", 0), 4));

        var resolve = Assert.Single(lowered.OfType<IrResolveGuestMemoryRange>());
        Assert.Equal("r3", resolve.Base.RegisterName);
        Assert.Equal(0, resolve.MinOffset);
        Assert.Equal(8, resolve.Length);
    }

    [Fact]
    public void TracksAffineBaseUpdateThroughTemporaryReassignment()
    {
        // psq_stu emission shape: the effective address lives in a temp and is
        // then copied back into the architectural base.
        var lowered = Lower(
            new IrStore(new IrAddress("r10", 8), IrValue.Register("r6"), 8),
            new IrBinary("r10_psq_ea_0", IrValue.Register("r10"), IrValue.Imm(8), "add"),
            new IrAssign("r10", IrValue.Register("r10_psq_ea_0")),
            new IrStore(new IrAddress("r10", 8), IrValue.Register("r7"), 8),
            new IrBinary("r10_psq_ea_1", IrValue.Register("r10"), IrValue.Imm(8), "add"),
            new IrAssign("r10", IrValue.Register("r10_psq_ea_1")),
            new IrStore(new IrAddress("r10", 8), IrValue.Register("r8"), 8));

        var resolve = Assert.Single(lowered.OfType<IrResolveGuestMemoryRange>());
        Assert.Equal("r10", resolve.Base.RegisterName);
        Assert.Equal(8, resolve.MinOffset);
        Assert.Equal(24, resolve.Length);
        var stores = lowered.OfType<IrResolvedStore>().ToArray();
        Assert.Equal(3, stores.Length);
        Assert.Equal(0, stores[0].RangeOffset);
        Assert.Equal(8, stores[1].RangeOffset);
        Assert.Equal(16, stores[2].RangeOffset);
    }

    [Fact]
    public void RejectsGroupWhenBaseChangesUnpredictablyBetweenAccesses()
    {
        // A base redefinition the tracker can't follow starts a new generation, so accesses
        // through different runtime values never share one range proof.
        var lowered = Lower(
            new IrLoad("r4", new IrAddress("r3", 0), 4),
            new IrLoad("r3", new IrAddress("r4", 0), 4),
            new IrLoad("r5", new IrAddress("r3", 0), 4));

        Assert.Empty(lowered.OfType<IrResolveGuestMemoryRange>());
    }

    [Fact]
    public void RejectsCrossPageSizedRanges()
    {
        var lowered = Lower(
            new IrLoad("r4", new IrAddress("r3", 0), 4),
            new IrLoad("r5", new IrAddress("r3", 5000), 4));

        Assert.Empty(lowered.OfType<IrResolveGuestMemoryRange>());
    }

    [Fact]
    public void GroupsAdjacentPairedSingleLoadsAndPreservesKnownGqr()
    {
        var lowered = Lower(
            new IrCall("f1", "PPC_PsqLKnown_00050000", [IrValue.Register("r3"), IrValue.Imm(0), IrValue.Imm(2)]),
            new IrBinary("ea", IrValue.Register("r3"), IrValue.Imm(4), "add"),
            new IrCall("f2", "PPC_PsqLKnown_00050000", [IrValue.Register("ea"), IrValue.Imm(1), IrValue.Imm(2)]),
            new IrCall("f3", "PPC_PsqLKnown_00050000", [IrValue.Register("r3"), IrValue.Imm(1), IrValue.Imm(2)]));

        var resolve = Assert.Single(lowered.OfType<IrResolveGuestMemoryRange>());
        Assert.True(resolve.NeedsReadAccess);
        Assert.False(resolve.NeedsWriteAccess);
        Assert.Equal(8, resolve.Length);
        var loads = lowered.OfType<IrResolvedPsqLoad>().ToArray();
        Assert.Equal(3, loads.Length);
        Assert.Equal(0x00050000u, loads[0].KnownGqr);
        Assert.Equal(4, loads[1].RangeOffset);
    }

    [Fact]
    public void CombinesConsecutiveAdjacentHalfwordLoads()
    {
        var lowered = Lower(
            new IrLoad("r4", new IrAddress("r3", 8), 2),
            new IrLoad("r5", new IrAddress("r3", 10), 2));

        var pair = Assert.Single(lowered.OfType<IrResolvedLoadPair>());
        Assert.Equal(2, pair.ElementSizeBytes);
        Assert.Equal(0, pair.RangeOffset);
        Assert.Equal("r4", pair.FirstDestination);
        Assert.Equal("r5", pair.SecondDestination);
    }

    [Fact]
    public void CombinesConsecutiveAdjacentWordStores()
    {
        var lowered = Lower(
            new IrStore(new IrAddress("r3", 0), IrValue.Register("r4"), 4),
            new IrStore(new IrAddress("r3", 4), IrValue.Register("r5"), 4));

        var pair = Assert.Single(lowered.OfType<IrResolvedStorePair>());
        Assert.Equal(4, pair.ElementSizeBytes);
        Assert.Equal(0, pair.RangeOffset);
    }

    [Fact]
    public void CombinesAdjacentStoresAcrossPureEffectiveAddressCalculation()
    {
        var lowered = Lower(
            new IrStore(new IrAddress("r3", 0), IrValue.Register("r4"), 4),
            new IrBinary("ea_next", IrValue.Register("r3"), IrValue.Imm(4), "add"),
            new IrStore(new IrAddress("ea_next", 0), IrValue.Register("r5"), 4));

        var pair = Assert.Single(lowered.OfType<IrResolvedStorePair>());
        Assert.False(pair.Descending);
        Assert.Contains(lowered, instruction => instruction is IrBinary { Destination: "ea_next" });
    }

    [Fact]
    public void CombinesDescendingAdjacentLoadsAndPreservesDirection()
    {
        var lowered = Lower(
            new IrLoad("r4", new IrAddress("r3", 12), 4),
            new IrLoad("r5", new IrAddress("r3", 8), 4));

        var pair = Assert.Single(lowered.OfType<IrResolvedLoadPair>());
        Assert.True(pair.Descending);
        Assert.Equal(0, pair.RangeOffset);
        Assert.Equal("r4", pair.FirstDestination);
        Assert.Equal("r5", pair.SecondDestination);
    }

    [Fact]
    public void DoesNotInferRuntimeCachingFromCfgCycleAlone()
    {
        var function = new IrFunction("loop", "entry",
        [
            new IrBasicBlock("entry", [new IrJump("loop")]),
            new IrBasicBlock("loop",
            [
                new IrLoad("r4", new IrAddress("r3", 0), 4),
                new IrLoad("r5", new IrAddress("r3", 4), 4),
                new IrLoad("r6", new IrAddress("r3", 8), 4),
                new IrJump("loop")
            ])
        ]);

        var resolve = Assert.Single(GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2).Blocks
            .SelectMany(static block => block.Instructions).OfType<IrResolveGuestMemoryRange>());
    }

    [Fact]
    public void RejectsSmallStraightLineGroupAsUnprofitable()
    {
        var function = new IrFunction("straight", "entry",
        [
            new IrBasicBlock("entry",
            [
                new IrLoad("r4", new IrAddress("r3", 0), 4),
                new IrLoad("r5", new IrAddress("r3", 4), 4),
                new IrReturn(null)
            ])
        ]);
        Assert.Empty(GuestMemoryRangeLowering.Lower(function).Blocks[0].Instructions
            .OfType<IrResolveGuestMemoryRange>());
    }

    [Fact]
    public void RetainsLiveInRangeAcrossDominatedCfgBlocks()
    {
        var function = new IrFunction("cfg", "entry",
        [
            new IrBasicBlock("entry",
            [
                new IrLoad("r4", new IrAddress("r3_0", 0), 4),
                new IrJump("body")
            ]),
            new IrBasicBlock("body",
            [
                new IrLoad("r5", new IrAddress("r3_0", 4), 4),
                new IrReturn(null)
            ])
        ]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);
        Assert.Single(lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolveGuestMemoryRange>());
        Assert.Equal(2, lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolvedLoad>().Count());
    }

    [Fact]
    public void PlacesLoopCarriedPhiRangeAfterAuthoritativeDefinition()
    {
        var function = new IrFunction("phi_loop", "entry",
        [
            new IrBasicBlock("entry", [new IrJump("loop")]),
            new IrBasicBlock("loop",
            [
                new IrPhi("r3_1", new Dictionary<string, string>
                {
                    ["entry"] = "r3_0",
                    ["loop"] = "r3_2"
                }),
                new IrLoad("r4", new IrAddress("r3_1", 0), 4),
                new IrLoad("r5", new IrAddress("r3_1", 4), 4),
                new IrBinary("r3_2", IrValue.Register("r3_1"), IrValue.Imm(8), "add"),
                new IrJump("loop")
            ])
        ]);

        var loop = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2).Blocks[1].Instructions;
        Assert.IsType<IrPhi>(loop[0]);
        Assert.IsType<IrResolveGuestMemoryRange>(loop[1]);
        Assert.Single(loop.OfType<IrResolvedLoadPair>());
    }

    [Fact]
    public void KeepsProvenGatherPipeStoresOutOfResolvedRanges()
    {
        // Gather-pipe writes (`lis rX, 0xCC01; stX rY, -0x8000(rX)`) must not get hoisted into a
        // resolved range: ResolveRangeHost returns nullptr for that address at runtime, which
        // would drop the write into the cold path instead of the emitter's direct GX_HLE_FIFO_Write*.
        var instructions = new List<IrInstruction>
        {
            new IrAssign("r10", IrValue.Imm(unchecked((int)0xCC010000)))
        };
        for (var index = 0; index < 13; index++)
            instructions.Add(new IrStore(new IrAddress("r10", -32768), IrValue.Register($"r{index % 8}"), 4));
        instructions.Add(new IrReturn(null));
        var function = new IrFunction("gather_pipe", "entry",
            [new IrBasicBlock("entry", instructions)]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);

        Assert.Empty(lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolveGuestMemoryRange>());
        Assert.Equal(13, lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrStore>().Count());
    }

    [Fact]
    public void KeepsProvenHardwareRegisterLoadsOutOfResolvedRanges()
    {
        var instructions = new List<IrInstruction>
        {
            new IrAssign("r10", IrValue.Imm(unchecked((int)0xCD000000)))
        };
        for (var index = 0; index < 8; index++)
            instructions.Add(new IrLoad($"r{index + 3}", new IrAddress("r10", index * 4), 4));
        instructions.Add(new IrReturn(null));
        var function = new IrFunction("hardware_reads", "entry",
            [new IrBasicBlock("entry", instructions)]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);

        Assert.Empty(lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolveGuestMemoryRange>());
        Assert.Equal(8, lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrLoad>().Count());
    }

    [Fact]
    public void StillHoistsStoresThroughAProvenMainMemoryBase()
    {
        // The exclusion is keyed on the hardware window only: an ordinary
        // absolute MEM1 base must keep its range proof.
        var instructions = new List<IrInstruction>
        {
            new IrAssign("r10", IrValue.Imm(unchecked((int)0x80300000)))
        };
        for (var index = 0; index < 13; index++)
            instructions.Add(new IrStore(new IrAddress("r10", index * 4), IrValue.Register($"r{index % 8}"), 4));
        instructions.Add(new IrReturn(null));
        var function = new IrFunction("main_memory", "entry",
            [new IrBasicBlock("entry", instructions)]);

        var lowered = GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2);

        Assert.NotEmpty(lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrResolveGuestMemoryRange>());
        Assert.Empty(lowered.Blocks.SelectMany(static block => block.Instructions)
            .OfType<IrStore>());
    }

    [Fact]
    public void EmitsDirectFifoWritesForProvenGatherPipeStores()
    {
        var instructions = new List<IrInstruction>
        {
            new IrAssign("r10", IrValue.Imm(unchecked((int)0xCC010000)))
        };
        for (var index = 0; index < 13; index++)
            instructions.Add(new IrStore(new IrAddress("r10", -32768), IrValue.Register("r3"), 4));
        instructions.Add(new IrReturn(null));
        var function = new IrFunction("gather_pipe_emit", "entry",
            [new IrBasicBlock("entry", instructions)]);
        var types = new RepresentationEnvironment(Enumerable.Range(0, 12)
            .ToDictionary(index => $"r{index}", _ => ValueRepresentation.UInt32));

        var code = new CxxLinearCodeGenerator().Emit(
            0x800605C0,
            new SsaResult(function, IrCfg.Build(function)),
            new FunctionAbiClassification("gather_pipe_emit", ValueRepresentation.UInt32),
            types);

        Assert.DoesNotContain("MemoryInline::ResolveRangeHost", code, System.StringComparison.Ordinal);
        Assert.DoesNotContain("WriteResolved", code, System.StringComparison.Ordinal);
        Assert.Contains("GX_HLE_FIFO_", code, System.StringComparison.Ordinal);
    }

    private static IrInstruction[] Lower(params IrInstruction[] instructions)
    {
        var loopInstructions = instructions.Append<IrInstruction>(new IrJump("entry")).ToArray();
        var function = new IrFunction("test", "entry",
            [new IrBasicBlock("entry", loopInstructions)]);
        return GuestMemoryRangeLowering.Lower(function, minimumAccesses: 2).Blocks[0].Instructions.ToArray();
    }
}
