// Applies a SAdK-Resurrected sourcemap (types.gdt + sourcemap.json.gz, see README.md) to the current program:
// data types, namespaces/classes, function names and signatures, labels, typed globals and comments.
// Run it on a fresh import of the SAME binary (the script checks its MD5), after auto-analysis.
//
// GUI: Script Manager -> run, then pick the folder holding types.gdt and sourcemap.json.gz.
// Headless: analyzeHeadless ... -postScript ApplySourcemap.java <folder>
//@category SAdK
import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.symbol.*;
import ghidra.program.model.util.CodeUnitInsertionException;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import com.google.gson.*;
import java.io.*;
import java.util.*;
import java.util.zip.GZIPInputStream;

public class ApplySourcemap extends GhidraScript {
  DataTypeManager dtm;
  Map<String, DataType> typeCache = new HashMap<>();
  Map<String, Namespace> nsCache = new HashMap<>();
  int failures = 0;

  void fail(String what, Exception e) {
    failures++;
    if (failures <= 25) printerr(what + ": " + e.getMessage());
  }

  DataType type(String path) {
    if (path == null) return null;
    if (typeCache.containsKey(path)) return typeCache.get(path);
    DataType d = dtm.getDataType(path);
    if (d == null) d = BuiltInDataTypeManager.getDataTypeManager().getDataType(path);
    if (d == null && path.endsWith("]")) {                           // "/T[4]" or "/T *[2]"
      int b = path.lastIndexOf('[');
      DataType el = type(path.substring(0, b));
      int n = Integer.parseInt(path.substring(b + 1, path.length() - 1));
      if (el != null && el.getLength() > 0) d = new ArrayDataType(el, n, el.getLength(), dtm);
    } else if (d == null && path.endsWith(" *")) {                   // "/T *"
      DataType el = type(path.substring(0, path.length() - 2));
      if (el != null) d = new PointerDataType(el, dtm);
    }
    if (d != null) d = dtm.resolve(d, DataTypeConflictHandler.DEFAULT_HANDLER);
    typeCache.put(path, d);
    return d;
  }

  Namespace namespace(String path) throws Exception {
    if (path == null || path.isEmpty()) return currentProgram.getGlobalNamespace();
    Namespace ns = nsCache.get(path);
    if (ns == null) throw new IllegalStateException("namespace not declared: " + path);
    return ns;
  }

  void applyTypes(File gdt) throws Exception {
    FileDataTypeManager arch = FileDataTypeManager.openFileArchive(gdt, false);
    int n = 0;
    try {
      Iterator<DataType> it = arch.getAllDataTypes();
      while (it.hasNext()) {
        DataType d = it.next();
        try {
          dtm.addDataType(d, DataTypeConflictHandler.REPLACE_HANDLER);
          n++;
        } catch (Exception e) {
          fail("type " + d.getPathName(), e);
        }
      }
    } finally {
      arch.close();
    }
    println("types: " + n);
  }

  void applyNamespaces(JsonArray nss) throws Exception {
    SymbolTable st = currentProgram.getSymbolTable();
    List<JsonObject> list = new ArrayList<>();
    for (JsonElement e : nss) list.add(e.getAsJsonObject());
    list.sort(Comparator.comparingInt(o -> o.get("path").getAsString().split("::").length));
    for (JsonObject o : list) {
      String path = o.get("path").getAsString();
      boolean isClass = o.get("class").getAsBoolean();
      int cut = path.lastIndexOf("::");
      Namespace parent = cut < 0 ? currentProgram.getGlobalNamespace() : nsCache.get(path.substring(0, cut));
      String name = cut < 0 ? path : path.substring(cut + 2);
      try {
        Namespace ns = st.getNamespace(name, parent);
        if (ns == null) {
          ns = isClass ? st.createClass(parent, name, SourceType.USER_DEFINED)
                       : st.createNameSpace(parent, name, SourceType.USER_DEFINED);
        } else if (isClass && !(ns instanceof GhidraClass)) {
          ns = NamespaceUtils.convertNamespaceToClass(ns);
        }
        nsCache.put(path, ns);
      } catch (Exception e) {
        fail("namespace " + path, e);
      }
    }
    println("namespaces: " + nsCache.size());
  }

  void applyFunctions(JsonArray fns) {
    int n = 0;
    for (JsonElement el : fns) {
      if (monitor.isCancelled()) return;
      JsonObject o = el.getAsJsonObject();
      Address a = toAddr(o.get("entry").getAsString());
      try {
        Function f = getFunctionAt(a);
        if (f == null) {
          if (getInstructionAt(a) == null) new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
          f = createFunction(a, null);
        }
        if (f == null) throw new IllegalStateException("no function could be created");
        if (o.has("name") && !o.get("name").isJsonNull()) {
          f.setParentNamespace(namespace(o.get("ns").getAsString()));
          f.setName(o.get("name").getAsString(), SourceType.USER_DEFINED);
        }
        // A signature nobody ever set (source DEFAULT, shown as "()") stays as analysis made it.
        boolean hasSig = !"DEFAULT".equals(o.has("sig_source") ? o.get("sig_source").getAsString() : "");
        if (!hasSig) {
          if (o.has("tags")) for (JsonElement t : o.getAsJsonArray("tags")) f.addTag(t.getAsString());
          n++;
          continue;
        }
        String cc = o.has("cc") && !o.get("cc").isJsonNull() ? o.get("cc").getAsString() : null;
        boolean custom = o.has("custom_storage") && o.get("custom_storage").getAsBoolean();
        DataType ret = type(o.has("ret") && !o.get("ret").isJsonNull() ? o.get("ret").getAsString() : null);
        List<ParameterImpl> params = new ArrayList<>();
        for (JsonElement pe : o.getAsJsonArray("params")) {
          JsonObject p = pe.getAsJsonObject();
          DataType t = type(p.get("type").isJsonNull() ? null : p.get("type").getAsString());
          if (t == null) t = DataType.DEFAULT;
          String pname = p.get("name").getAsString();
          params.add(custom
              ? new ParameterImpl(pname, t, VariableStorage.deserialize(currentProgram, p.get("storage").getAsString()), currentProgram)
              : new ParameterImpl(pname, t, currentProgram));
        }
        ReturnParameterImpl r = custom
            ? new ReturnParameterImpl(ret == null ? DataType.DEFAULT : ret,
                  VariableStorage.deserialize(currentProgram, o.get("ret_storage").getAsString()), currentProgram)
            : new ReturnParameterImpl(ret == null ? DataType.DEFAULT : ret, currentProgram);
        f.updateFunction(cc, r, params,
            // the map lists the formal parameters only (no auto `this`)
            custom ? FunctionUpdateType.CUSTOM_STORAGE : FunctionUpdateType.DYNAMIC_STORAGE_FORMAL_PARAMS,
            true, SourceType.USER_DEFINED);
        f.setVarArgs(o.has("varargs") && o.get("varargs").getAsBoolean());
        f.setNoReturn(o.has("noreturn") && o.get("noreturn").getAsBoolean());
        if (o.has("tags")) for (JsonElement t : o.getAsJsonArray("tags")) f.addTag(t.getAsString());
        n++;
      } catch (Exception e) {
        fail("function " + a, e);
      }
    }
    println("functions: " + n + " of " + fns.size());
  }

  void applyLabels(JsonArray labels) {
    SymbolTable st = currentProgram.getSymbolTable();
    int n = 0;
    for (JsonElement el : labels) {
      JsonObject o = el.getAsJsonObject();
      Address a = toAddr(o.get("addr").getAsString());
      try {
        Symbol s = st.createLabel(a, o.get("name").getAsString(), namespace(o.get("ns").getAsString()),
                                  SourceType.USER_DEFINED);
        if (o.has("primary") && o.get("primary").getAsBoolean()) s.setPrimary();
        n++;
      } catch (Exception e) {
        fail("label " + a, e);
      }
    }
    println("labels: " + n + " of " + labels.size());
  }

  void applyData(JsonArray data) {
    int n = 0;
    for (JsonElement el : data) {
      JsonObject o = el.getAsJsonObject();
      Address a = toAddr(o.get("addr").getAsString());
      try {
        DataType t = type(o.get("type").getAsString());
        if (t == null) throw new IllegalStateException("unknown type " + o.get("type").getAsString());
        DataUtilities.createData(currentProgram, a, t, o.get("len").getAsInt(),
                                 DataUtilities.ClearDataMode.CLEAR_ALL_CONFLICT_DATA);
        n++;
      } catch (Exception e) {
        fail("data " + a, e);
      }
    }
    println("data: " + n + " of " + data.size());
  }

  void applyComments(JsonArray comments) {
    Listing li = currentProgram.getListing();
    Map<String, Integer> kinds = Map.of("eol", CodeUnit.EOL_COMMENT, "pre", CodeUnit.PRE_COMMENT,
        "post", CodeUnit.POST_COMMENT, "plate", CodeUnit.PLATE_COMMENT, "repeatable", CodeUnit.REPEATABLE_COMMENT);
    int n = 0;
    for (JsonElement el : comments) {
      JsonObject o = el.getAsJsonObject();
      try {
        li.setComment(toAddr(o.get("addr").getAsString()), kinds.get(o.get("kind").getAsString()),
                      o.get("text").getAsString());
        n++;
      } catch (Exception e) {
        fail("comment " + o.get("addr").getAsString(), e);
      }
    }
    println("comments: " + n + " of " + comments.size());
  }

  public void run() throws Exception {
    String[] args = getScriptArgs();
    File dir = args.length > 0 ? new File(args[0])
                               : askDirectory("Sourcemap folder (types.gdt + sourcemap.json.gz)", "Apply");
    File gdt = new File(dir, "types.gdt"), json = new File(dir, "sourcemap.json.gz");
    if (!gdt.isFile() || !json.isFile()) throw new FileNotFoundException("types.gdt / sourcemap.json.gz not in " + dir);

    JsonObject root;
    try (Reader r = new InputStreamReader(new GZIPInputStream(new FileInputStream(json)), "UTF-8")) {
      root = JsonParser.parseReader(r).getAsJsonObject();
    }
    JsonObject prog = root.getAsJsonObject("program");
    String want = prog.get("md5").getAsString(), have = currentProgram.getExecutableMD5();
    if (!want.equalsIgnoreCase(have)) {
      String msg = "This sourcemap is for " + prog.get("name").getAsString() + " (MD5 " + want
                   + "), but this program's MD5 is " + have + ". Addresses would not match.";
      if (isRunningHeadless() || !askYesNo("Different binary", msg + "\n\nApply anyway?")) {
        printerr(msg);
        return;
      }
    }
    if (!currentProgram.getImageBase().toString().equalsIgnoreCase(prog.get("image_base").getAsString()))
      printerr("Warning: image base differs (" + currentProgram.getImageBase() + " vs "
               + prog.get("image_base").getAsString() + ")");

    dtm = currentProgram.getDataTypeManager();
    long t0 = System.currentTimeMillis();
    monitor.setMessage("SAdK sourcemap: types");
    applyTypes(gdt);
    monitor.setMessage("SAdK sourcemap: namespaces");
    applyNamespaces(root.getAsJsonArray("namespaces"));
    monitor.setMessage("SAdK sourcemap: data");
    applyData(root.getAsJsonArray("data"));
    monitor.setMessage("SAdK sourcemap: functions");
    applyFunctions(root.getAsJsonArray("functions"));
    monitor.setMessage("SAdK sourcemap: labels");
    applyLabels(root.getAsJsonArray("labels"));
    monitor.setMessage("SAdK sourcemap: comments");
    applyComments(root.getAsJsonArray("comments"));
    println(String.format("SAdK sourcemap (%s, exported %s) applied in %d s, %d failure(s)",
        prog.get("name").getAsString(), root.get("exported").getAsString(),
        (System.currentTimeMillis() - t0) / 1000, failures));
  }
}
