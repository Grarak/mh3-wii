using System.Buffers.Binary;

namespace Translator.Core.Disassembly;

public static partial class PpcDecoder
{
    private static void DecodePrimary19(uint address, uint word, List<PpcOperand> operands, List<uint> branches,
        ref string mnemonic, ref bool isReturn, ref bool isCall, ref bool isCond)
    {
        var field0 = GetField(word, 21, 5);
        var field1 = GetField(word, 16, 5);
        var field2 = GetField(word, 11, 5);
        var xo = GetField(word, 1, 10);
        var lk = (word & 1) != 0;
        var bo = field0;
        var bi = field1;

        if (xo == 16) // bclr
        {
            var isAlways = bo == 20;  // BI is ignored when BO says "always"
            var crField = bi / 4;
            if (isAlways)
            {
                mnemonic = lk ? "blrl" : "blr";
            }
            else if (lk)
            {
                // Conditional with link - use extended mnemonics with 'l' suffix if possible
                var crBit = bi % 4;
                mnemonic = (bo, crBit) switch
                {
                    (12, 0) => "bltlrl",
                    (12, 1) => "bgtlrl",
                    (12, 2) => "beqlrl",
                    (12, 3) => "bsolrl",
                    (4, 0) => "bgelrl",
                    (4, 1) => "blelrl",
                    (4, 2) => "bnelrl",
                    (4, 3) => "bnslrl",
                    _ => "bclrl"
                };
            }
            else
            {
                // Conditional without link - extended mnemonics for common CR-based returns.
                // BO=12 branches when the CR bit is 1, BO=4 branches when it is 0.
                var crBit = bi % 4;
                mnemonic = (bo, crBit) switch
                {
                    (12, 0) => "bltlr",
                    (12, 1) => "bgtlr",
                    (12, 2) => "beqlr",
                    (12, 3) => "bsolr",
                    (4, 0) => "bgelr",
                    (4, 1) => "blelr",
                    (4, 2) => "bnelr",
                    (4, 3) => "bnslr",
                    _ => "bclr"
                };
            }

            isCall = lk;
            isReturn = !lk && mnemonic.EndsWith("lr", StringComparison.OrdinalIgnoreCase);
            isCond = !isAlways;
            if (isCond && crField != 0)
            {
                operands.Add(CrField(crField));
            }
        }
        else if (xo == 528) // bcctr
        {
            var isAlways = bo == 20;  // BI is ignored when BO says "always"
            var crField = bi / 4;
            if (isAlways)
            {
                mnemonic = lk ? "bctrl" : "bctr";
            }
            else
            {
                mnemonic = lk ? "bcctrl" : "bcctr";
            }
            isCall = lk;
            // Unconditional bctr never falls through (tail/virtual dispatch, or later a switch
            // terminator via JumpTableDetector); the disassembler must not treat it as linear code.
            isReturn = !lk && isAlways;
            isCond = !isAlways;
            if (isCond && crField != 0)
            {
                operands.Add(CrField(crField));
            }
        }
        else if (xo == 50)
        {
            mnemonic = "rfi";
            isReturn = true;
        }
        else if (xo == 150)
        {
            mnemonic = "isync";
        }
        else if (xo == 0)
        {
            mnemonic = "mcrf";
            operands.Add(CrField(field0 / 4));
            operands.Add(CrField(field1 / 4));
        }
        else if (xo is 33 or 129 or 193 or 225 or 257 or 289 or 417 or 449)
        {
            mnemonic = xo switch
            {
                33 => "crnor",
                129 => "crandc",
                193 => "crxor",
                225 => "crnand",
                257 => "crand",
                289 => "creqv",
                417 => "crorc",
                449 => "cror",
                _ => "unk19"
            };

            operands.Add(CrBit(field0));
            operands.Add(CrBit(field1));
            operands.Add(CrBit(field2));
        }
        else
        {
            mnemonic = "unk19";
        }
    }
}
