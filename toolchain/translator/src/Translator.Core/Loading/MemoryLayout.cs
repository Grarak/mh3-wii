namespace Translator.Core.Loading;

public static class MemoryLayout
{
    public const uint RamBase = 0x80000000;
    public const int RamSize = 24 * 1024 * 1024; // 24 MiB MEM1
    public const uint DolBaseAddress = 0x80004000; // First executable section start
}
