using System.Buffers.Binary;
using Translator.Core.CodeGen;
using Translator.Core.Loading;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public sealed class InferredGuestFunctionAbiProviderDeterminismTests
{
    private const uint FunctionA = MemoryLayout.RamBase;
    private const uint FunctionB = MemoryLayout.RamBase + 0x10;

    [Fact]
    public void MutuallyRecursiveFunctions_AreIndependentOfRootRequestOrder()
    {
        var forward = CreateProvider();
        forward.Prewarm(new[] { FunctionA, FunctionB });
        var forwardA = GetSnapshot(forward, FunctionA);
        var forwardB = GetSnapshot(forward, FunctionB);

        var reverse = CreateProvider();
        reverse.Prewarm(new[] { FunctionB, FunctionA });
        var reverseB = GetSnapshot(reverse, FunctionB);
        var reverseA = GetSnapshot(reverse, FunctionA);

        Assert.Equal(forwardA, reverseA);
        Assert.Equal(forwardB, reverseB);
    }

    [Fact]
    public void MutuallyRecursiveFunctions_AreDeterministicUnderConcurrentRequests()
    {
        var expectedProvider = CreateProvider();
        expectedProvider.Prewarm(new[] { FunctionA, FunctionB });
        var expected = new Dictionary<uint, AbiSnapshot>
        {
            [FunctionA] = GetSnapshot(expectedProvider, FunctionA),
            [FunctionB] = GetSnapshot(expectedProvider, FunctionB)
        };
        var provider = CreateProvider();
        var addresses = Enumerable.Range(0, 64)
            .Select(index => index % 2 == 0 ? FunctionB : FunctionA)
            .ToArray();
        provider.Prewarm(addresses);
        var actual = new AbiSnapshot[addresses.Length];

        Parallel.For(0, addresses.Length, index =>
        {
            actual[index] = GetSnapshot(provider, addresses[index]);
        });

        for (var index = 0; index < addresses.Length; index++)
        {
            Assert.Equal(expected[addresses[index]], actual[index]);
        }
    }

    private static InferredGuestFunctionAbiProvider CreateProvider()
    {
        var memory = new byte[0x18];
        WriteWord(memory, 0x00, 0x38800000); // li r4, 0
        WriteWord(memory, 0x04, LinkBranch(FunctionA + 4, FunctionB));
        WriteWord(memory, 0x08, 0x4E800020); // blr
        WriteWord(memory, 0x0C, 0x4E800020);
        WriteWord(memory, 0x10, LinkBranch(FunctionB, FunctionA));
        WriteWord(memory, 0x14, 0x4E800020); // blr
        var range = AddressRange.FromStartAndSize(MemoryLayout.RamBase, (uint)memory.Length);
        var image = new ProgramImage(memory, range, range, default, "abi-cycle-determinism");
        return new InferredGuestFunctionAbiProvider(image);
    }

    private static AbiSnapshot GetSnapshot(InferredGuestFunctionAbiProvider provider, uint address)
    {
        Assert.True(provider.TryGetGuestFunctionAbi($"func_{address:X8}", out var abi));
        return new AbiSnapshot(
            string.Join(",", abi.ArgumentRegisters.Order(StringComparer.OrdinalIgnoreCase)),
            string.Join(",", abi.ScalarFloatArgumentRegisters.Order(StringComparer.OrdinalIgnoreCase)),
            abi.ReturnsPairedScalarFloat,
            abi.WritesFloatReturnRegister,
            abi.WritesGprReturnRegister,
            abi.PreservesVolatileContext);
    }

    private static uint LinkBranch(uint from, uint to) =>
        0x48000001u | ((to - from) & 0x03FFFFFCu);

    private static void WriteWord(byte[] memory, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(offset, 4), value);

    private sealed record AbiSnapshot(
        string ArgumentRegisters,
        string ScalarFloatArgumentRegisters,
        bool ReturnsPairedScalarFloat,
        bool WritesFloatReturnRegister,
        bool WritesGprReturnRegister,
        bool PreservesVolatileContext);
}
