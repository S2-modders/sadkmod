// Writes <folder>/types.json.gz from <folder>/types.gdt: every type of the archive as plain JSON, for tools that
// cannot read a Ghidra archive (sadkmod's header generator, sadkmod/gen/gen_game_headers.py).
//   structs/unions: size, alignment, packing, fields (offset, length, name, type, bit-field size, comment)
//   enums: size and values · typedefs: base type · function definitions: return, parameters, calling convention
// Type references are Ghidra path names ("/S2CE/CTexture *", "/byte[16]", "/std/string").
// Headless: analyzeHeadless <tmp project dir> tmp -import <any small PE> -noanalysis -deleteProject
//           -scriptPath <repo>/sourcemap -postScript ExportTypesJson.java <repo>/sourcemap/sadk_noav.exe
//@category SAdK
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.data.Enum;
import com.google.gson.*;
import java.io.*;
import java.util.*;
import java.util.zip.GZIPOutputStream;

public class ExportTypesJson extends GhidraScript {
  public void run() throws Exception {
    String[] args = getScriptArgs();
    File dir = args.length > 0 ? new File(args[0]) : askDirectory("Sourcemap folder", "Export");
    FileDataTypeManager arch = FileDataTypeManager.openFileArchive(new File(dir, "types.gdt"), false);
    JsonArray types = new JsonArray();
    try {
      List<DataType> all = new ArrayList<>();
      arch.getAllDataTypes(all);
      all.sort(Comparator.comparing(DataType::getPathName));
      for (DataType d : all) {
        if (d instanceof Pointer || d instanceof Array || d instanceof BuiltInDataType) continue;
        JsonObject o = new JsonObject();
        o.addProperty("path", d.getPathName());
        o.addProperty("size", d.getLength());
        if (d.getDescription() != null && !d.getDescription().isEmpty()) o.addProperty("desc", d.getDescription());
        if (d instanceof Composite) {
          Composite c = (Composite) d;
          o.addProperty("kind", d instanceof Union ? "union" : "struct");
          o.addProperty("align", c.getAlignment());
          o.addProperty("packed", c.isPackingEnabled());
          JsonArray fs = new JsonArray();
          for (DataTypeComponent f : c.getDefinedComponents()) {
            JsonObject fo = new JsonObject();
            fo.addProperty("off", f.getOffset());
            fo.addProperty("len", f.getLength());
            fo.addProperty("name", f.getFieldName());
            DataType ft = f.getDataType();
            if (ft instanceof BitFieldDataType) {
              BitFieldDataType b = (BitFieldDataType) ft;
              fo.addProperty("type", b.getBaseDataType().getPathName());
              fo.addProperty("bits", b.getBitSize());
              fo.addProperty("bit_off", b.getBitOffset());
            } else {
              fo.addProperty("type", ft.getPathName());
            }
            if (f.getComment() != null) fo.addProperty("comment", f.getComment());
            fs.add(fo);
          }
          o.add("fields", fs);
        } else if (d instanceof Enum) {
          Enum e = (Enum) d;
          o.addProperty("kind", "enum");
          JsonObject vs = new JsonObject();
          for (String n : e.getNames()) vs.addProperty(n, e.getValue(n));
          o.add("values", vs);
        } else if (d instanceof TypeDef) {
          o.addProperty("kind", "typedef");
          o.addProperty("base", ((TypeDef) d).getDataType().getPathName());
        } else if (d instanceof FunctionDefinition) {
          FunctionDefinition f = (FunctionDefinition) d;
          o.addProperty("kind", "funcdef");
          o.addProperty("cc", f.getCallingConventionName());
          o.addProperty("ret", f.getReturnType().getPathName());
          JsonArray ps = new JsonArray();
          for (ParameterDefinition p : f.getArguments()) {
            JsonObject po = new JsonObject();
            po.addProperty("name", p.getName());
            po.addProperty("type", p.getDataType().getPathName());
            ps.add(po);
          }
          o.add("params", ps);
          if (f.hasVarArgs()) o.addProperty("varargs", true);
          if (f.getComment() != null) o.addProperty("comment", f.getComment());
        } else {
          o.addProperty("kind", "other");
        }
        types.add(o);
      }
    } finally {
      arch.close();
    }
    JsonObject root = new JsonObject();
    root.addProperty("format", "sadk-types/1");
    root.add("types", types);
    try (Writer w = new OutputStreamWriter(new GZIPOutputStream(new FileOutputStream(new File(dir, "types.json.gz"))), "UTF-8")) {
      new GsonBuilder().disableHtmlEscaping().create().toJson(root, w);
    }
    println("types.json.gz: " + types.size() + " types -> " + dir);
  }
}
