// Headless helper (run with -scriptPath scripts/ghidra): print the Gekko
// disassembly of each given function (paired singles decoded, which generic
// PowerPC disassemblers get wrong) into <outdir>/<addr>.s.
// Args: <outdir> <addr>...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import java.io.*;
import java.nio.file.*;

public class Mh3Listing extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path outdir = Path.of(args[0]);
        Files.createDirectories(outdir);
        for (int i = 1; i < args.length; ++i) {
            Address a = toAddr(Long.parseLong(args[i], 16));
            Function f = getFunctionAt(a);
            if (f == null) f = createFunction(a, null);
            if (f == null) {
                println("no function at " + args[i]);
                continue;
            }
            try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(outdir.resolve(args[i].toUpperCase() + ".s")))) {
                for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true))
                    w.printf("%s  %s%n", ins.getAddress(), ins);
            }
        }
    }
}
