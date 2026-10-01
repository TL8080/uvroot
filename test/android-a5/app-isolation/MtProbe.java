import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Ask MT Manager (bin.mt.plus 2.26.7) itself what it can see.
 *
 * This is not "make MT Manager work", it is a *check on the isolation*: MT
 * Manager is a file manager, so it is exactly the app whose job is to reach
 * /sdcard, /storage, other apps' data.  We run this same probe twice -- once
 * with uvroot and once without -- and compare.
 *
 * The measuring instrument is MT Manager's own native code where possible:
 *
 *   bin.mt.plus.Features   (libmt1.so)  getABI, uid2name, deleteFile, analyze
 *   bin.mt.plus.Features3  (libmt3.so)  startMTIO, read/write/seek/length,
 *                                       readlink, rename, delete ...
 *
 * MtProbe prints "PROBE: key=value" lines; cases/66-mt-isolation.sh diffs the
 * two runs.  -Dmt.mode=<label> only tags the output.
 */
public class MtProbe {
    static final String MODE = System.getProperty("mt.mode", "?");
    static final String LIBS = System.getProperty("mt.libs", "/opt/harness/mtlibs");
    static final String DATA = System.getProperty("mt.data", "/data/data/bin.mt.plus");
    static final String CODE = System.getProperty("mt.code", "");

    /** Paths a file manager cares about, plus the uvroot fiction and the real one. */
    static final String[] PATHS = {
        "/sdcard", "/storage", "/storage/emulated/0", "/mnt/sdcard", "/mnt",
        "/etc", "/home", "/tmp", "/root", "/data/media",
        "/data/data/com.termux", "/data/data/bin.mt.plus",
        "/data/app/bin.mt.plus/base.apk",          // only uvroot can invent this
        "/system/bin/app_process64", "/vendor", "/linkerconfig",
    };

    static void p(String k, Object v) { System.out.println("PROBE: " + k + "=" + v); }

    static Object callSafe(Class<?> c, String name, Class<?>[] types, Object... a) {
        try {
            Method m = c.getDeclaredMethod(name, types);
            m.setAccessible(true);
            return m.invoke(null, a);
        } catch (InvocationTargetException e) { return "EXC:" + e.getCause(); }
        catch (Throwable t) { return "ERR:" + t; }
    }

    public static void main(String[] args) throws Exception {
        p("mode", MODE);
        p("vm", System.getProperty("java.vm.name") + " " + System.getProperty("java.vm.version"));
        p("uid", readStatus("Uid"));
        p("root.entries", list("/"));

        // Give MT's readlink a symlink to resolve, in the mapped data dir.
        File link = new File(DATA, "files/mtlink");
        try {
            Files.deleteIfExists(link.toPath());
            Files.createSymbolicLink(link.toPath(), new File("/system").toPath());
        } catch (Throwable t) { p("symlink.create", "ERR:" + t); }

        // ---- MT Manager's libraries and dex --------------------------------
        for (String so : new String[]{"libmt1.so", "libmt3.so", "libmt2.so",
                                      "libmtprotect.so", "libterm.so"}) {
            try { System.load(LIBS + "/" + so); p("dlopen:" + so, "ok"); }
            catch (Throwable t) { p("dlopen:" + so, "FAIL:" + t); }
        }

        Class<?> feat = null, feat3 = null;
        try { feat = Class.forName("bin.mt.plus.Features");  p("dex.Features", "ok"); }
        catch (Throwable t) { p("dex.Features", "FAIL:" + t); }
        try { feat3 = Class.forName("bin.mt.plus.Features3"); p("dex.Features3", "ok"); }
        catch (Throwable t) { p("dex.Features3", "FAIL:" + t); }

        // ---- MT Manager's own JNI ------------------------------------------
        if (feat != null) {
            p("Features.getABI", callSafe(feat, "getABI", new Class<?>[]{}));
            p("Features.uid2name(10107)", callSafe(feat, "uid2name", new Class[]{int.class}, 10107));
        }

        // ---- what MT Manager can see ---------------------------------------
        for (String path : PATHS) {
            p("exists:" + path, new File(path).exists());
        }
        if (feat3 != null) {
            for (String path : PATHS) {
                p("mt.readlink:" + path, callSafe(feat3, "readlink", new Class[]{String.class}, path));
            }
            p("mt.readlink:" + DATA + "/files/mtlink",
              callSafe(feat3, "readlink", new Class[]{String.class}, DATA + "/files/mtlink"));
            // MT's own I/O layer, on a file that only exists in the mapped dir
            p("mt.startMTIO:" + DATA + "/files/rooms.json",
              callSafe(feat3, "startMTIO", new Class[]{String.class, String.class},
                       DATA + "/files/rooms.json", "r"));
            p("mt.startMTIO:/sdcard/DCIM",
              callSafe(feat3, "startMTIO", new Class[]{String.class, String.class}, "/sdcard/DCIM", "r"));
        }
        if (!CODE.isEmpty()) {
            p("exists:codeDir", new File(CODE).isDirectory());
        }
        p("seen.by.this.probe", "done");
    }

    static String readStatus(String key) {
        try {
            for (String line : Files.readAllLines(new File("/proc/self/status").toPath())) {
                if (line.startsWith(key + ":")) return line.substring(key.length() + 1).trim();
            }
        } catch (Throwable t) { /* ignore */ }
        return "?";
    }

    static String list(String dir) {
        List<String> n = new ArrayList<String>();
        File[] fs = new File(dir).listFiles();
        if (fs != null) for (File f : fs) n.add(f.getName());
        Collections.sort(n);
        return n.toString();
    }
}
