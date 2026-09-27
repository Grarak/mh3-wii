using System.Buffers.Binary;
using Translator.Core.CodeGen;
using Translator.Core.Loading;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public sealed class PairedScaleArgumentAbiTests
{
    [Theory]
    [InlineData(0x10000058u, true)] // ps_muls0 f0, f0, f1
    [InlineData(0x1000005Au, false)] // ps_muls1 needs PS1, not a scalar PS0
    [InlineData(0x1000005Cu, true)] // ps_madds0 f0, f0, f1, f0
    [InlineData(0x1000005Eu, false)] // ps_madds1 needs PS1
    public void InfersLaneZeroScaleThroughFloatMoveWrapper(uint multiply, bool scalar)
    {
        const uint start = MemoryLayout.RamBase;
        var words = new uint[] {
            0xFFE00890, // fmr f31, f1
            0xFC20F890, // fmr f1, f31
            0x48000019, // bl start+0x20
            0x4E800020,
            0x4E800020, 0x4E800020, 0x4E800020, 0x4E800020,
            0xE0040000, multiply, 0xF0030000, // load, scale, store XY
            0xE0048008, multiply, 0xF0038008, // load, scale, store Z
            0x4E800020,
        };
        var memory = new byte[words.Length * 4];
        for (var i = 0; i < words.Length; ++i)
            BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(i * 4, 4), words[i]);
        var range = AddressRange.FromStartAndSize(start, (uint)memory.Length);
        var image = new ProgramImage(memory, range, range, default, "paired-scale-argument");
        var provider = new InferredGuestFunctionAbiProvider(image);
        Assert.True(provider.TryGetGuestFunctionAbi($"func_{start + 0x20:X8}", out var leaf));
        Assert.Equal(scalar, leaf.HasScalarFloatArgument("f1"));
        Assert.True(provider.TryGetGuestFunctionAbi($"func_{start:X8}", out var wrapper));
        Assert.Equal(scalar, wrapper.HasScalarFloatArgument("f1"));
    }
}
