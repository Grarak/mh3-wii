using Translator.Core.Mods.Mkwii;
using Translator.Core.Parsing.Kamek;

namespace Translator.Tests;

public class RetroWfcBootstrapSuppressorTests
{
    [Fact]
    public void SuppressRemovesOnlyConfiguredLegacyBootstrapBranch()
    {
        var bootstrap = Command(KamekCommandId.Branch, 0x800ED6E8u, 0x0003437Cu);
        var other = Command(KamekCommandId.Write32, 0x800EE3A0u, 0x2C030000u);
        var chunk = Chunk([bootstrap, other]);

        var result = RetroWfcBootstrapSuppressor.Suppress(chunk, 0x800ED6E8u);

        Assert.Equal([other], result.Commands);
        Assert.Equal(2, chunk.Commands.Count);
    }

    [Fact]
    public void SuppressRejectsStaleOrIncorrectProfileAddress()
    {
        var chunk = Chunk([Command(KamekCommandId.Branch, 0x800ED6E8u, 0x0003437Cu)]);

        var error = Assert.Throws<InvalidDataException>(
            () => RetroWfcBootstrapSuppressor.Suppress(chunk, 0x800ED6ECu));

        Assert.Contains("0x800ED6EC", error.Message, StringComparison.Ordinal);
    }

    private static KamekCommand Command(KamekCommandId id, uint address, uint argument) =>
        new(0, 0, id, true, address, [argument]);

    private static KamekChunk Chunk(IReadOnlyList<KamekCommand> commands) =>
        new(0, 0, 0, 4, 0, 0, 4, [0, 0, 0, 0], commands);
}
