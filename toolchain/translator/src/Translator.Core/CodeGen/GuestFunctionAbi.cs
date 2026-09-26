using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core;

namespace Translator.Core.CodeGen;

public sealed class GuestFunctionAbi
{
    private readonly HashSet<string> _scalarFloatArgumentRegisters;
    private readonly HashSet<string> _argumentRegisters;

    public GuestFunctionAbi(
        IEnumerable<string>? scalarFloatArgumentRegisters = null,
        bool returnsPairedScalarFloat = false,
        bool writesFloatReturnRegister = false,
        IEnumerable<string>? argumentRegisters = null,
        bool preservesVolatileContext = false,
        bool writesGprReturnRegister = true)
    {
        _scalarFloatArgumentRegisters = (scalarFloatArgumentRegisters ?? Enumerable.Empty<string>())
            .Select(RegisterNameUtils.StripNumericSuffix)
            .Where(static r => !string.IsNullOrWhiteSpace(r))
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        _argumentRegisters = (argumentRegisters ?? Enumerable.Empty<string>())
            .Select(RegisterNameUtils.StripNumericSuffix)
            .Where(static r => !string.IsNullOrWhiteSpace(r))
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        ReturnsPairedScalarFloat = returnsPairedScalarFloat;
        WritesFloatReturnRegister = writesFloatReturnRegister || returnsPairedScalarFloat;
        PreservesVolatileContext = preservesVolatileContext;
        WritesGprReturnRegister = writesGprReturnRegister;
    }

    public static GuestFunctionAbi Empty { get; } = new();

    public IReadOnlySet<string> ScalarFloatArgumentRegisters => _scalarFloatArgumentRegisters;

    public IReadOnlySet<string> ArgumentRegisters => _argumentRegisters;

    public bool HasKnownArgumentRegisters => _argumentRegisters.Count != 0;

    public bool ReturnsPairedScalarFloat { get; }

    public bool WritesFloatReturnRegister { get; }

    public bool WritesGprReturnRegister { get; }

    public bool PreservesVolatileContext { get; }

    public bool HasScalarFloatArgument(string registerName)
    {
        var baseName = RegisterNameUtils.StripNumericSuffix(registerName);
        return _scalarFloatArgumentRegisters.Contains(baseName);
    }

    public bool HasArgumentRegister(string registerName)
    {
        var baseName = RegisterNameUtils.StripNumericSuffix(registerName);
        return _argumentRegisters.Contains(baseName);
    }
}

public interface IGuestFunctionAbiProvider
{
    bool TryGetGuestFunctionAbi(string target, out GuestFunctionAbi abi);
}

public sealed class CompositeGuestFunctionAbiProvider : IGuestFunctionAbiProvider
{
    private readonly IReadOnlyList<IGuestFunctionAbiProvider> _providers;

    public CompositeGuestFunctionAbiProvider(params IGuestFunctionAbiProvider[] providers)
    {
        _providers = providers.Where(static p => p is not null).ToArray();
    }

    public bool TryGetGuestFunctionAbi(string target, out GuestFunctionAbi abi)
    {
        foreach (var provider in _providers)
        {
            if (provider.TryGetGuestFunctionAbi(target, out abi))
            {
                return true;
            }
        }

        abi = GuestFunctionAbi.Empty;
        return false;
    }
}

public sealed class EmptyGuestFunctionAbiProvider : IGuestFunctionAbiProvider
{
    public static EmptyGuestFunctionAbiProvider Instance { get; } = new();

    private EmptyGuestFunctionAbiProvider()
    {
    }

    public bool TryGetGuestFunctionAbi(string target, out GuestFunctionAbi abi)
    {
        abi = GuestFunctionAbi.Empty;
        return false;
    }
}
