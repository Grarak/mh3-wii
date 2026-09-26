using System.IO;
using System.Linq;
using Translator.Core.Analysis.BasicBlocks;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Xunit;

namespace Translator.Tests;

public class BasicBlockTests
{
    private static (ProgramImage image, PpcDisassembler disassembler) Setup()
    {
        var root = ProjectPaths.FindRepositoryRoot();
        var assets = Path.Combine(root, "assets");
        var image = new ProgramImageBuilder().Build(Path.Combine(assets, "main.dol"), Path.Combine(assets, "StaticR.rel"));
        return (image, new PpcDisassembler());
    }

    [Fact]
    public void BuildsBasicBlocksWithTerminatingBranches()
    {
        var (image, dis) = Setup();
        using (dis)
        {
            var instructions = dis.DisassembleFunction(image, 0x800060A4, maxInstructions: 128);
            var blocks = BasicBlockBuilder.Build(instructions);

            Assert.NotEmpty(blocks);
            Assert.True(blocks.All(b => b.Instructions.Count > 0), "Every block should contain at least one instruction.");
            Assert.True(blocks.All(b =>
                    b.Terminator.IsReturn ||
                    b.Terminator.IsConditionalBranch ||
                    b.Terminator.IsUnconditionalBranch ||
                    b.Terminator.IsCall ||
                    b.Successors.Count > 0),
                "Each block must either end with a control-transfer or have an explicit fallthrough successor.");

            // CFG stitching: every successor must list this block as predecessor.
            foreach (var block in blocks)
            {
                foreach (var succAddr in block.Successors)
                {
                    var succ = blocks.Single(b => b.StartAddress == succAddr);
                    Assert.Contains(block.StartAddress, succ.Predecessors);
                }
            }
        }
    }

    [Fact]
    public void BuildSplitsAtRequestedEntryLeaderInsideDecodedRange()
    {
        var instructions = new[]
        {
            PpcInstruction.Synthetic(0x8064F7EC, 0x60000000, "nop", []),
            PpcInstruction.Synthetic(0x8064F7F0, 0x60000000, "nop", []),
            PpcInstruction.Synthetic(0x8064F7F4, 0x60000000, "nop", []),
            PpcInstruction.Synthetic(0x8064F7F8, 0x60000000, "nop", []),
            PpcInstruction.Synthetic(0x8064F7FC, 0x4E800020, "blr", [], isReturn: true)
        };

        var blocks = BasicBlockBuilder.Build(instructions, new[] { 0x8064F7F4u });

        Assert.Contains(blocks, b => b.StartAddress == 0x8064F7ECu);
        Assert.Contains(blocks, b => b.StartAddress == 0x8064F7F4u);
        Assert.Equal(new[] { 0x8064F7ECu, 0x8064F7F4u }, blocks.Select(b => b.StartAddress).ToArray());
    }
}
