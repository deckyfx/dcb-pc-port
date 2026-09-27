// Import a DCB_COVERAGE JSON report into this program: tag executed functions,
// bookmark them, and put call counts + first frame in a comment. Safe to re-run:
// clears its own previous tags/bookmarks/comments before applying the new ones.
//
// Java (not Python) so it runs in any Ghidra launch mode, GUI or headless, without PyGhidra.
//
// GUI:       Script Manager > DCB > ImportCoverage.java (arg: coverage JSON path)
// Headless:  support/analyzeHeadless ghidra/project DCB/SLPS-03101 -process SLPS_031.01 -noanalysis \
//              -scriptPath ghidra/scripts -postScript ImportCoverage.java /tmp/dcb-re-tracing/cov.json
//
// Overlays share one address window (KAWSEG::801E2A6C, ...): the report's overlay field
// selects the overlay block's address space, exactly like ApplyDiscovered.java does.
// @category DCB

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Iterator;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.BookmarkType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class ImportCoverage extends GhidraScript {

    private static long hex(JsonElement e) {
        return Long.parseLong(e.getAsString().replaceFirst("^0[xX]", ""), 16);
    }

    @Override
    public void run() throws Exception {
        final String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: ImportCoverage.java <coverage.json>");
            return;
        }
        final JsonObject report =
            JsonParser.parseString(Files.readString(Path.of(args[0]))).getAsJsonObject();
        final JsonArray entries = report.getAsJsonArray("entries");

        final Memory memory = currentProgram.getMemory();
        final FunctionManager functions = currentProgram.getFunctionManager();
        final AddressSpace defaultSpace = currentProgram.getAddressFactory().getDefaultAddressSpace();

        // Clear our own previous run: tags, bookmarks, comments.
        for (Function f : functions.getFunctions(true)) {
            monitor.checkCancelled();
            for (ghidra.program.model.listing.FunctionTag tag : f.getTags()) {
                if (tag.getName().startsWith("DCB_COV")) f.removeTag(tag.getName());
            }
            if (f.getComment() != null && f.getComment().startsWith("DCB coverage:")) f.setComment(null);
        }
        currentProgram.getBookmarkManager().removeBookmarks("DCB Coverage");

        int tagged = 0, missing = 0;
        for (JsonElement e : entries) {
            monitor.checkCancelled();
            final JsonObject fn = e.getAsJsonObject();
            if (!fn.has("calls") || fn.get("calls").getAsLong() == 0) continue;
            final String overlay = fn.has("overlay") ? fn.get("overlay").getAsString() : "";
            AddressSpace space = defaultSpace;
            if (!overlay.isEmpty()) {
                final MemoryBlock block = memory.getBlock(overlay);
                if (block == null) {
                    printerr("no overlay block " + overlay + " (run ApplyDiscovered first)");
                    ++missing;
                    continue;
                }
                space = block.getStart().getAddressSpace();
            }
            final Address addr = space.getAddress(hex(fn.get("addr")));
            final Function f = functions.getFunctionAt(addr);
            if (f == null) {
                ++missing;
                continue;
            }
            f.addTag("DCB_COV_EXECUTED");
            final long calls = fn.get("calls").getAsLong();
            final long first = fn.has("first_frame") ? fn.get("first_frame").getAsLong() : 0;
            f.setComment("DCB coverage: " + calls + " calls, first frame " + first);
            createBookmark(addr, "DCB Coverage", calls + " calls, first frame " + first);
            ++tagged;
        }
        println("ImportCoverage: " + tagged + " functions tagged, " + missing + " addresses without function");
    }
}
