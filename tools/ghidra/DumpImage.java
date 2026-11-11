// Writes the loaded (decrypted, decompressed) memory image of the current program to a flat file, one byte per
// virtual address starting at the image base, so Python tools can read it with offset = VA minus base.
// Usage (headless): -postScript DumpImage.java <output file> [base, hex, default 82000000]
// (XEXLoaderWV leaves the program image base at 0, so the base is given here.)
// @category BF1943

import java.io.RandomAccessFile;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;

public class DumpImage extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        String[] args = getScriptArgs();
        Address base = currentProgram.getAddressFactory().getDefaultAddressSpace()
            .getAddress(args.length > 1 ? args[1] : "82000000");
        try (RandomAccessFile f = new RandomAccessFile(out, "rw")) {
            f.setLength(0);
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                if (!b.isInitialized() || !b.getStart().getAddressSpace().equals(base.getAddressSpace())) {
                    continue;
                }
                byte[] data = new byte[(int) b.getSize()];
                b.getBytes(b.getStart(), data);
                long off = b.getStart().subtract(base);
                f.seek(off);
                f.write(data);
                println(String.format("%-12s %s..%s %8X bytes %s%s%s", b.getName(), b.getStart(), b.getEnd(),
                    b.getSize(), b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-", b.isExecute() ? "x" : "-"));
            }
        }
    }
}
