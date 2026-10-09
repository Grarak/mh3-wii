using System;
using System.Collections.Frozen;
using System.Collections.Generic;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

public sealed record GuestHelperEffect(
    uint GprReadMask = 0,
    uint GprWriteMask = 0,
    uint FprReadMask = 0,
    uint FprWriteMask = 0,
    byte CrReadMask = 0,
    byte CrWriteMask = 0,
    bool ReadsXer = false,
    bool WritesXer = false,
    bool ReadsCtr = false,
    bool WritesCtr = false,
    bool ReadsLr = false,
    bool WritesLr = false,
    GuestCallBoundaryFlags BoundaryFlags = GuestCallBoundaryFlags.None);

/// <summary>
/// Architectural effects hidden behind non-guest helper calls, reached implicitly through CpuContext
/// (explicit IrCall args/destinations are handled by the ordinary IR analyzer). Unknown helpers default
/// to complete-context boundaries so a new runtime helper can't silently invalidate resident ownership.
/// </summary>
public static class GuestHelperEffectCatalog
{
    // The effects below are constants, so one shared instance per distinct
    // effect is handed out instead of allocating a record per call site.
    // GuestHelperEffect is an immutable record: every member is init-only.
    private static readonly GuestHelperEffect NoEffect = new();
    private static readonly GuestHelperEffect CarryUpdate = new(ReadsXer: true, WritesXer: true);
    private static readonly GuestHelperEffect CarryRead = new(ReadsXer: true);
    private static readonly GuestHelperEffect UnboundedStringLoad = new(GprWriteMask: uint.MaxValue, ReadsXer: true);
    private static readonly GuestHelperEffect UnboundedStringStore = new(GprReadMask: uint.MaxValue, ReadsXer: true);
    private static readonly GuestHelperEffect ConditionalStore =
        new(CrReadMask: byte.MaxValue, CrWriteMask: 1, ReadsXer: true);
    private static readonly GuestHelperEffect CompleteBoundary = CreateComplete(GuestCallBoundaryFlags.None);
    private static readonly GuestHelperEffect SystemCallBoundary = CreateComplete(
        GuestCallBoundaryFlags.CanSuspend | GuestCallBoundaryFlags.CanSwitchThreads |
        GuestCallBoundaryFlags.InvokesGuestCode);

    /// <summary>Helpers whose effect is derived from a constant argument.</summary>
    private enum ArgumentDependentHelper
    {
        LoadStringImmediate,
        StoreStringImmediate,
        MoveConditionFromXer,
        MoveConditionField,
        MoveConditionFromFpscr,
        SetConditionBit,
        ConditionLogical,
        CompareIntoConditionField,
        ReadSpr,
        WriteSpr
    }

    private static readonly FrozenDictionary<string, ArgumentDependentHelper> ArgumentDependentHelpers =
        new Dictionary<string, ArgumentDependentHelper>(StringComparer.OrdinalIgnoreCase)
        {
            ["PPC_Lswi"] = ArgumentDependentHelper.LoadStringImmediate,
            ["PPC_Stswi"] = ArgumentDependentHelper.StoreStringImmediate,
            ["PPC_Mcrxr"] = ArgumentDependentHelper.MoveConditionFromXer,
            ["PPC_Mcrf"] = ArgumentDependentHelper.MoveConditionField,
            ["PPC_Mcrfs"] = ArgumentDependentHelper.MoveConditionFromFpscr,
            ["PPC_CrSetBit"] = ArgumentDependentHelper.SetConditionBit,
            ["PPC_CrLogical"] = ArgumentDependentHelper.ConditionLogical,
            ["PPC_Fcmp"] = ArgumentDependentHelper.CompareIntoConditionField,
            ["PPC_PsCmpo0"] = ArgumentDependentHelper.CompareIntoConditionField,
            ["PPC_PsCmpo1"] = ArgumentDependentHelper.CompareIntoConditionField,
            ["PPC_PsCmpu0"] = ArgumentDependentHelper.CompareIntoConditionField,
            ["PPC_PsCmpu1"] = ArgumentDependentHelper.CompareIntoConditionField,
            ["PPC_ReadSpr"] = ArgumentDependentHelper.ReadSpr,
            ["PPC_WriteSpr"] = ArgumentDependentHelper.WriteSpr
        }.ToFrozenDictionary(StringComparer.OrdinalIgnoreCase);

    private static readonly string[] PureHelpers =
    {
        "memset_zero_32", "PPC_Cntlzw", "PPC_Eciwx", "PPC_Ecowx", "PPC_Lwarx",
        "PPC_LoadHalfwordByteReverse", "PPC_LoadWordByteReverse",
        "PPC_StoreHalfwordByteReverse", "PPC_StoreWordByteReverse", "PPC_Stfiwx",
        "PPC_Fadds", "PPC_Fdivs", "PPC_Fmadd", "PPC_Fmadds", "PPC_Fmsub", "PPC_Fmsubs",
        "PPC_Fmuls", "PPC_Fnmadd", "PPC_Fnmadds", "PPC_Fnmsub", "PPC_Fnmsubs",
        "PPC_Fres", "PPC_Frsqrte", "PPC_Fsel", "PPC_Fsqrt", "PPC_Fsubs",
        "PPC_PsAbs", "PPC_PsAdd", "PPC_PsDiv", "PPC_PsMadd", "PPC_PsMadds0",
        "PPC_PsMadds1", "PPC_PsMerge00", "PPC_PsMerge01", "PPC_PsMerge10",
        "PPC_PsMerge11", "PPC_PsMsub", "PPC_PsMul", "PPC_PsMuls0", "PPC_PsMuls1",
        "PPC_PsNabs", "PPC_PsNeg", "PPC_PsNmadd", "PPC_PsNmsub", "PPC_PsRes",
        "PPC_PsRsqrte", "PPC_PsSel", "PPC_PsSub", "PPC_PsSum0", "PPC_PsSum1",
        "PPC_Mftb", "PPC_Mftbu", "PPC_TrapWord"
    };

    private static readonly string[] XerReadWriteHelpers =
    {
        "PPC_UpdateCarryAdd", "PPC_UpdateCarrySub", "PPC_UpdateCarryShiftRight",
        "PPC_Addco", "PPC_Addeo", "PPC_Addmeo", "PPC_Addo", "PPC_Addzeo",
        "PPC_Subfco", "PPC_Subfeo", "PPC_Subfmeo", "PPC_Subfo", "PPC_Subfzeo",
        "PPC_Divwo", "PPC_Divwuo", "PPC_Mullwo", "PPC_Nego"
    };

    /// <summary>
    /// Helpers that implicitly touch FPSCR or GQR state, which stays authoritative in CpuContext
    /// so no GPR sync is needed; kept as explicit entries so they don't default to unknown
    /// full-context fences.
    /// </summary>
    private static readonly string[] FpscrAndGqrHelpers =
    {
        "PPC_Mffs", "PPC_Mtfsb0", "PPC_Mtfsb1", "PPC_Mtfsf", "PPC_Mtfsfi", "PPC_PsqL", "PPC_PsqSt"
    };

    /// <summary>
    /// Helpers whose effect doesn't depend on arguments, as one hash lookup instead of the linear
    /// case-insensitive chain this used to walk per call site. Declared after the name tables above
    /// since static field initializers run in textual order and this one reads them.
    /// </summary>
    private static readonly FrozenDictionary<string, GuestHelperEffect> ConstantEffectHelpers =
        BuildConstantEffectHelpers();

    private static FrozenDictionary<string, GuestHelperEffect> BuildConstantEffectHelpers()
    {
        var entries = new Dictionary<string, GuestHelperEffect>(StringComparer.OrdinalIgnoreCase);
        foreach (var helper in PureHelpers) entries.Add(helper, NoEffect);
        foreach (var helper in XerReadWriteHelpers) entries.Add(helper, CarryUpdate);
        foreach (var helper in FpscrAndGqrHelpers) entries.Add(helper, NoEffect);
        entries.Add("PPC_GetCarry", CarryRead);
        entries.Add("OSSystemCall", SystemCallBoundary);
        entries.Add("PPC_Lswx", UnboundedStringLoad);
        entries.Add("PPC_Stswx", UnboundedStringStore);
        entries.Add("PPC_Stwcx", ConditionalStore);
        return entries.ToFrozenDictionary(StringComparer.OrdinalIgnoreCase);
    }

    public static GuestHelperEffect Analyze(IrCall call)
    {
        // The prefixed families are matched first, exactly as before: a name is
        // only looked up once it is known not to be a known-GQR paired form.
        if (call.Target.StartsWith("PPC_PsqLKnown_", StringComparison.OrdinalIgnoreCase) ||
            call.Target.StartsWith("PPC_PsqLKnownGuarded_", StringComparison.OrdinalIgnoreCase) ||
            call.Target.StartsWith("PPC_PsqStKnown_", StringComparison.OrdinalIgnoreCase) ||
            call.Target.StartsWith("PPC_PsqStKnownGuarded_", StringComparison.OrdinalIgnoreCase))
            return NoEffect;

        if (ConstantEffectHelpers.TryGetValue(call.Target, out var constantEffect)) return constantEffect;

        if (ArgumentDependentHelpers.TryGetValue(call.Target, out var helper))
        {
            return helper switch
            {
                ArgumentDependentHelper.LoadStringImmediate =>
                    new GuestHelperEffect(GprWriteMask: StringRegisterMask(call, countArgument: 2)),
                ArgumentDependentHelper.StoreStringImmediate =>
                    new GuestHelperEffect(GprReadMask: StringRegisterMask(call, countArgument: 2)),
                ArgumentDependentHelper.MoveConditionFromXer =>
                    new GuestHelperEffect(
                        CrReadMask: byte.MaxValue, CrWriteMask: FieldMask(call, 0),
                        ReadsXer: true, WritesXer: true),
                ArgumentDependentHelper.MoveConditionField =>
                    new GuestHelperEffect(CrReadMask: byte.MaxValue, CrWriteMask: FieldMask(call, 0)),
                ArgumentDependentHelper.MoveConditionFromFpscr =>
                    new GuestHelperEffect(CrReadMask: byte.MaxValue, CrWriteMask: FieldMask(call, 0)),
                ArgumentDependentHelper.SetConditionBit =>
                    new GuestHelperEffect(CrReadMask: byte.MaxValue, CrWriteMask: BitFieldMask(call, 0)),
                ArgumentDependentHelper.ConditionLogical =>
                    new GuestHelperEffect(
                        // Updates one field but preserves the other seven via a packed-CR
                        // read/modify/write, so resident CR ownership needs the full packed value first.
                        CrReadMask: byte.MaxValue,
                        CrWriteMask: BitFieldMask(call, 1)),
                ArgumentDependentHelper.CompareIntoConditionField =>
                    new GuestHelperEffect(CrReadMask: byte.MaxValue, CrWriteMask: FieldMask(call, 0)),
                ArgumentDependentHelper.ReadSpr => SprEffect(call, write: false),
                ArgumentDependentHelper.WriteSpr => SprEffect(call, write: true),
                _ => Complete()
            };
        }

        return Complete();
    }

    private static GuestHelperEffect SprEffect(IrCall call, bool write)
    {
        if (call.Arguments.Count == 0 || call.Arguments[0].Constant is not long spr)
            return Complete();
        return spr switch
        {
            1 => new GuestHelperEffect(ReadsXer: !write, WritesXer: write),
            8 => new GuestHelperEffect(ReadsLr: !write, WritesLr: write),
            9 => new GuestHelperEffect(ReadsCtr: !write, WritesCtr: write),
            _ => NoEffect
        };
    }

    private static uint StringRegisterMask(IrCall call, int countArgument)
    {
        if (call.Arguments.Count <= countArgument || call.Arguments[0].Constant is not long startValue ||
            call.Arguments[countArgument].Constant is not long countValue)
            return uint.MaxValue;
        var start = (int)(startValue & 31);
        var count = (int)(countValue == 0 ? 32 : countValue);
        var registers = Math.Min(32, (count + 3) / 4);
        uint mask = 0;
        for (var index = 0; index < registers; ++index) mask |= 1u << ((start + index) & 31);
        return mask;
    }

    private static byte FieldMask(IrCall call, int argument) =>
        call.Arguments.Count > argument && call.Arguments[argument].Constant is long field
            ? (byte)(1 << ((int)field & 7))
            : byte.MaxValue;

    private static byte BitFieldMask(IrCall call, int argument) =>
        call.Arguments.Count > argument && call.Arguments[argument].Constant is long bit
            ? (byte)(1 << (((int)bit & 31) / 4))
            : byte.MaxValue;

    private static GuestHelperEffect Complete(GuestCallBoundaryFlags extra = GuestCallBoundaryFlags.None) =>
        extra == GuestCallBoundaryFlags.None ? CompleteBoundary : CreateComplete(extra);

    private static GuestHelperEffect CreateComplete(GuestCallBoundaryFlags extra) =>
        new(uint.MaxValue, uint.MaxValue, uint.MaxValue, uint.MaxValue, byte.MaxValue, byte.MaxValue,
            true, true, true, true, true, true, extra | GuestCallBoundaryFlags.RequiresCompleteContext);
}
