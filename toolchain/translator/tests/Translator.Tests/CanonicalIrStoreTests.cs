using System.Buffers.Binary;
using Translator.Core.Loading;
using Translator.Core.Translation;
using Xunit;

namespace Translator.Tests;

public sealed class CanonicalIrStoreTests
{
    [Fact]
    public void RoundTripFeedsIdenticalFinalLowering()
    {
        var memory = new byte[8];
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(0, 4), 0x38630001u); // addi r3,r3,1
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(4, 4), 0x4E800020u); // blr
        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, (uint)memory.Length),
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, (uint)memory.Length),
            default,
            "canonical-test");
        var translator = new FunctionTranslator(image);
        var options = TranslationOptions.Default with
        {
            PreferredName = "canonical_test",
            AllowUnsupportedInstructions = true
        };
        var direct = translator.Translate(MemoryLayout.RamBase, options);
        var discovery = translator.Discover(MemoryLayout.RamBase, options);
        var store = new CanonicalIrStore();
        store.Put(MemoryLayout.RamBase, discovery.Ssa.Function);

        Assert.Equal(1, store.Count);
        Assert.True(store.TryGet(MemoryLayout.RamBase, out var restored));
        var lowered = translator.LowerCanonical(MemoryLayout.RamBase, restored, options);
        Assert.Equal(direct.CxxCode, lowered.CxxCode);

    }
}
