using System.Collections.Generic;
using Translator.Core.Disassembly;

namespace Translator.Core.Analysis.BasicBlocks;

public sealed class BasicBlock
{
    public BasicBlock(uint startAddress, IReadOnlyList<PpcInstruction> instructions)
    {
        StartAddress = startAddress;
        Instructions = instructions;
        Successors = new List<uint>();
        Predecessors = new List<uint>();
    }

    public uint StartAddress { get; }
    public IReadOnlyList<PpcInstruction> Instructions { get; }
    public List<uint> Successors { get; }
    public List<uint> Predecessors { get; }

    public PpcInstruction Terminator => Instructions[^1];

    public override string ToString() => $"BB 0x{StartAddress:X8} (len {Instructions.Count})";
}
