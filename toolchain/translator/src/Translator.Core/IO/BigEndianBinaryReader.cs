using System.Buffers.Binary;
using System.Text;

namespace Translator.Core.IO;

internal sealed class BigEndianBinaryReader : IDisposable
{
    private readonly BinaryReader _reader;

    public BigEndianBinaryReader(Stream stream, bool leaveOpen = false)
    {
        _reader = new BinaryReader(stream, Encoding.ASCII, leaveOpen);
    }

    public long Position => _reader.BaseStream.Position;

    public void Seek(long offset, SeekOrigin origin) => _reader.BaseStream.Seek(offset, origin);

    public byte ReadByte() => _reader.ReadByte();

    public ushort ReadUInt16()
    {
        Span<byte> buffer = stackalloc byte[2];
        EnsureRead(buffer);
        return BinaryPrimitives.ReadUInt16BigEndian(buffer);
    }

    public uint ReadUInt32()
    {
        Span<byte> buffer = stackalloc byte[4];
        EnsureRead(buffer);
        return BinaryPrimitives.ReadUInt32BigEndian(buffer);
    }

    public byte[] ReadBytes(int count)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(count);
        var data = new byte[count];
        EnsureRead(data);
        return data;
    }

    private void EnsureRead(Span<byte> destination)
    {
        var start = Position;
        try
        {
            _reader.BaseStream.ReadExactly(destination);
        }
        catch (EndOfStreamException exception)
        {
            throw new EndOfStreamException(
                $"Expected {destination.Length} bytes at 0x{start:X}, but the stream ended early.",
                exception);
        }
    }

    public void Dispose() => _reader.Dispose();
}
