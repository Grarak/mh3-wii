using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Ir;

namespace Translator.Core.Representation;

public sealed class RepresentationClassifier
{
    public RepresentationEnvironment Classify(
        IrFunction function,
        IReadOnlyDictionary<string, ValueRepresentation>? seeds = null)
    {
        var environment = seeds is null
            ? new RepresentationEnvironment()
            : new RepresentationEnvironment(seeds);

        var changed = true;
        var iterations = 0;
        while (changed && iterations++ < 20)
        {
            changed = false;
            foreach (var instruction in function.Blocks.SelectMany(static block => block.Instructions))
            {
                changed |= Propagate(instruction, environment);
            }
        }

        return environment;
    }

    private static bool Propagate(IrInstruction instruction, RepresentationEnvironment environment)
    {
        switch (instruction)
        {
            case IrAssign assign:
                return environment.Unify(assign.Destination, RepresentationOf(assign.Value, environment));

            case IrBinary binary:
            {
                var left = RepresentationOf(binary.Left, environment);
                var right = RepresentationOf(binary.Right, environment);
                var result = Merge(left, right);
                if (IsCrField(binary.Destination)) result = ValueRepresentation.UInt32;
                var changed = environment.Unify(binary.Destination, result);
                if (binary.Left.RegisterName is { } leftRegister)
                    changed |= environment.Unify(leftRegister, left);
                if (binary.Right.RegisterName is { } rightRegister)
                    changed |= environment.Unify(rightRegister, right);
                return changed;
            }

            case IrLoad load:
                return environment.Unify(load.Destination, RepresentationFromSize(load.SizeBytes));

            case IrStore store:
            {
                var changed = environment.Unify(store.Address.Base, ValueRepresentation.UInt32);
                if (store.Source.RegisterName is { } source)
                    changed |= environment.Unify(source, RepresentationOf(store.Source, environment));
                return changed;
            }

            case IrCall call when !string.IsNullOrWhiteSpace(call.Destination):
                return environment.Unify(
                    call.Destination,
                    IsFloatRegister(call.Destination) ? ValueRepresentation.Float64 : ValueRepresentation.UInt32);

            case IrIndirectCall call when !string.IsNullOrWhiteSpace(call.Destination):
                return environment.Unify(
                    call.Destination,
                    IsFloatRegister(call.Destination) ? ValueRepresentation.Float64 : ValueRepresentation.UInt32);

            case IrPhi phi:
                return environment.Unify(
                    phi.Destination,
                    phi.Sources.Values
                        .Select(environment.Get)
                        .Aggregate(ValueRepresentation.Unknown, Merge));

            case IrJumpTable table:
                return environment.Unify(table.Selector, ValueRepresentation.UInt32);

            default:
                return false;
        }
    }

    private static ValueRepresentation RepresentationOf(
        IrValue value,
        RepresentationEnvironment environment) =>
        value.RegisterName is { } register
            ? environment.Get(register)
            : value.Kind == "const"
                ? ValueRepresentation.Int32
                : ValueRepresentation.Unknown;

    private static ValueRepresentation RepresentationFromSize(int size) =>
        size == 8 ? ValueRepresentation.UInt64 :
        size is 1 or 2 or 4 ? ValueRepresentation.UInt32 :
        ValueRepresentation.Unknown;

    private static ValueRepresentation Merge(ValueRepresentation left, ValueRepresentation right)
    {
        if (left == ValueRepresentation.Unknown) return right;
        if (right == ValueRepresentation.Unknown) return left;
        return left.Size >= right.Size ? left : right;
    }

    private static bool IsFloatRegister(string name)
    {
        var baseName = RegisterNameUtils.StripNumericSuffix(name);
        return baseName.Length > 1 &&
               (baseName[0] == 'f' || baseName[0] == 'F') &&
               int.TryParse(baseName.AsSpan(1), out var index) &&
               index is >= 0 and < 32;
    }

    private static bool IsCrField(string name)
    {
        var baseName = RegisterNameUtils.StripNumericSuffix(name);
        return baseName.Length == 3 &&
               baseName.StartsWith("cr", StringComparison.OrdinalIgnoreCase) &&
               baseName[2] is >= '0' and <= '7';
    }
}

