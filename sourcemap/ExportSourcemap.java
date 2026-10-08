// [ai] Read-only. Exports the program's reverse-engineering map for sharing (sourcemap/README.md):
//   sourcemap/<program>/types.gdt          every data type the program defines (classes, structs, vtables,
//                                          enums, function definitions) as a Ghidra data type archive
//   sourcemap/<program>/sourcemap.json.gz  namespaces/classes, named functions with signatures, labels,
//                                          typed globals and all comments, keyed by address
// Applied to a fresh import of the same binary by sourcemap/ApplySourcemap.java.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import com.google.gson.*;
import java.io.*;
import java.text.SimpleDateFormat;
import java.util.*;
import java.util.zip.GZIPOutputStream;

public class ExportSourcemap extends GhidraScript {
  // The repo's sourcemap folder: the script's first argument, else ~/projects/sadk-resurrected/sourcemap
  static final String DEFAULT_ROOT = System.getProperty("user.home") + "/projects/sadk-resurrected/sourcemap";

  Map<String, Boolean> namespaces = new TreeMap<>();   // path -> is class

  String nsPath(Namespace ns) {
    if (ns == null || ns.isGlobal()) return "";
    String parent = nsPath(ns.getParentNamespace());
    String path = parent.isEmpty() ? ns.getName() : parent + "::" + ns.getName();
    namespaces.put(path, ns.getSymbol().getSymbolType() == SymbolType.CLASS);
    return path;
  }

  String typePath(DataType d) { return d == null ? null : d.getDataTypePath().getPath(); }

  boolean isProgramType(DataType d) {
    if (d == null) return false;
    DataType base = d;
    while (true) {
      if (base instanceof Pointer) base = ((Pointer) base).getDataType();
      else if (base instanceof Array) base = ((Array) base).getDataType();
      else if (base instanceof TypeDef) break;
      else break;
      if (base == null) return false;
    }
    SourceArchive sa = base.getSourceArchive();
    return !(base instanceof BuiltInDataType) && sa != null && sa.getArchiveType() == ArchiveType.PROGRAM;
  }

  public void run() throws Exception {
    String prog = currentProgram.getName();
    String[] args = getScriptArgs();
    File dir = new File(args.length > 0 ? args[0] : DEFAULT_ROOT, prog);
    dir.mkdirs();
    long t0 = System.currentTimeMillis();

    // ---- types.gdt
    File gdt = new File(dir, "types.gdt");
    if (gdt.exists()) gdt.delete();
    FileDataTypeManager arch = FileDataTypeManager.createFileArchive(gdt);
    int nTypes = 0;
    int tx = arch.startTransaction("export");
    try {
      Iterator<DataType> it = currentProgram.getDataTypeManager().getAllDataTypes();
      while (it.hasNext()) {
        DataType d = it.next();
        if (d instanceof BuiltInDataType) continue;
        SourceArchive sa = d.getSourceArchive();
        if (sa == null || sa.getArchiveType() != ArchiveType.PROGRAM) continue;
        arch.resolve(d, DataTypeConflictHandler.REPLACE_HANDLER);
        nTypes++;
      }
    } finally {
      arch.endTransaction(tx, true);
    }
    arch.save();
    arch.close();

    // ---- functions
    JsonArray functions = new JsonArray();
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      if (f.isThunk() || f.isExternal()) continue;
      Symbol s = f.getSymbol();
      if (s.getSource() == SourceType.DEFAULT && !f.getSignatureSource().isHigherPriorityThan(SourceType.ANALYSIS)) continue;
      JsonObject o = new JsonObject();
      o.addProperty("entry", f.getEntryPoint().toString());
      o.addProperty("name", s.getSource() == SourceType.DEFAULT ? null : f.getName());
      o.addProperty("ns", nsPath(f.getParentNamespace()));
      o.addProperty("source", s.getSource().toString());
      o.addProperty("sig", f.getPrototypeString(true, false));
      o.addProperty("sig_source", f.getSignatureSource().toString());
      o.addProperty("cc", f.getCallingConventionName());
      o.addProperty("ret", typePath(f.getReturn().getFormalDataType()));   // forced-indirect: the declared type
      JsonArray ps = new JsonArray();
      for (Parameter p : f.getParameters()) {
        if (p.isAutoParameter()) continue;
        JsonObject po = new JsonObject();
        po.addProperty("name", p.getName());
        po.addProperty("type", typePath(p.getFormalDataType()));
        if (f.hasCustomVariableStorage()) po.addProperty("storage", p.getVariableStorage().getSerializationString());
        ps.add(po);
      }
      o.add("params", ps);
      if (f.hasCustomVariableStorage()) {
        o.addProperty("custom_storage", true);
        o.addProperty("ret_storage", f.getReturn().getVariableStorage().getSerializationString());
      }
      JsonArray body = new JsonArray();   // [start, end) per address range, hex: code only (switch tables are data)
      for (AddressRange r : f.getBody().getAddressRanges()) {
        JsonArray pair = new JsonArray();
        pair.add(r.getMinAddress().toString());
        pair.add(r.getMaxAddress().add(1).toString());
        body.add(pair);
      }
      o.add("body", body);
      if (f.hasVarArgs()) o.addProperty("varargs", true);
      if (f.hasNoReturn()) o.addProperty("noreturn", true);
      JsonArray tags = new JsonArray();
      for (FunctionTag t : f.getTags()) tags.add(t.getName());
      if (tags.size() > 0) o.add("tags", tags);
      functions.add(o);
    }

    // ---- labels (our own names, not function entries)
    JsonArray labels = new JsonArray();
    SymbolTable st = currentProgram.getSymbolTable();
    SymbolIterator si = st.getAllSymbols(true);
    while (si.hasNext()) {
      Symbol s = si.next();
      if (s.getSymbolType() != SymbolType.LABEL || s.isExternal() || s.getSource() != SourceType.USER_DEFINED) continue;
      JsonObject o = new JsonObject();
      o.addProperty("addr", s.getAddress().toString());
      o.addProperty("name", s.getName());
      o.addProperty("ns", nsPath(s.getParentNamespace()));
      if (s.isPrimary()) o.addProperty("primary", true);
      labels.add(o);
    }

    // ---- typed globals: data that carries one of our names or one of the program's own types
    JsonArray data = new JsonArray();
    DataIterator di = currentProgram.getListing().getDefinedData(true);
    while (di.hasNext()) {
      Data d = di.next();
      Symbol ps = d.getPrimarySymbol();
      boolean named = ps != null && ps.getSource() == SourceType.USER_DEFINED;
      if (!named && !isProgramType(d.getDataType())) continue;
      JsonObject o = new JsonObject();
      o.addProperty("addr", d.getAddress().toString());
      o.addProperty("type", typePath(d.getDataType()));
      o.addProperty("len", d.getLength());
      data.add(o);
    }

    // ---- comments
    JsonArray comments = new JsonArray();
    Listing li = currentProgram.getListing();
    String[] kindNames = {"eol", "pre", "post", "plate", "repeatable"};
    int[] kinds = {CodeUnit.EOL_COMMENT, CodeUnit.PRE_COMMENT, CodeUnit.POST_COMMENT, CodeUnit.PLATE_COMMENT,
                   CodeUnit.REPEATABLE_COMMENT};
    for (int i = 0; i < kinds.length; i++) {
      AddressIterator it = li.getCommentAddressIterator(kinds[i], currentProgram.getMemory(), true);
      while (it.hasNext()) {
        Address a = it.next();
        String c = li.getComment(kinds[i], a);
        if (c == null) continue;
        JsonObject o = new JsonObject();
        o.addProperty("addr", a.toString());
        o.addProperty("kind", kindNames[i]);
        o.addProperty("text", c);
        comments.add(o);
      }
    }

    // ---- namespaces (filled while walking the symbols above)
    JsonArray nss = new JsonArray();
    for (Map.Entry<String, Boolean> e : namespaces.entrySet()) {
      JsonObject o = new JsonObject();
      o.addProperty("path", e.getKey());
      o.addProperty("class", e.getValue());
      nss.add(o);
    }

    JsonObject root = new JsonObject();
    root.addProperty("format", "sadk-sourcemap/1");
    JsonObject p = new JsonObject();
    p.addProperty("name", prog);
    p.addProperty("md5", currentProgram.getExecutableMD5());
    p.addProperty("image_base", currentProgram.getImageBase().toString());
    p.addProperty("language", currentProgram.getLanguageID().getIdAsString());
    p.addProperty("compiler", currentProgram.getCompilerSpec().getCompilerSpecID().getIdAsString());
    root.add("program", p);
    root.addProperty("exported", new SimpleDateFormat("yyyy-MM-dd").format(new Date()));
    JsonObject counts = new JsonObject();
    counts.addProperty("types", nTypes);
    counts.addProperty("namespaces", nss.size());
    counts.addProperty("functions", functions.size());
    counts.addProperty("labels", labels.size());
    counts.addProperty("data", data.size());
    counts.addProperty("comments", comments.size());
    root.add("counts", counts);
    root.add("namespaces", nss);
    root.add("functions", functions);
    root.add("labels", labels);
    root.add("data", data);
    root.add("comments", comments);

    File out = new File(dir, "sourcemap.json.gz");
    try (Writer w = new OutputStreamWriter(new GZIPOutputStream(new FileOutputStream(out)), "UTF-8")) {
      new GsonBuilder().disableHtmlEscaping().create().toJson(root, w);
    }
    println(String.format("%s: %s in %d ms", prog, counts, System.currentTimeMillis() - t0));
  }
}
