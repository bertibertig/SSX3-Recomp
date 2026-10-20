/* Read-only: decompile the function containing each address argument. An address in code Ghidra never made a
 * function of gets one for this run only (the query opens the project read-only, nothing is saved). */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.Function;

public class Decomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (String s : getScriptArgs()) {
            Function f = getFunctionContaining(toAddr(Long.parseLong(s.replace("0x", ""), 16)));
            if (f == null) {
                disassemble(toAddr(Long.parseLong(s.replace("0x", ""), 16)));
                f = createFunction(toAddr(Long.parseLong(s.replace("0x", ""), 16)), null);
            }
            if (f == null) { println("DC no function at " + s); continue; }
            DecompileResults r = d.decompileFunction(f, 60, monitor);
            println("DC ===== " + f.getName() + " @ " + f.getEntryPoint());
            println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "DC failed: " + r.getErrorMessage());
        }
    }
}
