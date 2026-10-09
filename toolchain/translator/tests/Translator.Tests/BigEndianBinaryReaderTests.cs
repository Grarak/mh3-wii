using System;
using System.IO;
using Translator.Core.IO;
using Xunit;

namespace Translator.Tests;

public class BigEndianBinaryReaderTests
{
    [Fact]
    public void ReadsBigEndianValuesAndTracksPosition()
    {
        using var stream = new MemoryStream(new byte[]
        {
            0x12,
            0x34, 0x56,
            0x78, 0x9A, 0xBC, 0xDE,
            0xF0, 0x0D
        });
        using var reader = new BigEndianBinaryReader(stream);

        Assert.Equal(0, reader.Position);
        Assert.Equal(0x12, reader.ReadByte());
        Assert.Equal(1, reader.Position);
        Assert.Equal(0x3456, reader.ReadUInt16());
        Assert.Equal(0x789ABCDEu, reader.ReadUInt32());
        Assert.Equal(new byte[] { 0xF0, 0x0D }, reader.ReadBytes(2));
        Assert.Equal(stream.Length, reader.Position);
    }

    [Fact]
    public void SeekMovesWithinStream()
    {
        using var stream = new MemoryStream(new byte[] { 0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD });
        using var reader = new BigEndianBinaryReader(stream);

        reader.Seek(2, SeekOrigin.Begin);
        Assert.Equal(2, reader.Position);
        Assert.Equal(0xAABB, reader.ReadUInt16());

        reader.Seek(-2, SeekOrigin.Current);
        Assert.Equal(2, reader.Position);
        Assert.Equal(0xAABBCCDDu, reader.ReadUInt32());
    }

    [Fact]
    public void ThrowsOnShortReads()
    {
        using var stream = new MemoryStream(new byte[] { 0x12, 0x34, 0x56 });
        using var reader = new BigEndianBinaryReader(stream);

        Assert.Throws<EndOfStreamException>(() => reader.ReadUInt32());
        reader.Seek(0, SeekOrigin.Begin);
        Assert.Throws<EndOfStreamException>(() => reader.ReadBytes(4));
    }

    [Fact]
    public void LeaveOpenPreservesUnderlyingStream()
    {
        var stream = new MemoryStream(new byte[] { 0xDE, 0xAD, 0xBE, 0xEF });
        using (var reader = new BigEndianBinaryReader(stream, leaveOpen: true))
        {
            Assert.Equal(0xDEAD, reader.ReadUInt16());
        }

        Assert.True(stream.CanRead);
        Assert.Equal(2, stream.Position);
        using var reader2 = new BigEndianBinaryReader(stream, leaveOpen: true);
        Assert.Equal(0xBEEF, reader2.ReadUInt16());
        stream.Dispose();
    }
}
