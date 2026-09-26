using System;
using System.Collections.Generic;
using System.Reflection;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

public class PpcLifterHelperCoverageTests
{
    private static readonly Type LifterType = typeof(PpcLifter);

    private static T InvokePrivate<T>(string name, params object?[] args)
    {
        var method = LifterType.GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);
        return (T)method!.Invoke(null, args)!;
    }

    private static PpcRegisterOperand Gpr(int index) => new($"r{index}", index);

    [Fact]
    public void HelperMethods_ReadParseAndNormalizeRegisters()
    {
        var instruction = PpcInstruction.Synthetic(0x80000000, 0x12345678, "addi", new PpcOperand[] { Gpr(3), Gpr(4), new PpcImmediateOperand(1) });
        Assert.Equal(0x12345678u, InvokePrivate<uint>("ReadRawInstruction", instruction));

        // PpcInstruction stores the raw big-endian word itself, so an instruction
        // can no longer be missing its encoding; a zero word still reads back.
        var zeroWord = new PpcInstruction(
            0x80000000,
            0u,
            "nop",
            Array.Empty<PpcOperand>(),
            Array.Empty<uint>(),
            isReturn: false,
            isCall: false,
            isConditionalBranch: false);
        Assert.Equal(0u, InvokePrivate<uint>("ReadRawInstruction", zeroWord));

        Assert.Equal(0, InvokePrivate<int>("ParseCrFieldIndex", string.Empty));
        Assert.Equal(0, InvokePrivate<int>("ParseCrFieldIndex", "cr"));
        Assert.Equal(7, InvokePrivate<int>("ParseCrFieldIndex", "cr7_3"));
        Assert.Equal(3, InvokePrivate<int>("ParseCrFieldIndex", "cr3"));
        Assert.Equal(0, InvokePrivate<int>("ParseCrFieldIndex", "bad"));

        var zeroBase = InvokePrivate<IrValue>("BaseOrZero", "r0");
        Assert.Equal("const", zeroBase.Kind);
        Assert.Equal(0, zeroBase.Constant);

        var regBase = InvokePrivate<IrValue>("BaseOrZero", "r5");
        Assert.Equal("register", regBase.Kind);
        Assert.Equal("r5", regBase.RegisterName);

        Assert.Equal(31, InvokePrivate<int>("ParseRegisterNumber", "r31"));
        var badRegister = Assert.Throws<TargetInvocationException>(() => InvokePrivate<int>("ParseRegisterNumber", "ctr"));
        Assert.IsType<FormatException>(badRegister.InnerException);
    }

    [Fact]
    public void HelperMethods_EmitDFormAndPairedSingleMemoryAccesses()
    {
        var zeroBasedStore = InvokePrivate<IReadOnlyList<IrInstruction>>("DFormStore", "r0", 24, IrValue.Register("r7"), 4);
        Assert.Collection(
            zeroBasedStore,
            ins =>
            {
                var assign = Assert.IsType<IrAssign>(ins);
                Assert.Equal("r7_ea", assign.Destination);
                Assert.Equal(24, assign.Value.Constant);
            },
            ins =>
            {
                var store = Assert.IsType<IrStore>(ins);
                Assert.Equal("r7_ea", store.Address.Base);
                Assert.Equal("r7", store.Source.RegisterName);
                Assert.Equal(4, store.SizeBytes);
            });

        var normalStore = InvokePrivate<IReadOnlyList<IrInstruction>>("DFormStore", "r4", -8, IrValue.Register("r6"), 8);
        var storeIns = Assert.IsType<IrStore>(Assert.Single(normalStore));
        Assert.Equal("r4", storeIns.Address.Base);
        Assert.Equal(-8, storeIns.Address.Offset);
        Assert.Equal(8, storeIns.SizeBytes);

        var zeroBasedLoad = InvokePrivate<IReadOnlyList<IrInstruction>>("EmitPairedSingleLoad", "f1", "r0", 32, 1, 5, false, 0x80001000u);
        Assert.Collection(
            zeroBasedLoad,
            ins => Assert.Contains("psq_load", Assert.IsType<IrComment>(ins).Text, StringComparison.Ordinal),
            ins =>
            {
                var assign = Assert.IsType<IrAssign>(ins);
                Assert.Equal("f1_psq_ea", assign.Destination);
                Assert.Equal(32, assign.Value.Constant);
            },
            ins =>
            {
                var call = Assert.IsType<IrCall>(ins);
                Assert.Equal("PPC_PsqL", call.Target);
                Assert.Equal("f1_psq_ea", call.Arguments[0].RegisterName);
            });

        var updatingLoad = InvokePrivate<IReadOnlyList<IrInstruction>>("EmitPairedSingleLoad", "f2", "r9", 12, 0, 3, true, 0x80001004u);
        Assert.Equal("r9_psq_addr", Assert.IsType<IrBinary>(updatingLoad[1]).Destination);
        Assert.Equal("PPC_PsqL", Assert.IsType<IrCall>(updatingLoad[2]).Target);
        Assert.Equal("r9", Assert.IsType<IrAssign>(updatingLoad[3]).Destination);

        var zeroOffsetLoad = InvokePrivate<IReadOnlyList<IrInstruction>>("EmitPairedSingleLoad", "f3", "r8", 0, 0, 2, false, 0x80001008u);
        var directLoadCall = Assert.IsType<IrCall>(zeroOffsetLoad[1]);
        Assert.Equal("r8", directLoadCall.Arguments[0].RegisterName);

        var invalidLoad = Assert.Throws<TargetInvocationException>(() =>
            InvokePrivate<IReadOnlyList<IrInstruction>>("EmitPairedSingleLoad", "f4", "r0", 4, 0, 0, true, 0x8000100Cu));
        Assert.IsType<InvalidOperationException>(invalidLoad.InnerException);

        var zeroBasedStorePs = InvokePrivate<IReadOnlyList<IrInstruction>>("EmitPairedSingleStore", "f5", "r0", 48, 1, 6, false, 0x80001010u);
        Assert.Collection(
            zeroBasedStorePs,
            ins => Assert.Contains("psq_store", Assert.IsType<IrComment>(ins).Text, StringComparison.Ordinal),
            ins =>
            {
                var assign = Assert.IsType<IrAssign>(ins);
                Assert.Equal("f5_psq_ea", assign.Destination);
                Assert.Equal(48, assign.Value.Constant);
            },
            ins =>
            {
                var call = Assert.IsType<IrCall>(ins);
                Assert.Equal("PPC_PsqSt", call.Target);
                Assert.Equal("f5", call.Arguments[1].RegisterName);
            });

        var updatingStore = InvokePrivate<IReadOnlyList<IrInstruction>>("EmitPairedSingleStore", "f6", "r10", 16, 0, 7, true, 0x80001014u);
        Assert.Equal("r10_psq_ea", Assert.IsType<IrBinary>(updatingStore[1]).Destination);
        Assert.Equal("PPC_PsqSt", Assert.IsType<IrCall>(updatingStore[2]).Target);
        Assert.Equal("r10", Assert.IsType<IrAssign>(updatingStore[3]).Destination);

        var zeroOffsetStore = InvokePrivate<IReadOnlyList<IrInstruction>>("EmitPairedSingleStore", "f7", "r11", 0, 0, 1, false, 0x80001018u);
        var directStoreCall = Assert.IsType<IrCall>(zeroOffsetStore[1]);
        Assert.Equal("r11", directStoreCall.Arguments[0].RegisterName);

        var invalidStore = Assert.Throws<TargetInvocationException>(() =>
            InvokePrivate<IReadOnlyList<IrInstruction>>("EmitPairedSingleStore", "f8", "r0", 4, 0, 0, true, 0x8000101Cu));
        Assert.IsType<InvalidOperationException>(invalidStore.InnerException);
    }

    [Fact]
    public void HelperMethods_ParseCrBitsBuildTargetLabelsAndLiftUndefined()
    {
        Assert.Equal(0, InvokePrivate<int>("ParseCrBitIndex", string.Empty));
        Assert.Equal(30, InvokePrivate<int>("ParseCrBitIndex", "cr7eq"));
        Assert.Equal(31, InvokePrivate<int>("ParseCrBitIndex", "crb40"));
        Assert.Equal(13, InvokePrivate<int>("ParseCrBitIndex", "cr3gt"));
        Assert.Equal(29, InvokePrivate<int>("ParseCrBitIndex", "29"));
        Assert.Equal(0, InvokePrivate<int>("ParseCrBitIndex", "bogus"));

        Assert.Equal(0, InvokePrivate<int>("ParseCrFieldName", string.Empty));
        Assert.Equal(7, InvokePrivate<int>("ParseCrFieldName", "crf7"));
        Assert.Equal(3, InvokePrivate<int>("ParseCrFieldName", "cr3"));
        Assert.Equal(7, InvokePrivate<int>("ParseCrFieldName", "cr99"));
        Assert.Equal(0, InvokePrivate<int>("ParseCrFieldName", "bogus"));

        var noTarget = PpcInstruction.Synthetic(0x80002000, 0x48000000, "b", Array.Empty<PpcOperand>());
        var noTargetEx = Assert.Throws<TargetInvocationException>(() =>
            InvokePrivate<string>("TargetLabel", noTarget, new HashSet<uint>(), true));
        Assert.IsType<InvalidOperationException>(noTargetEx.InnerException);

        var branch = PpcInstruction.Synthetic(0x80002010, 0x48000000, "b", Array.Empty<PpcOperand>(), branchTargets: new[] { 0x80003000u });
        Assert.Equal("0x80002014", InvokePrivate<string>("TargetLabel", branch, new HashSet<uint>(), true));
        Assert.Equal("0x80003000", InvokePrivate<string>("TargetLabel", branch, new HashSet<uint>(), false));
        Assert.Equal("0x80003000", InvokePrivate<string>("TargetLabel", branch, new HashSet<uint> { 0x80003000u }, true));

        var withOperands = PpcInstruction.Synthetic(0x80003000, 0x38630001, "addi", new PpcOperand[] { Gpr(3), Gpr(3), new PpcImmediateOperand(1) });
        var undefinedWithOperands = Assert.IsType<IrUndefined>(Assert.Single(InvokePrivate<IReadOnlyList<IrInstruction>>("LiftUndefined", withOperands, "unsupported")));
        Assert.Equal("addi r3, r3, 1", undefinedWithOperands.Disassembly);
        Assert.Equal("unsupported", undefinedWithOperands.Reason);

        var withoutOperands = PpcInstruction.Synthetic(0x80003004, 0x60000000, "nop", Array.Empty<PpcOperand>());
        var undefinedWithoutOperands = Assert.IsType<IrUndefined>(Assert.Single(InvokePrivate<IReadOnlyList<IrInstruction>>("LiftUndefined", withoutOperands, "no-op")));
        Assert.Equal("nop", undefinedWithoutOperands.Disassembly);
    }

    [Fact]
    public void HelperMethods_LiftCrLogicalAndIndexedPairedSingles()
    {
        var creqv = Assert.IsType<IrCall>(Assert.Single(InvokePrivate<IReadOnlyList<IrInstruction>>("LiftCrLogical", "creqv", "crb1", "crb2", "crb3")));
        Assert.Equal("PPC_CrLogical", creqv.Target);
        Assert.Equal(new long?[] { 5, 1, 2, 3 }, new[] { creqv.Arguments[0].Constant, creqv.Arguments[1].Constant, creqv.Arguments[2].Constant, creqv.Arguments[3].Constant });

        var unknownLogical = Assert.IsType<IrCall>(Assert.Single(InvokePrivate<IReadOnlyList<IrInstruction>>("LiftCrLogical", "unknown", "cr0lt", "cr0gt", "cr0eq")));
        Assert.Equal(0, unknownLogical.Arguments[0].Constant);

        var indexedLoad = PpcInstruction.Synthetic(0x80004000, 0, "psq_lx", Array.Empty<PpcOperand>());
        var indexedLoadOps = InvokePrivate<IReadOnlyList<IrInstruction>>("LiftPairedSingleIndexedLoad", indexedLoad, (3u << 21) | (4u << 16) | (5u << 11) | (1u << 10) | (2u << 7), false);
        Assert.Equal("addr_psqx_80004000_loc", Assert.IsType<IrBinary>(indexedLoadOps[0]).Destination);
        Assert.Equal("PPC_PsqL", Assert.IsType<IrCall>(indexedLoadOps[1]).Target);

        var indexedLoadZeroBase = InvokePrivate<IReadOnlyList<IrInstruction>>("LiftPairedSingleIndexedLoad", indexedLoad, (3u << 21) | (0u << 16) | (5u << 11), false);
        Assert.Equal("addr_psqx_80004000_loc", Assert.IsType<IrAssign>(indexedLoadZeroBase[0]).Destination);

        var indexedLoadUpdate = InvokePrivate<IReadOnlyList<IrInstruction>>("LiftPairedSingleIndexedLoad", indexedLoad, (3u << 21) | (4u << 16) | (5u << 11), true);
        Assert.Equal("r4", Assert.IsType<IrAssign>(indexedLoadUpdate[2]).Destination);

        var badIndexedLoad = Assert.Throws<TargetInvocationException>(() =>
            InvokePrivate<IReadOnlyList<IrInstruction>>("LiftPairedSingleIndexedLoad", indexedLoad, (3u << 21) | (0u << 16) | (5u << 11), true));
        Assert.IsType<InvalidOperationException>(badIndexedLoad.InnerException);

        var indexedStore = PpcInstruction.Synthetic(0x80004004, 0, "psq_stx", Array.Empty<PpcOperand>());
        var indexedStoreOps = InvokePrivate<IReadOnlyList<IrInstruction>>("LiftPairedSingleIndexedStore", indexedStore, (6u << 21) | (7u << 16) | (8u << 11) | (1u << 10) | (3u << 7), false);
        Assert.Equal("addr_psqx_80004004_loc", Assert.IsType<IrBinary>(indexedStoreOps[0]).Destination);
        Assert.Equal("PPC_PsqSt", Assert.IsType<IrCall>(indexedStoreOps[1]).Target);

        var indexedStoreZeroBase = InvokePrivate<IReadOnlyList<IrInstruction>>("LiftPairedSingleIndexedStore", indexedStore, (6u << 21) | (0u << 16) | (8u << 11), false);
        Assert.Equal("addr_psqx_80004004_loc", Assert.IsType<IrAssign>(indexedStoreZeroBase[0]).Destination);

        var indexedStoreUpdate = InvokePrivate<IReadOnlyList<IrInstruction>>("LiftPairedSingleIndexedStore", indexedStore, (6u << 21) | (7u << 16) | (8u << 11), true);
        Assert.Equal("r7", Assert.IsType<IrAssign>(indexedStoreUpdate[2]).Destination);

        var badIndexedStore = Assert.Throws<TargetInvocationException>(() =>
            InvokePrivate<IReadOnlyList<IrInstruction>>("LiftPairedSingleIndexedStore", indexedStore, (6u << 21) | (0u << 16) | (8u << 11), true));
        Assert.IsType<InvalidOperationException>(badIndexedStore.InnerException);
    }
}
