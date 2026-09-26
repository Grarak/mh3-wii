using System.Buffers.Binary;
using Translator.Core.Loading;
using Translator.Core.Translation;
using Xunit;

namespace Translator.Tests;

public sealed class ModTranslationWaveRunnerTests
{
    [Fact]
    public void Translate_IsDeterministicAcrossThreadCounts()
    {
        var memory = new byte[24];
        WriteWord(memory, 0, 0x38600001); // li r3, 1
        WriteWord(memory, 4, 0x4E800020); // blr
        WriteWord(memory, 8, 0x38600002); // li r3, 2
        WriteWord(memory, 12, 0x4E800020); // blr
        WriteWord(memory, 16, 0x38600003); // li r3, 3
        WriteWord(memory, 20, 0x4E800020); // blr
        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, (uint)memory.Length),
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, (uint)memory.Length),
            default,
            "mod-wave-test");
        var knownEntries = new HashSet<uint>
        {
            MemoryLayout.RamBase,
            MemoryLayout.RamBase + 8,
            MemoryLayout.RamBase + 16
        };
        ModTranslationWork Work(uint address) => new(
            address,
            TranslationOptions.Default with
            {
                PreferredName = $"wave_{address:X8}",
                MaxBytes = 8,
                AllowUnsupportedInstructions = true,
                KnownFunctionEntryPoints = knownEntries
            },
            $"{address:X8}.cpp");
        var shuffled = new[]
        {
            Work(MemoryLayout.RamBase + 16),
            Work(MemoryLayout.RamBase),
            Work(MemoryLayout.RamBase + 8)
        };

        var sequentialTranslator = new FunctionTranslator(image);
        var parallelTranslator = new FunctionTranslator(image);
        var sequential = ModTranslationWaveRunner.Translate(
            () => sequentialTranslator,
            shuffled,
            new ParallelOptions { MaxDegreeOfParallelism = 1 });
        var parallel = ModTranslationWaveRunner.Translate(
            () => parallelTranslator,
            shuffled,
            new ParallelOptions { MaxDegreeOfParallelism = 4 });

        Assert.Equal(sequential.Select(item => item.Work.Address), parallel.Select(item => item.Work.Address));
        Assert.Equal(sequential.Select(item => item.Error), parallel.Select(item => item.Error));
        Assert.Equal(sequential.Select(item => item.Result!.CxxCode), parallel.Select(item => item.Result!.CxxCode));
        Assert.Equal(knownEntries.Order(), parallel.Select(item => item.Work.Address));
    }

    private static void WriteWord(byte[] memory, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(offset, 4), value);
}
