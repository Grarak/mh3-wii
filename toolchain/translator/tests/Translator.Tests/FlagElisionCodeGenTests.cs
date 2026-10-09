using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

/// <summary>
/// Compare+branch fusion, dead CR-field/XER.CA elimination, and LR store elision. Negative cases
/// outnumber positive ones because each would be a silent-corruption bug if it started firing.
/// </summary>
public class FlagElisionCodeGenTests
{
    private static string Emit(
        IrFunction function,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts = null,
        IReadOnlySet<uint>? modOverridableCallTargets = null,
        IReadOnlySet<uint>? lrContinuationCallTargets = null) =>
        new CxxLinearCodeGenerator().Emit(
            0x80001000u,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            guestAbiContracts: guestAbiContracts,
            lrContinuationCallTargets: lrContinuationCallTargets,
            modOverridableCallTargets: modOverridableCallTargets);

    private static GuestAbiContract Contract(
        bool readsLr = false,
        bool readsXer = false,
        bool writesXer = false,
        byte crRead = 0,
        byte crWrite = 0,
        GuestCallBoundaryFlags flags = GuestCallBoundaryFlags.None) =>
        new(
            GprReadBeforeWriteMask: 0,
            GprPossibleWriteMask: 0,
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
            ReadsLrBeforeWrite: readsLr,
            MayWriteLr: false,
            BoundaryFlags: flags,
            DirectCallTargets: Array.Empty<uint>());

    /// <summary>A block that overwrites CR0 and returns, so reaching it on every outgoing edge
    /// makes the CR0 producer dead (and its own CR0 write too: the return reads only CR2-CR4).</summary>
    private static IrBasicBlock Cr0KillingExit(string label, string register) =>
        new(label, new IrInstruction[]
        {
            new IrSetCrField(0, IrValue.Register(register), IrValue.Imm(0), false),
            new IrReturn(null)
        });

    private static IrFunction CompareThenBranch(
        string name,
        string condition,
        bool isUnsigned,
        IrValue right)
    {
        return new IrFunction(name, "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("r3"), right, isUnsigned),
                new IrBranch(condition, "taken", "fallthrough", "cr0")
            }),
            Cr0KillingExit("taken", "r4"),
            Cr0KillingExit("fallthrough", "r5")
        });
    }

    [Theory]
    [InlineData("beq", false, "(static_cast<int32_t>(r3) == static_cast<int32_t>(0))")]
    [InlineData("bne", false, "(static_cast<int32_t>(r3) != static_cast<int32_t>(0))")]
    [InlineData("blt", false, "(static_cast<int32_t>(r3) < static_cast<int32_t>(0))")]
    [InlineData("bge", false, "(static_cast<int32_t>(r3) >= static_cast<int32_t>(0))")]
    [InlineData("bgt", false, "(static_cast<int32_t>(r3) > static_cast<int32_t>(0))")]
    [InlineData("ble", false, "(static_cast<int32_t>(r3) <= static_cast<int32_t>(0))")]
    [InlineData("blt", true, "(static_cast<uint32_t>(r3) < static_cast<uint32_t>(0))")]
    [InlineData("bge", true, "(static_cast<uint32_t>(r3) >= static_cast<uint32_t>(0))")]
    public void CompareFeedingItsOnlyBranchBecomesADirectComparison(
        string condition, bool isUnsigned, string expected)
    {
        var code = Emit(CompareThenBranch($"fuse_{condition}", condition, isUnsigned, IrValue.Imm(0)));

        Assert.Contains($"if ({expected})", code, StringComparison.Ordinal);
        // The exit blocks' own CR0 writes are dead too: a return publishes
        // only CR2-CR4.
        Assert.Equal(0, CountOccurrences(code, "SetCRResident("));
    }

    [Fact]
    public void FusionUsesTheRegisterOperandOfARegisterToRegisterCompare()
    {
        var code = Emit(CompareThenBranch("fuse_reg", "bgt", isUnsigned: false, IrValue.Register("r6")));

        Assert.Contains(
            "if ((static_cast<int32_t>(r3) > static_cast<int32_t>(r6)))",
            code, StringComparison.Ordinal);
    }

    [Fact]
    public void FusionReachesTheCompareThroughASingleEntrySinglExitPredecessor()
    {
        // The basic-block builder frequently starts a new block at the branch,
        // which puts the compare in the sole predecessor.
        var function = new IrFunction("fuse_across_block", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(7), false)
            }),
            new IrBasicBlock("branch", new IrInstruction[]
            {
                new IrBranch("beq", "taken", "fallthrough", "cr0")
            }),
            Cr0KillingExit("taken", "r4"),
            Cr0KillingExit("fallthrough", "r5")
        });

        var code = Emit(function);

        Assert.Contains(
            "if ((static_cast<int32_t>(r3) == static_cast<int32_t>(7)))",
            code, StringComparison.Ordinal);
        Assert.Equal(0, CountOccurrences(code, "SetCRResident("));
    }

    [Theory]
    [InlineData("blt", "if ((f1.d < f2.d))")]
    [InlineData("bge", "if ((!(f1.d < f2.d)))")]
    [InlineData("beq", "if ((f1.d == f2.d))")]
    [InlineData("ble", "if ((!(f1.d > f2.d)))")]
    public void FloatCompareFusionKeepsThePowerPcBitSemantics(string condition, string expected)
    {
        // A cleared LT bit means "not less than", which is true for an unordered
        // compare; spelling it as >= would be wrong for NaN.
        var function = new IrFunction($"fuse_float_{condition}", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("f1"), IrValue.Register("f2"), false),
                new IrBranch(condition, "taken", "fallthrough", "cr0")
            }),
            Cr0KillingExit("taken", "r4"),
            Cr0KillingExit("fallthrough", "r5")
        });

        var code = Emit(function);

        Assert.Contains(expected, code, StringComparison.Ordinal);
        Assert.DoesNotContain("SetCRFloatResident(", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CompareIsNotFusedWhenTheFieldIsStillReadAfterTheBranch()
    {
        var function = new IrFunction("live_across_branch", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrBranch("beq", "taken", "fallthrough", "cr0")
            }),
            // Reads CR0 again before writing it: the packed field is live.
            new IrBasicBlock("taken", new IrInstruction[]
            {
                new IrBranch("bgt", "fallthrough", "second", "cr0")
            }),
            Cr0KillingExit("second", "r4"),
            Cr0KillingExit("fallthrough", "r5")
        });

        var code = Emit(function);

        Assert.DoesNotContain("static_cast<int32_t>(r3) == static_cast<int32_t>(0)", code, StringComparison.Ordinal);
        Assert.Contains("SetCRResident(cr, xer, 0, static_cast<int32_t>(r3)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CompareIsNotFusedWhenTheBranchTestsTheSummaryOverflowBit()
    {
        // bso reads the XER.SO copy, which a direct comparison cannot rebuild.
        var code = Emit(CompareThenBranch("so_consumer", "bso", isUnsigned: false, IrValue.Imm(0)));

        Assert.Contains("SetCRResident(cr, xer, 0, static_cast<int32_t>(r3)", code, StringComparison.Ordinal);
        // The exits' CR0 writes are dead (a return publishes only CR2-CR4).
        Assert.Equal(1, CountOccurrences(code, "SetCRResident("));
    }

    [Fact]
    public void CompareIsNotFusedWhenAnMfcrObservesThePackedRegister()
    {
        var function = new IrFunction("mfcr_reader", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrBranch("beq", "taken", "fallthrough", "cr0")
            }),
            new IrBasicBlock("taken", new IrInstruction[]
            {
                new IrAssign("r7", IrValue.Register("cr")),
                new IrReturn(null)
            }),
            Cr0KillingExit("fallthrough", "r5")
        });

        var code = Emit(function);

        Assert.Contains("SetCRResident(cr, xer, 0, static_cast<int32_t>(r3)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CompareIsNotFusedWhenAnOperandIsRedefinedBeforeTheBranch()
    {
        // The fused comparison is evaluated at the branch, where r3 no longer
        // holds the compared value.
        var function = new IrFunction("clobbered_operand", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Imm(1), "add"),
                new IrBranch("beq", "taken", "fallthrough", "cr0")
            }),
            Cr0KillingExit("taken", "r4"),
            Cr0KillingExit("fallthrough", "r5")
        });

        var code = Emit(function);

        Assert.Contains("SetCRResident(cr, xer, 0, static_cast<int32_t>(r3)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("if ((static_cast<int32_t>(r3) == static_cast<int32_t>(0)))", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CompareIsNotFusedAcrossACall()
    {
        // CR2 survives the call, but r3 may not.
        var function = new IrFunction("call_between", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(2, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrBranch("beq", "taken", "fallthrough", "cr2")
            }),
            Cr0KillingExit("taken", "r4"),
            Cr0KillingExit("fallthrough", "r5")
        });

        var code = Emit(
            function,
            guestAbiContracts: new Dictionary<uint, GuestAbiContract> { [0x80002000u] = Contract() });

        Assert.Contains("SetCRResident(cr, xer, 2, static_cast<int32_t>(r3)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CompareIsNotFusedIntoTheFunctionEntryBlock()
    {
        // "branch" is the entry label, so control reaches it without ever
        // executing the block that happens to be its only recorded predecessor.
        var function = new IrFunction("entry_is_branch", "branch", new[]
        {
            new IrBasicBlock("branch", new IrInstruction[]
            {
                new IrBranch("beq", "taken", "fallthrough", "cr0")
            }),
            new IrBasicBlock("producer", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrJump("branch")
            }),
            Cr0KillingExit("taken", "r4"),
            Cr0KillingExit("fallthrough", "r5")
        });

        var code = Emit(function);

        Assert.DoesNotContain("static_cast<int32_t>(r3) == static_cast<int32_t>(0)", code, StringComparison.Ordinal);
        Assert.Contains("SetCRResident(cr, xer, 0, static_cast<int32_t>(r3)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void NoFlagIsElidedInABodyThatCanResumeThroughALinkRegisterDispatch()
    {
        // A callee that returns into one of this function's own labels makes the
        // static CFG incomplete, so every flag statement derived from it is void.
        var function = new IrFunction("lr_continuation", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("lr", IrValue.Imm(0x80001010)),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrBranch("beq", "taken", "fallthrough", "cr0")
            }),
            Cr0KillingExit("taken", "r4"),
            Cr0KillingExit("fallthrough", "r5")
        });

        var code = Emit(
            function,
            guestAbiContracts: new Dictionary<uint, GuestAbiContract> { [0x80002000u] = Contract() },
            lrContinuationCallTargets: new HashSet<uint> { 0x80002000u });

        Assert.Equal(3, CountOccurrences(code, "SetCRResident("));
        Assert.Contains("ctx->lr = 2147487760;", code, StringComparison.Ordinal);
    }

    private static IrFunction CallAfterBranch(int field) =>
        new("call_after_branch", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(field, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrBranch("beq", "taken", "fallthrough", $"cr{field}")
            }),
            new IrBasicBlock("taken", new IrInstruction[]
            {
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrSetCrField(field, IrValue.Register("r4"), IrValue.Imm(0), false),
                new IrReturn(null)
            }),
            new IrBasicBlock("fallthrough", new IrInstruction[]
            {
                new IrSetCrField(field, IrValue.Register("r5"), IrValue.Imm(0), false),
                new IrReturn(null)
            })
        });

    [Fact]
    public void ACalleeThatDoesNotReadTheFieldDoesNotKeepItAlive()
    {
        // This is what makes the transform pay off in a real build: without an
        // ABI contract every call after the branch is a potential CR reader.
        // CR2 is one a callee may read: it preserves it for the caller.
        var function = CallAfterBranch(2);
        var contracts = new Dictionary<uint, GuestAbiContract> { [0x80002000u] = Contract() };
        Assert.Contains(
            "if ((static_cast<int32_t>(r3) == static_cast<int32_t>(0)))",
            Emit(function, guestAbiContracts: contracts), StringComparison.Ordinal);

        // The same callee declared as a CR2 reader keeps the compare.
        var readerContracts = new Dictionary<uint, GuestAbiContract>
        {
            [0x80002000u] = Contract(crRead: 0x04)
        };
        Assert.Contains(
            "SetCRResident(cr, xer, 2, static_cast<int32_t>(r3)",
            Emit(function, guestAbiContracts: readerContracts), StringComparison.Ordinal);

        // ...and so does an unknown callee.
        Assert.Contains(
            "SetCRResident(cr, xer, 2, static_cast<int32_t>(r3)",
            Emit(function), StringComparison.Ordinal);
    }

    [Fact]
    public void NoCalleeReadsAVolatileField()
    {
        // CR0 is no call input under the EABI, whatever a contract claims, and
        // an unknown callee follows the EABI too.
        var function = CallAfterBranch(0);
        var readerContracts = new Dictionary<uint, GuestAbiContract>
        {
            [0x80002000u] = Contract(crRead: 0x01)
        };
        Assert.Contains(
            "if ((static_cast<int32_t>(r3) == static_cast<int32_t>(0)))",
            Emit(function, guestAbiContracts: readerContracts), StringComparison.Ordinal);
        Assert.Contains(
            "if ((static_cast<int32_t>(r3) == static_cast<int32_t>(0)))",
            Emit(function), StringComparison.Ordinal);
    }

    [Fact]
    public void ConditionHelpersReadOnlyTheirOwnFields()
    {
        // cror on CR0 bits and mtcrf's copy of the fields it keeps leave the
        // CR6 compare unread; mcrf reads its source field only.
        IrFunction Body(params IrInstruction[] between) =>
            new("cr_helpers", "entry", new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrSetCrField(6, IrValue.Register("r3"), IrValue.Imm(0), false),
                    new IrSetCrField(0, IrValue.Register("r4"), IrValue.Imm(0), false)
                }.Concat(between).Append(new IrReturn(null)).ToArray())
            });

        var cror = new IrCall("cr", "PPC_CrLogical", new[]
        {
            IrValue.Imm(7), IrValue.Imm(2), IrValue.Imm(1), IrValue.Imm(2)
        });
        var keepCr0 = new IrBinary("r12_mtcrf_preserve", IrValue.Register("cr"), IrValue.Imm(unchecked((int)0xF0000000u)), "and");
        var mcrfFromCr6 = new IrCall("cr", "PPC_Mcrf", new[] { IrValue.Imm(2), IrValue.Imm(6) });

        Assert.Equal(0, CountOccurrences(Emit(Body(cror)), "static_cast<int32_t>(r3)"));
        Assert.Equal(0, CountOccurrences(Emit(Body(keepCr0)), "static_cast<int32_t>(r3)"));
        Assert.Equal(1, CountOccurrences(Emit(Body(mcrfFromCr6)), "SetCRResident(cr, xer, 6, static_cast<int32_t>(r3)"));
    }

    [Fact]
    public void RecordFormWriteOverwrittenBeforeAnyReadIsRemoved()
    {
        // Two writes into the same field with no reader in between: only the
        // second can be observed (CR2, which the return publishes).
        var function = new IrFunction("dead_record_form", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Register("r4"), "add"),
                new IrSetCrField(2, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrBinary("r5", IrValue.Register("r5"), IrValue.Register("r6"), "add"),
                new IrSetCrField(2, IrValue.Register("r5"), IrValue.Imm(0), false),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.DoesNotContain("SetCRResident(cr, xer, 2, static_cast<int32_t>(r3)", code, StringComparison.Ordinal);
        Assert.Contains("SetCRResident(cr, xer, 2, static_cast<int32_t>(r5)", code, StringComparison.Ordinal);
        Assert.Equal(1, CountOccurrences(code, "SetCRResident("));
    }

    [Fact]
    public void LastNonvolatileCrWriteBeforeReturnIsKept()
    {
        // CR2-CR4 survive a call (EABI), so the caller may read them after
        // the return.
        var function = new IrFunction("cr_live_out", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(3, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Contains("SetCRResident(cr, xer, 3,", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(5)]
    [InlineData(6)]
    [InlineData(7)]
    public void LastVolatileCrWriteBeforeReturnIsRemoved(int field)
    {
        // No caller reads a volatile field a callee left behind.
        var function = new IrFunction("cr_dead_out", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(field, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrReturn(null)
            })
        });

        Assert.Equal(0, CountOccurrences(Emit(function), "SetCRResident("));
    }

    [Fact]
    public void RedundantCrWriteIsEliminated()
    {
        var function = new IrFunction("dead_cr_off", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(2, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrSetCrField(2, IrValue.Register("r5"), IrValue.Imm(0), false),
                new IrReturn(null)
            })
        });

        Assert.Equal(1, CountOccurrences(Emit(function), "SetCRResident("));
    }

    [Fact]
    public void XerBecomesAResidentLocal()
    {
        var function = new IrFunction("resident_xer", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(2, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Contains("uint32_t xer = ctx->xer;", code, StringComparison.Ordinal);
        Assert.Contains("SetCRResident(cr, xer, 2,", code, StringComparison.Ordinal);
        Assert.DoesNotContain("SetCRResident(cr, ctx->xer", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CarryUpdatesAndReadsUseTheResidentXerLocal()
    {
        var function = new IrFunction("resident_carry", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                {
                    IrValue.Register("r3"), IrValue.Register("r4"), IrValue.Imm(0)
                }),
                new IrCall("r7", "PPC_GetCarry", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Contains("xer = (xer & 0xDFFFFFFFu)", code, StringComparison.Ordinal);
        Assert.Contains("r7 = (xer >> 29) & 1u;", code, StringComparison.Ordinal);
        Assert.Contains("ctx->xer = xer;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->xer >> 29", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CarryUpdateOverwrittenBeforeAnyReadIsRemoved()
    {
        var function = new IrFunction("dead_carry", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                {
                    IrValue.Register("r3"), IrValue.Register("r4"), IrValue.Imm(0)
                }),
                new IrCall("xer", "PPC_UpdateCarrySub", new[]
                {
                    IrValue.Register("r5"), IrValue.Register("r6")
                }),
                new IrCall("r8", "PPC_GetCarry", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Equal(0, CountOccurrences(code, "const uint64_t ppcCarryWide"));
        Assert.Contains(
            "xer = (xer & 0xDFFFFFFFu) | ((static_cast<uint32_t>(r5) >= static_cast<uint32_t>(r6) ? 1u : 0u) << 29);",
            code, StringComparison.Ordinal);
    }

    [Fact]
    public void CarryUpdateIsKeptWhenAnAddeChainReadsIt()
    {
        var function = new IrFunction("live_carry", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                {
                    IrValue.Register("r3"), IrValue.Register("r4"), IrValue.Imm(0)
                }),
                new IrCall("r8", "PPC_GetCarry", Array.Empty<IrValue>()),
                new IrCall("xer", "PPC_UpdateCarrySub", new[]
                {
                    IrValue.Register("r5"), IrValue.Register("r6")
                }),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Equal(1, CountOccurrences(code, "const uint64_t ppcCarryWide"));
    }

    [Fact]
    public void CarryUpdateWhoseOnlyReaderIsTheReturnBoundaryIsRemoved()
    {
        // XER is volatile across a call: no caller reads the carry a callee left.
        var function = new IrFunction("carry_live_out", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                {
                    IrValue.Register("r3"), IrValue.Register("r4"), IrValue.Imm(0)
                }),
                new IrReturn(null)
            })
        });

        Assert.Equal(0, CountOccurrences(Emit(function), "const uint64_t ppcCarryWide"));
    }

    [Fact]
    public void CarryUpdateIsDeadAcrossACall()
    {
        // XER.CA is volatile: no callee reads it, whatever its contract claims,
        // and the update after the call reaches only the return.
        var function = new IrFunction("carry_across_call", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall("xer", "PPC_UpdateCarryAdd", new[]
                {
                    IrValue.Register("r3"), IrValue.Register("r4"), IrValue.Imm(0)
                }),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrCall("xer", "PPC_UpdateCarrySub", new[]
                {
                    IrValue.Register("r5"), IrValue.Register("r6")
                }),
                new IrReturn(null)
            })
        });

        var code = Emit(
            function,
            guestAbiContracts: new Dictionary<uint, GuestAbiContract>
            {
                [0x80002000u] = Contract(readsXer: true)
            });

        Assert.Equal(0, CountOccurrences(code, "const uint64_t ppcCarryWide"));
    }

    [Fact]
    public void CallLeavesNoVolatileFieldForTheCaller()
    {
        // The branch after the call reads what the callee left in CR0, never
        // the caller's compare before it.
        var function = new IrFunction("call_between_cr0", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrBranch("beq", "taken", "fallthrough", "cr0")
            }),
            Cr0KillingExit("taken", "r4"),
            Cr0KillingExit("fallthrough", "r5")
        });

        var code = Emit(
            function,
            guestAbiContracts: new Dictionary<uint, GuestAbiContract> { [0x80002000u] = Contract() });

        Assert.Equal(0, CountOccurrences(code, "SetCRResident("));
    }

    private static IrFunction CallWithLinkRegister(string name, uint target) =>
        new(name, "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("lr", IrValue.Imm(0x80001010)),
                new IrCall(string.Empty, $"0x{target:X8}", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

    [Fact]
    public void LinkRegisterStoreIsElidedForACalleeThatNeverReadsLr()
    {
        var code = Emit(
            CallWithLinkRegister("leaf_caller", 0x80002000u),
            guestAbiContracts: new Dictionary<uint, GuestAbiContract> { [0x80002000u] = Contract() });

        Assert.Contains("InvokeDirectCpu<0x80002000u>(ctx);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->lr = ", code, StringComparison.Ordinal);
    }

    [Fact]
    public void LinkRegisterStoreIsKeptForAnMflrCallee()
    {
        var code = Emit(
            CallWithLinkRegister("nonleaf_caller", 0x80002000u),
            guestAbiContracts: new Dictionary<uint, GuestAbiContract>
            {
                [0x80002000u] = Contract(readsLr: true)
            });

        Assert.Contains("ctx->lr = 2147487760;", code, StringComparison.Ordinal);
    }

    [Fact]
    public void LinkRegisterStoreIsKeptWithoutACalleeContract()
    {
        var code = Emit(CallWithLinkRegister("unknown_callee", 0x80002000u));

        Assert.Contains("ctx->lr = 2147487760;", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(GuestCallBoundaryFlags.RequiresCompleteContext)]
    [InlineData(GuestCallBoundaryFlags.CanSuspend)]
    [InlineData(GuestCallBoundaryFlags.CanSwitchThreads)]
    [InlineData(GuestCallBoundaryFlags.InvokesGuestCode)]
    public void LinkRegisterStoreIsKeptForACalleeThatEscapesAnalysis(GuestCallBoundaryFlags flags)
    {
        // setjmp-style natives (OSSaveContext) and anything that re-enters guest
        // code can observe the architectural return address.
        var code = Emit(
            CallWithLinkRegister("escaping_callee", 0x80002000u),
            guestAbiContracts: new Dictionary<uint, GuestAbiContract>
            {
                [0x80002000u] = Contract(flags: flags)
            });

        Assert.Contains("ctx->lr = 2147487760;", code, StringComparison.Ordinal);
    }

    [Fact]
    public void LinkRegisterStoreIsKeptForAModOverridableCallee()
    {
        var code = Emit(
            CallWithLinkRegister("mod_callee", 0x80002000u),
            guestAbiContracts: new Dictionary<uint, GuestAbiContract> { [0x80002000u] = Contract() },
            modOverridableCallTargets: new HashSet<uint> { 0x80002000u });

        Assert.Contains("ctx->lr = 2147487760;", code, StringComparison.Ordinal);
    }

    [Fact]
    public void DefinitionsCarryRestrictWhileDeclarationsDoNot()
    {
        var function = new IrFunction("restrict_probe", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(string.Empty, "func_helper_target", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var code = Emit(function);

        Assert.Contains("extern \"C\" void restrict_probe(CpuContext* MKW_RESTRICT ctx)", code, StringComparison.Ordinal);
        Assert.Contains("extern \"C\" void func_80001000(CpuContext* MKW_RESTRICT ctx)", code, StringComparison.Ordinal);
        // Top-level restrict is not part of the function type, so forward
        // declarations stay plain and still match.
        Assert.Contains("extern \"C\" void func_helper_target(CpuContext* ctx);", code, StringComparison.Ordinal);
    }

    private static int CountOccurrences(string text, string value)
    {
        var count = 0;
        var index = text.IndexOf(value, StringComparison.Ordinal);
        while (index >= 0)
        {
            ++count;
            index = text.IndexOf(value, index + value.Length, StringComparison.Ordinal);
        }

        return count;
    }
}
