using System.Text;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    private static bool TryEmitInlineGuestThunk(
        uint address,
        StringBuilder sb,
        string pad,
        RepresentationEnvironment types,
        bool frameBaseIsStack)
    {
        if (!TryGetInlineGuestThunkSpec(address, out var spec))
        {
            return false;
        }

        var frameBase = RegisterToReadExpression("r11", types);
        var isFloat = spec.Kind is InlineGuestThunkKind.SaveFpr or InlineGuestThunkKind.RestFpr;
        var isStore = spec.Kind is InlineGuestThunkKind.SaveGpr or InlineGuestThunkKind.SaveFpr;
        EmitInlineGuestThunkRange(spec, frameBase, sb, pad, types, frameBaseIsStack, isFloat, isStore);
        return true;
    }

    private static void EmitInlineGuestThunkRange(
        InlineGuestThunkSpec spec,
        string frameBase,
        StringBuilder sb,
        string pad,
        RepresentationEnvironment types,
        bool frameBaseIsStack,
        bool isFloat,
        bool isStore)
    {
        var bytesPerRegister = isFloat ? 8 : 4;
        var helperSize = bytesPerRegister;

        if (frameBaseIsStack)
        {
            var firstOffset = (spec.StartRegister - 32) * bytesPerRegister;
            var length = (32 - spec.StartRegister) * bytesPerRegister;
            sb.AppendLine($"{pad}{{");
            var needsRead = isStore ? "false" : "true";
            var needsWrite = isStore ? "true" : "false";
            sb.AppendLine($"{pad}    uint8_t* const guest_thunk_stack = MemoryInline::ResolveRangeHost(({frameBase} + {firstOffset}), 0, {length}u, {needsRead}, {needsWrite});");
            for (var reg = spec.StartRegister; reg <= 31; reg++)
            {
                var offset = (reg - 32) * bytesPerRegister;
                var rangeOffset = offset - firstOffset;
                var registerName = isFloat ? FprRegisterName(reg) : GprRegisterName(reg);
                var value = isStore
                    ? RegisterToReadExpression(registerName, types)
                    : RegisterToWriteExpression(registerName, types);
                if (isStore)
                {
                    var storedValue = isFloat ? value : $"static_cast<uint32_t>({value})";
                    var helper = isFloat ? "WriteResolvedFloat64" : "WriteResolved32";
                    sb.AppendLine($"{pad}    MemoryInline::{helper}(guest_thunk_stack, {rangeOffset}u, ({frameBase} + {offset}), {storedValue});");
                }
                else
                {
                    var helper = isFloat ? "ReadResolvedFloat64" : "ReadResolved32";
                    sb.AppendLine($"{pad}    {value} = MemoryInline::{helper}(guest_thunk_stack, {rangeOffset}u, ({frameBase} + {offset}));");
                }
            }
            sb.AppendLine($"{pad}}}");
            return;
        }

        for (var reg = spec.StartRegister; reg <= 31; reg++)
        {
            var offset = (reg - 32) * bytesPerRegister;
            var registerName = isFloat ? FprRegisterName(reg) : GprRegisterName(reg);
            var value = isStore
                ? RegisterToReadExpression(registerName, types)
                : RegisterToWriteExpression(registerName, types);
            if (isStore)
            {
                var storedValue = isFloat ? value : $"static_cast<uint32_t>({value})";
                sb.AppendLine($"{pad}{GuestStoreHelper(helperSize, isFloat)}(({frameBase} + {offset}), {storedValue});");
            }
            else
            {
                sb.AppendLine($"{pad}{value} = {GuestLoadHelper(helperSize, isFloat)}(({frameBase} + {offset}));");
            }
        }
    }

    private static bool HasLocallyProvenStackFrameBase(
        IReadOnlyList<Translator.Core.Ir.IrInstruction> instructions,
        int callIndex,
        StackAddressFacts stackFacts)
    {
        // EABI save/restore thunks implicitly consume r11, so it is not present
        // in the ordinary argument list. Prove the thunk's frame base from the
        // nearest local SSA definition. Standard compiler prologues/epilogues
        // establish r11 immediately before the thunk; anything less direct
        // deliberately falls back to generic guest memory.
        for (var index = callIndex - 1; index >= 0; --index)
        {
            var destination = instructions[index] switch
            {
                Translator.Core.Ir.IrAssign assign => assign.Destination,
                Translator.Core.Ir.IrBinary binary => binary.Destination,
                Translator.Core.Ir.IrLoad load => load.Destination,
                Translator.Core.Ir.IrCall call => call.Destination,
                Translator.Core.Ir.IrIndirectCall call => call.Destination,
                Translator.Core.Ir.IrPhi phi => phi.Destination,
                _ => null
            };
            if (destination is null ||
                !GetRegisterBaseName(destination).Equals("r11", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            return stackFacts.ContainsTemporary(destination);
        }

        return false;
    }
}
