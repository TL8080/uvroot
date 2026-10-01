import android.content.Intent;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * Ask ActivityTaskManagerService to start the app's launcher activity, from the
 * isolated process, owning up to our own identity instead of the shell's.
 *
 * /system/bin/am cannot be used for this: on Android 13 it is a wrapper around
 * `cmd activity`, and ActivityManagerShellCommand hard-codes the caller package
 * to com.android.shell, which uid 10117 does not own.  Talking to
 * IActivityTaskManager directly with callingPackage=com.termux passes that
 * check -- but AMS still applies the background-activity-start policy to us.
 *
 * The host-side logcat is where AMS's verdict shows up; this probe only prints
 * the binder result.
 */
public class AmStart {
    static void p(String k, Object v) { System.out.println("PROBE: " + k + "=" + v); }

    public static void main(String[] args) throws Exception {
        String pkg = System.getProperty("app.pkg", "com.matecal.ceiling");
        String caller = System.getProperty("app.caller", "com.termux");

        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.setClassName(pkg, pkg + ".MainActivity");
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);

        Class<?> atmCls = Class.forName("android.app.ActivityTaskManager");
        Object atm = atmCls.getMethod("getService").invoke(null);
        p("ATM", atm.getClass().getName());

        Method target = null;
        for (Method m : atm.getClass().getMethods()) {
            if (!m.getName().equals("startActivityAsUser")) continue;
            Class<?>[] t = m.getParameterTypes();
            if (t.length >= 12 && t[0].getName().endsWith("IApplicationThread")
                && t[1] == String.class && t[3] == Intent.class) {
                if (target == null || t.length > target.getParameterTypes().length) target = m;
            }
        }
        if (target == null) { p("result", "no suitable startActivityAsUser"); return; }

        Class<?>[] t = target.getParameterTypes();
        Object[] a = new Object[t.length];
        for (int i = 0; i < t.length; i++) {
            if (t[i] == int.class) a[i] = 0;
            else if (t[i] == boolean.class) a[i] = Boolean.FALSE;
            else a[i] = null;
        }
        a[1] = caller;
        a[3] = intent;
        p("callingPackage", caller);
        try {
            p("result", target.invoke(atm, a));
        } catch (InvocationTargetException e) {
            p("result", "FAILED: " + e.getCause());
        }
    }
}
