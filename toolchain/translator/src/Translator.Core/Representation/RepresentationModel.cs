using System;
using System.Collections.Generic;
using Translator.Core;

namespace Translator.Core.Representation;

public sealed record ValueRepresentation(string Name, int Size, bool IsFloat = false)
{
    public static readonly ValueRepresentation Void = new("void", 0);
    public static readonly ValueRepresentation Int32 = new("int32", 4);
    public static readonly ValueRepresentation UInt32 = new("uint32", 4);
    public static readonly ValueRepresentation Int64 = new("int64", 8);
    public static readonly ValueRepresentation UInt64 = new("uint64", 8);
    public static readonly ValueRepresentation Float32 = new("float", 4, true);
    public static readonly ValueRepresentation Float64 = new("double", 8, true);
    public static readonly ValueRepresentation Unknown = new("unknown", 0);
}

public sealed class RepresentationEnvironment
{
    private static readonly IReadOnlyDictionary<string, ValueRepresentation> DefaultRegisterRepresentations =
        BuildDefaultRegisterRepresentations();

    private readonly Dictionary<string, ValueRepresentation> _representations =
        new(StringComparer.OrdinalIgnoreCase);

    public RepresentationEnvironment()
    {
        foreach (var (name, representation) in DefaultRegisterRepresentations)
        {
            _representations[name] = representation;
        }
    }

    public RepresentationEnvironment(IReadOnlyDictionary<string, ValueRepresentation> seeded)
        : this()
    {
        foreach (var (name, representation) in seeded)
        {
            _representations[Base(name)] = representation;
        }
    }

    public ValueRepresentation Get(string variable)
    {
        var key = Base(variable);
        return _representations.TryGetValue(key, out var representation)
            ? representation
            : ValueRepresentation.Unknown;
    }

    public bool Unify(string variable, ValueRepresentation representation)
    {
        if (string.IsNullOrWhiteSpace(variable))
        {
            return false;
        }

        var key = Base(variable);
        var existing = Get(key);
        var merged = Merge(existing, representation);
        if (existing == merged)
        {
            return false;
        }

        _representations[key] = merged;
        return true;
    }

    private static ValueRepresentation Merge(ValueRepresentation left, ValueRepresentation right)
    {
        if (left == ValueRepresentation.Unknown) return right;
        if (right == ValueRepresentation.Unknown) return left;
        if (left.IsFloat != right.IsFloat) return left.Size >= right.Size ? left : right;
        return left.Size >= right.Size ? left : right;
    }

    private static IReadOnlyDictionary<string, ValueRepresentation> BuildDefaultRegisterRepresentations()
    {
        var defaults = new Dictionary<string, ValueRepresentation>(StringComparer.OrdinalIgnoreCase);
        for (var index = 0; index < 32; index++) defaults[$"r{index}"] = ValueRepresentation.UInt32;
        for (var index = 0; index < 32; index++) defaults[$"f{index}"] = ValueRepresentation.Float64;
        for (var index = 0; index < 8; index++) defaults[$"gqr{index}"] = ValueRepresentation.UInt32;

        foreach (var register in new[] { "hid0", "hid1", "hid2", "lr", "ctr", "cr", "xer", "srr0", "srr1", "msr" })
        {
            defaults[register] = ValueRepresentation.UInt32;
        }

        return defaults;
    }

    private static string Base(string name) => RegisterNameUtils.StripNumericSuffix(name);
}

