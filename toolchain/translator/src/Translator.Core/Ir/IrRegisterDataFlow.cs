using System;
using System.Collections.Generic;

namespace Translator.Core.Ir;

internal static class IrRegisterDataFlow
{
    public static IEnumerable<string> Uses(IrInstruction instruction)
    {
        switch (instruction)
        {
            case IrAssign assign when TryRegister(assign.Value, out var source):
                yield return source;
                break;
            case IrBinary binary:
                if (TryRegister(binary.Left, out var left)) yield return left;
                if (TryRegister(binary.Right, out var right)) yield return right;
                break;
            case IrLoad load:
                yield return load.Address.Base;
                break;
            case IrStore store:
                yield return store.Address.Base;
                if (TryRegister(store.Source, out var stored)) yield return stored;
                break;
            case IrResolveGuestMemoryRange resolve when TryRegister(resolve.Base, out var rangeBase):
                yield return rangeBase;
                break;
            case IrResolvedLoad load:
                yield return load.OriginalAddress.Base;
                break;
            case IrResolvedStore store:
                yield return store.OriginalAddress.Base;
                if (TryRegister(store.Source, out var resolvedStored)) yield return resolvedStored;
                break;
            case IrResolvedPsqLoad load:
                if (TryRegister(load.OriginalAddress, out var psqLoadAddress)) yield return psqLoadAddress;
                if (load.KnownGqr is null || load.GuardKnownGqr) yield return $"gqr{load.I}";
                break;
            case IrResolvedPsqStore store:
                if (TryRegister(store.OriginalAddress, out var psqStoreAddress)) yield return psqStoreAddress;
                if (TryRegister(store.Source, out var psqStored)) yield return psqStored;
                if (store.KnownGqr is null || store.GuardKnownGqr) yield return $"gqr{store.I}";
                break;
            case IrResolvedLoadPair pair:
                yield return pair.FirstOriginalAddress.Base;
                yield return pair.SecondOriginalAddress.Base;
                break;
            case IrResolvedStorePair pair:
                yield return pair.FirstOriginalAddress.Base;
                yield return pair.SecondOriginalAddress.Base;
                if (TryRegister(pair.FirstSource, out var firstStored)) yield return firstStored;
                if (TryRegister(pair.SecondSource, out var secondStored)) yield return secondStored;
                break;
            case IrCall call:
                foreach (var argument in call.Arguments)
                    if (TryRegister(argument, out var name)) yield return name;
                break;
            case IrIndirectCall call:
                if (TryRegister(call.Target, out var target)) yield return target;
                foreach (var argument in call.Arguments)
                    if (TryRegister(argument, out var name)) yield return name;
                break;
            case IrIndirectJump jump when TryRegister(jump.Target, out var jumpTarget):
                yield return jumpTarget;
                break;
            case IrReturn ret when ret.Value is not null && TryRegister(ret.Value, out var returned):
                yield return returned;
                break;
            case IrBranch branch when IsRegisterName(branch.ConditionRegister):
                yield return branch.ConditionRegister;
                break;
            case IrPhi phi:
                foreach (var source in phi.Sources.Values) yield return source;
                break;
            case IrSetCrField setCr:
                if (TryRegister(setCr.Left, out var compareLeft)) yield return compareLeft;
                if (TryRegister(setCr.Right, out var compareRight)) yield return compareRight;
                yield return "xer";
                break;
            case IrJumpTable table:
                yield return table.Selector;
                break;
        }
    }

    public static IEnumerable<string> Definitions(IrInstruction instruction)
    {
        switch (instruction)
        {
            case IrAssign assign:
                yield return assign.Destination;
                break;
            case IrBinary binary:
                yield return binary.Destination;
                break;
            case IrLoad load:
                yield return load.Destination;
                break;
            case IrResolveGuestMemoryRange resolve:
                yield return resolve.Destination;
                break;
            case IrResolvedLoad load:
                yield return load.Destination;
                break;
            case IrResolvedPsqLoad load:
                yield return load.Destination;
                break;
            case IrResolvedLoadPair pair:
                yield return pair.FirstDestination;
                yield return pair.SecondDestination;
                break;
            case IrCall call when !string.IsNullOrWhiteSpace(call.Destination):
                yield return call.Destination;
                break;
            case IrIndirectCall call when !string.IsNullOrWhiteSpace(call.Destination):
                yield return call.Destination;
                break;
            case IrPhi phi:
                yield return phi.Destination;
                break;
            case IrSetCrField setCr:
                yield return $"cr{setCr.FieldIndex}";
                break;
        }
    }

    public static bool IsRegisterName(string name)
    {
        if (string.IsNullOrWhiteSpace(name)) return false;

        var baseName = BaseName(name);
        if (baseName.Length is >= 2 and <= 3 &&
            (baseName[0] == 'r' || baseName[0] == 'f') &&
            int.TryParse(baseName.AsSpan(1), out var register) && register is >= 0 and < 32)
            return true;

        if (baseName.Length == 3 &&
            baseName.StartsWith("cr", StringComparison.OrdinalIgnoreCase) &&
            char.IsDigit(baseName[2]) && (baseName[2] - '0') is >= 0 and <= 7)
            return true;

        if (baseName.Length is >= 4 and <= 5 &&
            baseName.StartsWith("crb", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(baseName.AsSpan(3), out var crBit) && crBit is >= 0 and < 32)
            return true;

        if (baseName.Length == 4 && baseName.StartsWith("gqr", StringComparison.OrdinalIgnoreCase) &&
            baseName[3] is >= '0' and <= '7')
            return true;

        return baseName.Equals("cr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("lr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("ctr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("xer", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("fpscr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("msr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("dar", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("dsisr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("iccr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("tbr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("tbl", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("tbu", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("hid0", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("hid1", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("hid2", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("srr0", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("srr1", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Returns the architectural/SSA base spelling for a value name. Only a numeric suffix is an SSA
    /// version, so lifter temporaries like <c>r3_addc_left</c> stay temporaries instead of being
    /// mistaken for architectural <c>r3</c>.
    /// </summary>
    public static string BaseName(string name) =>
        RegisterNameUtils.StripNumericSuffix(name);

    private static bool TryRegister(IrValue value, out string register)
    {
        register = value.RegisterName ?? string.Empty;
        return value.Kind == "register" && register.Length > 0;
    }
}
