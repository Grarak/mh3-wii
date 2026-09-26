using System.Buffers.Binary;
using Translator.Core.Mods;
using Translator.Core.Parsing.Kamek;
using Xunit;

namespace Translator.Tests;

/// <summary>
/// Pins base-translation reuse decisions: a Code.pul that touches nothing inside a translated
/// function should be reusable, and one that moves a patch on/off a translated function must not be.
/// </summary>
public class BaseTranslationModAwarenessTests
{
    // Two translated functions with a gap between them, so an address can be provably outside both.
    private static readonly Dictionary<uint, uint> FunctionEnds = new()
    {
        [0x80005F34] = 0x8000608C,
        [0x80543BB4] = 0x80543C40
    };

    [Fact]
    public void ReusesTheBaseTranslationForAPulThatPatchesTheSameTranslatedFunctions()
    {
        using var original = TemporaryPul(
            Absolute(KamekCommandId.BranchLink, 0x80005F40),
            Absolute(KamekCommandId.Write32, 0x80001920));
        using var candidate = TemporaryPul(
            // The low-memory write is gone and a new one appears, but neither is inside a translated
            // function, so the translation could not have seen either.
            Absolute(KamekCommandId.BranchLink, 0x80005F40),
            Absolute(KamekCommandId.Write32, 0x80003000));

        var awareness = Awareness(original.Path);

        Assert.True(awareness.CoversCodePul("retro-rewind", candidate.Path, out var reason), reason);
    }

    [Fact]
    public void RefusesAPulThatMovesAPatchOntoADifferentTranslatedFunction()
    {
        using var original = TemporaryPul(Absolute(KamekCommandId.BranchLink, 0x80005F40));
        using var candidate = TemporaryPul(Absolute(KamekCommandId.BranchLink, 0x80543BB8));

        var awareness = Awareness(original.Path);

        Assert.False(awareness.CoversCodePul("retro-rewind", candidate.Path, out var reason));
        Assert.Contains("80543BB8", reason, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void RefusesAPulThatStopsPatchingATranslatedFunction()
    {
        using var original = TemporaryPul(
            Absolute(KamekCommandId.BranchLink, 0x80005F40),
            Absolute(KamekCommandId.Branch, 0x80543BB8));
        using var candidate = TemporaryPul(Absolute(KamekCommandId.BranchLink, 0x80005F40));

        var awareness = Awareness(original.Path);

        Assert.False(awareness.CoversCodePul("retro-rewind", candidate.Path, out _));
    }

    [Fact]
    public void RefusesAProfileTheBaseTranslationNeverSaw()
    {
        using var original = TemporaryPul(Absolute(KamekCommandId.BranchLink, 0x80005F40));
        var awareness = Awareness(original.Path);

        Assert.False(awareness.CoversCodePul("some-other-mod", original.Path, out var reason));
        Assert.Contains("retro-rewind", reason, StringComparison.Ordinal);
    }

    [Fact]
    public void RefusesWhenSeveralProfilesShapedTheTranslation()
    {
        using var first = TemporaryPul(Absolute(KamekCommandId.BranchLink, 0x80005F40));
        using var second = TemporaryPul(Absolute(KamekCommandId.Branch, 0x80543BB8));
        var awareness = BaseTranslationModAwareness.Create(null, 2,
            [
                ("retro-rewind", "P", new string('a', 64),
                    BaseTranslationModAwareness.PatchedAddresses(first.Path, "P")),
                ("other", "P", new string('b', 64),
                    BaseTranslationModAwareness.PatchedAddresses(second.Path, "P"))
            ],
            FunctionEnds);

        Assert.False(awareness.CoversCodePul("retro-rewind", first.Path, out var reason));
        Assert.Contains("2 mod profile", reason, StringComparison.Ordinal);
    }

    [Fact]
    public void SurvivesAWriteAndReadRoundTrip()
    {
        using var original = TemporaryPul(
            Absolute(KamekCommandId.BranchLink, 0x80005F40),
            Absolute(KamekCommandId.Write32, 0x80001920));
        var path = Path.Combine(Path.GetTempPath(), $"mkwc-awareness-{Guid.NewGuid():N}.json");
        try
        {
            BaseTranslationModAwarenessFile.WriteIfChangedAtomic(path, Awareness(original.Path));
            var round = BaseTranslationModAwarenessFile.Read(path);

            Assert.Equal(2, round.TranslatedFunctionCount);
            Assert.Equal(4, round.TranslatedFunctionRanges.Count);
            // Only the patch inside a translated function is recorded as consequential.
            Assert.Equal([0x80005F40u], round.Profiles[0].ConsequentialPatchedAddresses);
            Assert.True(round.CoversCodePul("retro-rewind", original.Path, out var reason), reason);
        }
        finally
        {
            File.Delete(path);
        }
    }

    private static BaseTranslationModAwareness Awareness(string codePulPath) =>
        BaseTranslationModAwareness.Create(null, FunctionEnds.Count,
            [("retro-rewind", "P", new string('a', 64),
                BaseTranslationModAwareness.PatchedAddresses(codePulPath, "P"))],
            FunctionEnds);

    private static TemporaryFile TemporaryPul(params byte[][] commands)
    {
        var file = new TemporaryFile();
        File.WriteAllBytes(file.Path, BuildChunk([0x60, 0x00, 0x00, 0x00], commands));
        return file;
    }

    private static byte[] BuildChunk(byte[] code, byte[][] commands)
    {
        var chunkSize = KamekChunk.HeaderSize + code.Length + commands.Sum(command => command.Length);
        var data = new byte[chunkSize];
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(0x00), KamekChunk.Magic0);
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(0x04), KamekChunk.Magic1);
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(0x08), 0x10);
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(0x0C), (uint)code.Length);
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(0x18), (uint)chunkSize);
        code.CopyTo(data, KamekChunk.HeaderSize);
        var offset = KamekChunk.HeaderSize + code.Length;
        foreach (var command in commands)
        {
            command.CopyTo(data, offset);
            offset += command.Length;
        }
        return data;
    }

    private static byte[] Absolute(KamekCommandId id, uint address)
    {
        var data = new byte[12];
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(0), ((uint)(byte)id << 24) | 0x00FFFFFEu);
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(4), address);
        return data;
    }

    private sealed class TemporaryFile : IDisposable
    {
        public string Path { get; } =
            System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"mkwc-pul-{Guid.NewGuid():N}.pul");

        public void Dispose() => File.Delete(Path);
    }
}
