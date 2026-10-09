// Headless helper (run with -scriptPath scripts/ghidra): apply build/MAP.txt + symbols.txt names and the mh3.sel data
// names (build/ghidra/data_symbols.txt, written by scripts/dol2elf.py), set r13/r2
// to the small-data bases, then decompile the given addresses into
// <outdir>/<addr>_<name>.c.
// Args: <map> <symbols> <outdir> <addr>...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import java.io.*;
import java.math.BigInteger;
import java.nio.file.*;

public class Mh3Decompile extends GhidraScript {
    void applyNames(String path) throws Exception {
        if (!Files.exists(Path.of(path))) return;
        for (String line : Files.readAllLines(Path.of(path))) {
            line = line.strip();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] p = line.split("\\s+");
            if (p.length < 2) continue;
            Address a = toAddr(Long.parseLong(p[0], 16));
            Function f = getFunctionAt(a);
            if (f == null) f = createFunction(a, null);
            if (f != null && !p[1].startsWith("func_")) f.setName(p[1], SourceType.USER_DEFINED);
        }
    }
    // Data names; returns the small-data bases if present.
    void applyLabels(Path path) throws Exception {
        if (!Files.exists(path)) return;
        for (String line : Files.readAllLines(path)) {
            String[] p = line.strip().split("\\s+");
            if (p.length < 2) continue;
            long value = Long.parseLong(p[0], 16);
            if (p[1].equals("_SDA_BASE_")) setBase("r13", value);
            else if (p[1].equals("_SDA2_BASE_")) setBase("r2", value);
            else createLabel(toAddr(value), p[1], true, SourceType.USER_DEFINED);
        }
    }
    void setBase(String name, long value) throws Exception {
        Register reg = currentProgram.getRegister(name);
        // MEM1 (cached), where all game code lives.
        currentProgram.getProgramContext().setValue(reg, toAddr(0x80000000L), toAddr(0x817FFFFFL),
                                                    BigInteger.valueOf(value));
    }
    public void run() throws Exception {
        String[] args = getScriptArgs();
        applyNames(args[0]);
        applyNames(args[1]);
        applyLabels(Path.of(args[0]).resolveSibling("ghidra/data_symbols.txt"));
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        Files.createDirectories(Path.of(args[2]));
        for (int i = 3; i < args.length; i++) {
            Address a = toAddr(Long.parseLong(args[i], 16));
            Function f = getFunctionAt(a);
            if (f == null) f = createFunction(a, null);
            DecompileResults r = d.decompileFunction(f, 120, monitor);
            String c = r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : r.getErrorMessage();
            Files.writeString(Path.of(args[2], args[i] + "_" + f.getName() + ".c"), c);
        }
    }
}
