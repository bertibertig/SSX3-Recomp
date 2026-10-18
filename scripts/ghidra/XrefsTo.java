/* Read-only: list every reference to each address argument, with the
 * containing function and the instruction. */
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class XrefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        Listing l = currentProgram.getListing();
        for (String s : getScriptArgs()) {
            Address a = toAddr(Long.parseLong(s.replace("0x", ""), 16));
            println("XR === refs to " + a);
            for (Reference r : getReferencesTo(a)) {
                Address from = r.getFromAddress();
                Function f = getFunctionContaining(from);
                Instruction i = l.getInstructionAt(from);
                println("XR " + from + " " + r.getReferenceType() + " func="
                        + (f == null ? "-" : f.getEntryPoint().toString()) + " " + (i == null ? "?" : i.toString()));
            }
        }
    }
}
