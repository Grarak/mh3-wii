using Translator.Core.Disassembly;
using Translator.Core.Ir;

namespace Translator.Core.Lifting;

public sealed record LiftedInstruction(PpcInstruction Origin, IReadOnlyList<IrInstruction> Ir);
