import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Runs *inside* the uvroot isolated root, on ART, against the real APK of the
 * target application.
 *
 * It is started the way every Android process is started, but without going
 * through zygote/AMS:
 *
 *   app_process64 -Djava.class.path=<base.apk>:<harness.zip> /system/bin AppProbe
 *
 * so the process inherits uvroot's virtual root and sees only the mappings.
 * Everything it reports is printed as "PROBE: key=value" lines so the shell
 * suite (cases/65-app-isolation.sh) can assert on it.
 */
public class AppProbe {
    static final String PKG  = System.getProperty("app.pkg", "com.matecal.ceiling");
    static final String DATA = System.getProperty("app.data", "/data/data/" + PKG);
    static final String LIBS = System.getProperty("app.libs", "/opt/harness/libs");

    static int pass = 0, fail = 0;

    static void p(String k, Object v) { System.out.println("PROBE: " + k + "=" + v); }

    static void check(String name, boolean ok, Object detail) {
        if (ok) { pass++; System.out.println("PROBE-OK   " + name + (detail == null ? "" : " (" + detail + ")")); }
        else    { fail++; System.out.println("PROBE-FAIL " + name + " (" + detail + ")"); }
    }

    static void setDouble(Object o, String f, double v) throws Exception {
        Field fd = o.getClass().getDeclaredField(f); fd.setAccessible(true); fd.setDouble(o, v);
    }
    static void setInt(Object o, String f, int v) throws Exception {
        Field fd = o.getClass().getDeclaredField(f); fd.setAccessible(true); fd.setInt(o, v);
    }
    static void setObj(Object o, String f, Object v) throws Exception {
        Field fd = o.getClass().getDeclaredField(f); fd.setAccessible(true); fd.set(o, v);
    }
    static double getDouble(Object o, String f) throws Exception {
        Field fd = o.getClass().getDeclaredField(f); fd.setAccessible(true); return fd.getDouble(o);
    }
    static int getInt(Object o, String f) throws Exception {
        Field fd = o.getClass().getDeclaredField(f); fd.setAccessible(true); return fd.getInt(o);
    }

    public static void main(String[] args) throws Exception {
        p("pkg", PKG);
        p("vm", System.getProperty("java.vm.name") + " " + System.getProperty("java.vm.version"));
        p("java.home", System.getProperty("java.home"));
        p("cwd", new File(".").getAbsolutePath());
        p("uid", readStatus("Uid"));
        p("gid", readStatus("Gid"));

        // ---- 1. the virtual root -----------------------------------------
        List<String> top = new ArrayList<String>();
        File[] tops = new File("/").listFiles();
        if (tops != null) for (File f : tops) top.add(f.getName());
        Collections.sort(top);
        p("root.entries", top);

        for (String path : new String[]{"/sdcard", "/storage", "/data/media", "/etc", "/home",
                                        "/tmp", "/root", "/data/data/com.termux",
                                        "/data/data/com.android.settings"}) {
            check("hidden:" + path, !new File(path).exists(), null);
        }
        for (String path : new String[]{"/system/bin/app_process64", "/data/app/" + PKG + "/base.apk",
                                        "/data/data/" + PKG}) {
            check("visible:" + path, new File(path).exists(), null);
        }

        // ---- 2. the app's data (the mapped shadow) ------------------------
        File files = new File(DATA, "files");
        p("data.files", list(files));
        File rooms = new File(files, "rooms.json");
        String body = rooms.exists() ? new String(Files.readAllBytes(rooms.toPath()), "UTF-8") : "<missing>";
        p("rooms.json.bytes", body.length());
        p("rooms.json.head", body.length() > 120 ? body.substring(0, 120) : body);

        // ---- 3. a write must land in the mapped shadow --------------------
        File marker = new File(files, "uvroot-probe-marker.txt");
        FileOutputStream out = new FileOutputStream(marker, false);
        out.write("written-inside-the-uvroot-guest\n".getBytes("UTF-8"));
        out.close();
        check("shadow.write", marker.exists() && marker.length() > 0, marker.length() + " bytes");
        p("marker.path", marker.getAbsolutePath());

        // ---- 4. the application's own engine ------------------------------
        Class<?> matC = Class.forName("com.matecal.ceiling.CeilingCalc$Materials");
        Object mat = matC.getDeclaredConstructor().newInstance();
        setDouble(mat, "panelW", 0.6); setDouble(mat, "panelL", 0.6);
        setDouble(mat, "mainSpacing", 1.0); setDouble(mat, "crossSpacing", 0.6);
        setDouble(mat, "keelStd", 3.0); setDouble(mat, "trimStd", 3.0); setDouble(mat, "waste", 0.05);
        setDouble(mat, "mainEdge", 0.3); setDouble(mat, "hangerSpacing", 1.0);

        Class<?> cc = Class.forName("com.matecal.ceiling.CeilingCalc");
        Method grid = cc.getDeclaredMethod("calcPanelGrid", double.class, double.class, matC);
        grid.setAccessible(true);
        Object gr = grid.invoke(null, 4.8, 3.6, mat);
        p("calcPanelGrid.full", getInt(gr, "full"));
        check("calcPanelGrid(4.8x3.6).full==48", getInt(gr, "full") == 48, getInt(gr, "full"));

        Class<?> rectC = Class.forName("com.matecal.ceiling.CeilingCalc$Rect");
        Object rect = rectC.getDeclaredConstructor(double.class, double.class).newInstance(4.8, 3.6);
        Class<?> roomC = Class.forName("com.matecal.ceiling.CeilingCalc$Room");
        Object room = roomC.getDeclaredConstructor().newInstance();
        setObj(room, "name", "uvroot-isolated-room");
        setObj(room, "dimDesc", "4800x3600");
        List<Object> rects = new ArrayList<Object>();
        rects.add(rect);
        setObj(room, "rects", rects);

        // LayoutVerts.build() bails out with an empty array unless the room also
        // carries its outline; pts is a List<double[]> of (x, y) corners.
        Class<?> polyC = Class.forName("com.matecal.ceiling.CeilingCalc$Poly");
        Object poly = polyC.getDeclaredConstructor().newInstance();
        List<Object> pts = new ArrayList<Object>();
        pts.add(new double[]{0.0, 0.0});
        pts.add(new double[]{4.8, 0.0});
        pts.add(new double[]{4.8, 3.6});
        pts.add(new double[]{0.0, 3.6});
        setObj(poly, "pts", pts);
        setObj(room, "poly", poly);

        Method calcRoom = cc.getDeclaredMethod("calcRoom", roomC, matC);
        calcRoom.setAccessible(true);
        Object rr = calcRoom.invoke(null, room, mat);
        p("calcRoom.areaM2", getDouble(rr, "areaM2"));
        p("calcRoom.panelsOpt", getInt(rr, "panelsOpt"));
        p("calcRoom.mainKeelQty", getInt(rr, "mainKeelQty"));
        p("calcRoom.crossKeelQty", getInt(rr, "crossKeelQty"));
        p("calcRoom.hangerQty", getInt(rr, "hangerQty"));
        p("calcRoom.trimQty", getInt(rr, "trimQty"));
        check("calcRoom produced a result", getDouble(rr, "areaM2") > 0, getDouble(rr, "areaM2"));

        Method calcAll = cc.getDeclaredMethod("calcAll", List.class, matC);
        calcAll.setAccessible(true);
        List<Object> roomsList = new ArrayList<Object>();
        roomsList.add(room);
        Object tr = calcAll.invoke(null, roomsList, mat);
        p("calcAll.totalArea", getDouble(tr, "totalArea"));
        p("calcAll.panelsOpt", getInt(tr, "panelsOpt"));
        p("calcAll.mainMeters", getDouble(tr, "mainMeters"));
        check("calcAll ran the full app pipeline", getDouble(tr, "totalArea") > 0, getDouble(tr, "totalArea"));

        Class<?> lv = Class.forName("com.matecal.ceiling.LayoutVerts");
        Method build = lv.getDeclaredMethod("build", roomC, boolean.class, boolean.class,
                                            boolean.class, double.class, double.class);
        build.setAccessible(true);
        float[] verts = (float[]) build.invoke(null, room, Boolean.TRUE, Boolean.TRUE, Boolean.TRUE, 4.8, 3.6);
        p("LayoutVerts.build.floats", verts == null ? -1 : verts.length);
        check("LayoutVerts.build produced geometry", verts != null && verts.length > 0,
              verts == null ? "null" : verts.length);

        // ---- 5. the application's native libraries ------------------------
        for (String so : new String[]{"libc++_shared.so", "libceilingvk.so"}) {
            try {
                System.load(LIBS + "/" + so);
                check("dlopen:" + so, true, null);
            } catch (Throwable t) {
                check("dlopen:" + so, false, t);
            }
        }

        p("summary.pass", pass);
        p("summary.fail", fail);
    }

    static String readStatus(String key) {
        try {
            for (String line : Files.readAllLines(new File("/proc/self/status").toPath())) {
                if (line.startsWith(key + ":")) return line.substring(key.length() + 1).trim();
            }
        } catch (Throwable t) { /* ignore */ }
        return "?";
    }

    static String list(File d) {
        File[] fs = d.listFiles();
        if (fs == null) return "<unreadable>";
        List<String> n = new ArrayList<String>();
        for (File f : fs) n.add(f.getName() + (f.isDirectory() ? "/" : ""));
        Collections.sort(n);
        return n.toString();
    }
}
