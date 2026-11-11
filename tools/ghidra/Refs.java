// References to addresses, as Ghidra's analysis knows them (data pointers, lis and addi or lwz pairs it resolved):
// for each address, every place that refers to it and the function holding that place. Args: output file, then
// addresses (hex, with or without 0x). Our own script.
//@category BF1943

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class Refs extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
			for (int i = 1; i < args.length; i++) {
				Address a = toAddr(Long.parseLong(args[i].replace("0x", ""), 16));
				out.println("== " + a);
				for (Reference r : getReferencesTo(a)) {
					Address from = r.getFromAddress();
					Function f = getFunctionContaining(from);
					out.println("  " + from + " " + r.getReferenceType() + " in "
							+ (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
				}
			}
		}
		println("refs written to " + args[0]);
	}
}
