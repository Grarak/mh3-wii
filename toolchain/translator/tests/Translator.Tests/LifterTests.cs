using System.Collections.Generic;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

public class LifterTests
{
    [Fact]
    public void LiftsSimpleBlock()
    {
        // Synthetic block: li r3,5; mr r4,r3; blr
        var instructions = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(0x80000000, 0x38600005, "li", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(5) }),
            PpcInstruction.Synthetic(0x80000004, 0x7C832378, "mr", new PpcOperand[] { new PpcRegisterOperand("r4", 4), new PpcRegisterOperand("r3", 3), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(0x80000008, 0x4E800020, "blr", Array.Empty<PpcOperand>())
        };

        var lifter = new PpcLifter();
        var lifted = lifter.Lift(instructions);
        Assert.Equal(instructions, lifted.Select(item => item.Origin));
        Assert.IsType<IrAssign>(lifted[0].Ir.First());
        Assert.IsType<IrAssign>(lifted[1].Ir.First());
        Assert.IsType<IrReturn>(lifted[2].Ir.Single());
    }
}
