import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * Why a GUI cannot be brought up from inside the uvroot guest.
 *
 * An Android window belongs to an *application process*: one that zygote forked
 * on behalf of ActivityManagerService and that AMS is waiting to hear from.
 * A process uvroot launched is an ordinary process -- it can run the app's dex
 * and native code (see AppProbe) but nobody registered it as an app, so it has
 * no ActivityThread, no Application and no window token.
 *
 * This probe prints what it actually finds, instead of asserting it.
 */
public class WindowProbe {
    static void p(String k, Object v) { System.out.println("PROBE: " + k + "=" + v); }

    public static void main(String[] args) {
        try {
            Method myUid = Class.forName("android.os.Process").getMethod("myUid");
            p("android.uid", myUid.invoke(null));
        } catch (Throwable t) { p("android.uid", "err:" + t); }

        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Method m = at.getDeclaredMethod("currentActivityThread");
            m.setAccessible(true);
            p("ActivityThread.currentActivityThread", m.invoke(null));
            Method m2 = at.getDeclaredMethod("currentApplication");
            m2.setAccessible(true);
            p("ActivityThread.currentApplication", m2.invoke(null));
        } catch (Throwable t) { p("ActivityThread", "err:" + t); }

        try {
            Class<?> wmg = Class.forName("android.view.WindowManagerGlobal");
            Method gws = wmg.getDeclaredMethod("getWindowManagerService");
            gws.setAccessible(true);
            p("WindowManagerService", gws.invoke(null));
            Method gs = wmg.getDeclaredMethod("getWindowSession");
            gs.setAccessible(true);
            try {
                p("WindowSession", gs.invoke(null));
            } catch (InvocationTargetException e) {
                p("WindowSession", "FAILED: " + e.getCause());
            }
        } catch (Throwable t) { p("WindowSession", "err:" + t); }
    }
}
