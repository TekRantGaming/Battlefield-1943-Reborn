// Decompile functions to C text for reading. Args: output file, then addresses (hex, with or without 0x), or
// "tree:<addr>:<depth>" to take a function and its callees down to that depth.
// Each function is printed with its name, size and callees, then the decompiler output.
//@category BF1943
// Copied from the King Kong decomp (E:/kk-decomp/scripts/ghidra, same author), unchanged apart from the category.

import java.io.PrintWriter;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		Map<Function, Integer> todo = new LinkedHashMap<>();
		for (int i = 1; i < args.length; i++) {
			String a = args[i];
			int depth = 0;
			if (a.startsWith("tree:")) {
				String[] p = a.split(":");
				a = p[1];
				depth = Integer.parseInt(p[2]);
			}
			Function f = getFunctionContaining(toAddr(Long.parseLong(a.replace("0x", ""), 16)));
			if (f == null) {
				println("no function at " + a);
				continue;
			}
			ArrayDeque<Object[]> q = new ArrayDeque<>();
			q.add(new Object[] { f, 0 });
			while (!q.isEmpty()) {
				Object[] e = q.poll();
				Function g = (Function) e[0];
				int d = (Integer) e[1];
				if (todo.containsKey(g)) {
					continue;
				}
				todo.put(g, d);
				if (d < depth) {
					for (Function c : g.getCalledFunctions(monitor)) {
						if (!c.isThunk()) {
							q.add(new Object[] { c, d + 1 });
						}
					}
				}
			}
		}

		DecompInterface di = new DecompInterface();
		di.setOptions(new DecompileOptions());
		di.openProgram(currentProgram);
		try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
			for (Map.Entry<Function, Integer> e : todo.entrySet()) {
				Function f = e.getKey();
				out.println("//==== " + f.getName() + " @ " + f.getEntryPoint() + " size " +
					f.getBody().getNumAddresses() + " depth " + e.getValue());
				StringBuilder calls = new StringBuilder();
				for (Function c : f.getCalledFunctions(monitor)) {
					calls.append(' ').append(c.getName()).append('@').append(c.getEntryPoint());
				}
				out.println("// calls:" + calls);
				DecompileResults r = di.decompileFunction(f, 60, monitor);
				out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " +
					r.getErrorMessage());
			}
		}
		di.dispose();
		println("decompiled " + todo.size() + " functions to " + args[0]);
	}
}
