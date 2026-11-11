// Seed the Battlefield 1943 program loaded from default.xex by XEXLoaderWV: create every function the recompiler
// translated, with its end when .pdata or the manifest gives one, then apply names. Run as a headless preScript
// (before auto analysis) with the seeds folder as the only argument:
//   -preScript SeedBF1943.java F:\BF-1942-MP-Decompuild\seeds
// Seeds come from tools/make_seeds.py. Adapted from the King Kong decomp's SeedKingKong.java (same author): the
// function and name steps unchanged, the King Kong script globals and comments steps left out.
//@category BF1943

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class SeedBF1943 extends GhidraScript {

	private static final long TEXT_START = 0x82160000L;

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File dir = new File(args.length > 0 ? args[0] : "F:\\BF-1942-MP-Decomp\\build\\seeds");

		// XEXLoaderWV maps the sections at their real addresses (its image base field may differ; do not rebase).
		var textBlock = currentProgram.getMemory().getBlock(".text");
		if (textBlock == null || textBlock.getStart().getOffset() != TEXT_START) {
			throw new IllegalStateException(".text is not at 0x82160000; load default.xex with XEXLoaderWV");
		}

		// The kernel import thunks at the end of .text hold import records until the loader patches them, so they
		// decode as bad instructions. This analyzer then marks their callers non returning and clears the code
		// after every such call (2.2 MB of .text and 1,387 functions in our first run). We know the real function
		// list, so it is off.
		setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");

		// 1. functions: disassemble and create, with the exact body when .pdata or the recomp config gives an end
		int created = 0, failed = 0;
		List<String[]> rows = read(new File(dir, "functions.tsv"));
		for (String[] r : rows) {
			Address start = toAddr(Long.parseLong(r[0], 16));
			new DisassembleCommand(start, null, true).applyTo(currentProgram, monitor);
		}
		// Bounded functions first, then the rest from the highest address down, so a function whose end we do not
		// know stops at the next start that already exists instead of swallowing it.
		List<String[]> order = new ArrayList<>();
		for (String[] r : rows) {
			if (r.length > 1 && !r[1].isEmpty()) {
				order.add(r);
			}
		}
		for (int i = rows.size() - 1; i >= 0; i--) {
			String[] r = rows.get(i);
			if (r.length < 2 || r[1].isEmpty()) {
				order.add(r);
			}
		}
		for (String[] r : order) {
			monitor.checkCancelled();
			Address start = toAddr(Long.parseLong(r[0], 16));
			Function f = getFunctionAt(start);
			if (f == null) {
				AddressSet body = null;
				if (r.length > 1 && !r[1].isEmpty()) {
					Address end = toAddr(Long.parseLong(r[1], 16));
					body = new AddressSet(start, end.subtract(1));
				}
				CreateFunctionCmd cmd = new CreateFunctionCmd(null, start, body, SourceType.ANALYSIS);
				if (cmd.applyTo(currentProgram, monitor)) {
					created++;
				}
				else {
					failed++;
					if (failed <= 20) {
						println("could not create function at " + start + ": " + cmd.getStatusMsg());
					}
				}
				f = getFunctionAt(start);
			}
			if (f != null) {
				f.setComment("seed: " + (r.length > 2 ? r[2] : ""));
			}
		}
		println("functions created " + created + ", failed " + failed + " of " + rows.size());

		// 2. names (engine name tables first, then port/report names; see names.tsv evidence)
		int named = 0;
		for (String[] r : read(new File(dir, "names.tsv"))) {
			Address a = toAddr(Long.parseLong(r[0], 16));
			Function f = getFunctionAt(a);
			if (f == null) {
				println("no function for name " + r[1] + " at " + a);
				continue;
			}
			f.setName(r[1], SourceType.IMPORTED);
			setPlateComment(a, "Name from " + r[2] + ": " + r[3]);
			// Register save/restore helpers (__savegprlr_N etc.) are entered in the middle by the caller's bl;
			// inlined, the decompiler sees the register saves instead of a call that returns r3.
			if (r[1].startsWith("__save") || r[1].startsWith("__rest")) {
				f.setInline(true);
			}
			named++;
		}
		println("named " + named);

	}

	private static List<String[]> read(File f) throws Exception {
		List<String[]> out = new ArrayList<>();
		try (BufferedReader br = new BufferedReader(
			new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
			br.readLine(); // header
			String line;
			while ((line = br.readLine()) != null) {
				if (!line.isEmpty()) {
					out.add(line.split("\t", -1));
				}
			}
		}
		return out;
	}
}
