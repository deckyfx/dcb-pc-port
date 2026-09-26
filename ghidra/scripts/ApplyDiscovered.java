// Apply the recompiler's function discovery to this program, so Ghidra shows exactly what gets
// recompiled: creates missing functions in the boot EXE, and adds every P.DRV overlay as a Ghidra
// overlay block (KAWSEG::801E2A6C, ...) with its functions. Safe to re-run.
//
// Java (not Python) so it runs in any Ghidra launch mode, GUI or headless, without PyGhidra.
//
// GUI:       Script Manager > DCB > ApplyDiscovered.java (arg optional: repo root)
// Headless:  support/analyzeHeadless ghidra/project DCB/SLPS-03101 -process SLPS_031.01 -noanalysis \
//              -scriptPath ghidra/scripts -postScript ApplyDiscovered.java <repo root>
//
// Inputs: generated/<serial>/discovered.json (from dcb_recompiler), config/<serial>/overlays.json,
//         extracted/<serial>/fs/P.DRV. <serial> is this program's project folder name.
// @category DCB

import java.io.ByteArrayInputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.TreeMap;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class ApplyDiscovered extends GhidraScript {

    private static long hex(JsonElement e) {
        return Long.parseLong(e.getAsString().replaceFirst("^0[xX]", ""), 16);
    }

    private static JsonObject readJson(Path path) throws Exception {
        return JsonParser.parseString(Files.readString(path)).getAsJsonObject();
    }

    @Override
    public void run() throws Exception {
        final String[] args = getScriptArgs();
        final Path root = args.length > 0
            ? Path.of(args[0])
            : getSourceFile().getParentFile().getParentFile().getParentFile().getFile(false).toPath();
        final String serial = currentProgram.getDomainFile().getParent().getName();

        final JsonObject discovered = readJson(root.resolve("generated").resolve(serial).resolve("discovered.json"));
        final JsonObject overlays = readJson(root.resolve("config").resolve(serial).resolve("overlays.json"));
        final byte[] container = Files.readAllBytes(
            root.resolve("extracted").resolve(serial).resolve("fs").resolve(overlays.get("container").getAsString()));

        final Map<String, byte[]> segmentBytes = new HashMap<>();
        for (JsonElement e : overlays.getAsJsonArray("segments")) {
            final JsonObject s = e.getAsJsonObject();
            final int off = (int) hex(s.get("file_offset"));
            final int size = (int) hex(s.get("size"));
            segmentBytes.put(s.get("name").getAsString(), Arrays.copyOfRange(container, off, off + size));
        }

        final Memory memory = currentProgram.getMemory();
        final FunctionManager functions = currentProgram.getFunctionManager();
        final AddressSpace defaultSpace = currentProgram.getAddressFactory().getDefaultAddressSpace();
        final Map<String, Integer> created = new TreeMap<>();
        int blocksAdded = 0;
        int failed = 0;

        for (JsonElement segElem : discovered.getAsJsonArray("segments")) {
            final JsonObject seg = segElem.getAsJsonObject();
            final String name = seg.get("name").getAsString();
            AddressSpace space = defaultSpace;
            if (!name.equals("main")) {
                MemoryBlock block = memory.getBlock(name);
                if (block == null) {
                    final byte[] data = segmentBytes.get(name);
                    block = memory.createInitializedBlock(name, defaultSpace.getAddress(hex(seg.get("base"))),
                        new ByteArrayInputStream(data), data.length, monitor, true);
                    block.setRead(true);
                    block.setWrite(true);
                    block.setExecute(true);
                    block.setComment("P.DRV overlay " + name + " (ApplyDiscovered.java)");
                    ++blocksAdded;
                }
                space = block.getStart().getAddressSpace();
            }

            for (JsonElement fnElem : seg.getAsJsonArray("functions")) {
                monitor.checkCancelled();
                final JsonObject fn = fnElem.getAsJsonObject();
                final Address addr = space.getAddress(hex(fn.get("entry")));
                if (functions.getFunctionAt(addr) != null) continue;
                new DisassembleCommand(addr, null, true).applyTo(currentProgram, monitor);
                new CreateFunctionCmd(addr).applyTo(currentProgram, monitor);
                final Function f = functions.getFunctionAt(addr);
                if (f == null) {
                    printerr("could not create function at " + addr);
                    ++failed;
                    continue;
                }
                f.addTag("DCB_" + fn.get("origin").getAsString().toUpperCase());
                created.merge(name, 1, Integer::sum);
            }
        }
        println(String.format("ApplyDiscovered: %d overlay blocks added; functions created %s; %d failed",
            blocksAdded, created, failed));
    }
}
