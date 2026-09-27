import com.luka.carplay.cluster.ClusterPlatform;
import com.luka.carplay.cluster.MostPresentation;
import com.luka.carplay.core.CarPlayApp;
import com.luka.carplay.core.FrameworkRef;
import com.luka.carplay.core.ScreenModule;
import com.luka.carplay.framework.Log;
import com.luka.carplay.rgd.BAPBridge;
import com.luka.carplay.rgd.ManeuverMapper;
import com.luka.carplay.rgd.RouteGuidance;
import de.audi.atip.base.IFrameworkAccess;
import de.audi.atip.hmi.HMIService;
import de.audi.atip.hmi.event.ATIPEvent;
import de.audi.atip.hmi.event.EventDispatcher;
import de.audi.atip.hmi.view.IDisplayListener;
import de.audi.atip.log.LogChannel;
import de.audi.atip.metrics.Distance;
import de.audi.tghu.fwhmi.DisplayManager;
import de.audi.tghu.fwhmi.DisplayManagerMIB2High;
import de.audi.tghu.navi.app.cluster.BAPDistanceFormatter;
import de.audi.tghu.navi.app.cluster.CarPlayKOMOService;
import de.audi.tghu.navi.app.cluster.ClusterService;
import de.audi.tghu.navi.app.cluster.KOMOCaller;
import de.audi.tghu.navi.app.cluster.KOMOService;
import de.audi.tghu.navi.app.cluster.KOMOTime;
import java.io.File;
import java.io.FileInputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import org.dsi.ifc.displaymanagement.DSIDisplayManagement;
import sun.misc.Unsafe;

/**
 * CarPlay route guidance on a MOST cluster.
 *
 *  - the real patched DisplayManagerMIB2High over the real stock DisplayManager core, with a
 *    recording DSIDisplayManagement that confirms switches from its own thread, and an HMI
 *    event thread: what reaches the display service, on which thread, and what stock's
 *    listener is told;
 *  - ScreenModule activation per cluster coding;
 *  - the real stock KOMOService/KOMOCaller chain under CarPlayKOMOService, and the stock
 *    distance/time formatters behind ClusterService.publishCarPlayKomoGuidance;
 *  - BAPBridge claim / publish / release.
 */
public final class MostArrowsTest {
    private static int checks;
    private static final Unsafe U = unsafe();

    static void check(boolean ok, String why) {
        checks++;
        if (!ok) throw new AssertionError(why);
    }

    static Unsafe unsafe() {
        try {
            Field f = Unsafe.class.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            return (Unsafe) f.get(null);
        } catch (Exception e) { throw new AssertionError(e); }
    }

    static Object alloc(Class<?> type) throws Exception { return U.allocateInstance(type); }

    static Field field(Class<?> type, String name) throws Exception {
        for (Class<?> c = type; c != null; c = c.getSuperclass()) {
            try { Field f = c.getDeclaredField(name); f.setAccessible(true); return f; }
            catch (NoSuchFieldException e) { }
        }
        throw new NoSuchFieldException(name);
    }

    static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }

    static void setStatic(Class<?> type, String name, Object value) throws Exception {
        field(type, name).set(null, value);
    }

    static LogChannel silentLog() throws Exception {
        Constructor<?> ctor = Class.forName("com.luka.carplay.rgd.BAPBridge$SilentLogChannel").getDeclaredConstructor();
        ctor.setAccessible(true);
        return (LogChannel) ctor.newInstance();
    }

    static IFrameworkAccess framework(final int kombiMapMode) {
        return (IFrameworkAccess) Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{IFrameworkAccess.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) {
                    if (m.getName().equals("getSysConst")) {
                        int id = ((Integer) a[0]).intValue();
                        if (id == 541) return Integer.valueOf(kombiMapMode);
                        if (id == 4383 || id == 4388) return Integer.valueOf(kombiMapMode > 0 ? 1 : 0);
                        return Integer.valueOf(0);
                    }
                    if (m.getReturnType() == Long.TYPE) return Long.valueOf(0L);
                    if (m.getReturnType() == Integer.TYPE) return Integer.valueOf(0);
                    if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
                    return null;
                }
            });
    }

    static void installCarPlayFramework(IFrameworkAccess fw) throws Exception {
        FrameworkRef ref = new FrameworkRef(null);
        set(ref, "fw", fw);
        setStatic(CarPlayApp.class, "fwRef", ref);
    }

    /* ---------------- DisplayManager rig ---------------- */

    /** The HMI event thread: one queue, events dispatched in order, as stock's EventDispatcher. */
    static final class Hmi implements InvocationHandler {
        final LinkedBlockingQueue<Object> queue = new LinkedBlockingQueue<Object>();
        final Thread thread;
        volatile Throwable failure;
        final java.util.concurrent.atomic.AtomicInteger posted = new java.util.concurrent.atomic.AtomicInteger();

        Hmi() {
            thread = new Thread(new Runnable() {
                public void run() {
                    while (true) {
                        Object e;
                        try { e = queue.take(); } catch (InterruptedException x) { return; }
                        try {
                            if (e instanceof Runnable) ((Runnable) e).run();
                            else ((ATIPEvent) e).dispatch();
                        } catch (Throwable t) {
                            if (failure == null) failure = t;
                        }
                    }
                }
            }, "hmi");
            thread.setDaemon(true);
            thread.start();
        }

        public Object invoke(Object p, Method m, Object[] a) {
            if (m.getName().equals("postEvent")) { posted.incrementAndGet(); queue.add(a[0]); return null; }
            if (m.getName().equals("isDispatchThread")) return Boolean.valueOf(Thread.currentThread() == thread);
            if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
            return null;
        }

        /** Run on the HMI thread and wait: how stock widgets issue their terminal-1 switches. */
        void run(final Runnable work) throws Exception {
            final CountDownLatch done = new CountDownLatch(1);
            queue.add(new Runnable() { public void run() { try { work.run(); } finally { done.countDown(); } } });
            if (!done.await(5, TimeUnit.SECONDS)) throw new AssertionError("HMI thread stalled");
            if (failure != null) throw new AssertionError(failure);
        }

        /** Everything posted so far has been dispatched. */
        void idle() throws Exception {
            run(new Runnable() { public void run() { } });
        }
    }

    static IFrameworkAccess framework(final int kombiMapMode, final Hmi hmi) {
        final Object dispatcher = Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{EventDispatcher.class}, hmi);
        final Object hmiService = Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{HMIService.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) {
                    if (m.getName().equals("getEventDispatcher")) return dispatcher;
                    if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
                    if (m.getReturnType() == Integer.TYPE) return Integer.valueOf(0);
                    return null;
                }
            });
        final IFrameworkAccess base = framework(kombiMapMode);
        return (IFrameworkAccess) Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{IFrameworkAccess.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) throws Throwable {
                    if (m.getName().equals("getHMIService")) return hmiService;
                    if (m.getName().equals("isFrontMU")) return Boolean.TRUE;   /* internal terminal 4 <-> cluster 1 */
                    return m.invoke(base, a);
                }
            });
    }

    /** A display service that answers like the car's: context confirmations and extents arrive
     *  on its own thread (DSIDisplayListener), extents are cached via an HMI RunnableEvent. */
    static final class Dsi implements InvocationHandler {
        final List<String> calls = Collections.synchronizedList(new ArrayList<String>());
        final List<String> offHmi = Collections.synchronizedList(new ArrayList<String>());
        final Map<Integer, int[]> extentsAnswers = Collections.synchronizedMap(new HashMap<Integer, int[]>());
        volatile long answerDelayMs = 5L;     /* < the DM's 200 ms wait: confirmed in time */
        volatile boolean failRateQuery;
        Hmi hmi;
        DisplayManagerMIB2High dm;

        public Object invoke(Object p, Method m, Object[] a) {
            String n = m.getName();
            String entry = null;
            if (n.equals("switchContext")) {
                entry = "switch " + a[0] + " term " + a[1];
                answerContext(((Integer) a[0]).intValue(), ((Integer) a[1]).intValue(), ((Integer) a[2]).intValue());
            } else if (n.equals("setCropping")) {
                entry = "crop " + a[1] + " src(" + a[2] + "," + a[3] + " " + a[4] + "x" + a[5]
                    + ") dst(" + a[6] + "," + a[7] + " " + a[8] + "x" + a[9] + ")";
            } else if (n.equals("setOpacity")) {
                entry = "opacity " + a[0] + "=" + a[2];
            } else if (n.equals("getExtents")) {
                entry = "getExtents " + a[0];
                answerExtents(((Integer) a[0]).intValue());
            } else if (n.equals("setUpdateRate")) {
                entry = "setUpdateRate " + a[0] + " " + a[1];
            } else if (n.equals("getUpdateRate")) {
                if (failRateQuery) throw new IllegalStateException("display service gone");
                entry = "getUpdateRate " + a[0];
            }
            if (entry != null) {
                calls.add(entry);
                if (Thread.currentThread() != hmi.thread) offHmi.add(entry + " on " + Thread.currentThread().getName());
            }
            return null;
        }

        private void answerContext(final int ctx, final int internalTerminal, final int session) {
            final long delay = answerDelayMs;
            later(delay, new Runnable() {
                public void run() {
                    try {
                        Method confirm = DisplayManager.class.getDeclaredMethod("setActiveContext", Integer.TYPE, Integer.TYPE, Integer.TYPE);
                        confirm.setAccessible(true);
                        confirm.invoke(dm, Integer.valueOf(ctx), Integer.valueOf(internalTerminal), Integer.valueOf(session));
                    } catch (Exception e) { throw new RuntimeException(e); }
                }
            });
        }

        private void answerExtents(final int displayable) {
            final int[] answer = extentsAnswers.get(Integer.valueOf(displayable));
            if (answer == null) return;
            later(30L, new Runnable() {
                public void run() {
                    hmi.queue.add(new de.audi.atip.hmi.event.RunnableEvent(false, new Runnable() {
                        public void run() {
                            try {
                                @SuppressWarnings("unchecked")
                                Map<Integer, int[]> cache = (Map<Integer, int[]>) field(DisplayManager.class, "displayableExtents").get(dm);
                                cache.put(Integer.valueOf(displayable), new int[]{answer[0], answer[1]});
                            } catch (Exception e) { throw new RuntimeException(e); }
                        }
                    }));
                }
            });
        }

        private static void later(final long delayMs, final Runnable work) {
            Thread t = new Thread(new Runnable() {
                public void run() {
                    try { Thread.sleep(delayMs); } catch (InterruptedException e) { return; }
                    work.run();
                }
            }, "dsi");
            t.setDaemon(true);
            t.start();
        }

        List<String> drain() {
            synchronized (calls) { List<String> out = new ArrayList<String>(calls); calls.clear(); return out; }
        }
    }

    static final class Listener implements IDisplayListener {
        final List<String> seen = Collections.synchronizedList(new ArrayList<String>());
        final List<String> offHmi = Collections.synchronizedList(new ArrayList<String>());
        Hmi hmi;
        public void activeContext(int ctx, int terminal) {
            seen.add(ctx + " term " + terminal);
            if (Thread.currentThread() != hmi.thread) offHmi.add(ctx + " on " + Thread.currentThread().getName());
        }
    }

    /** The real patched DisplayManagerMIB2High over the stock core, bound to MostPresentation
     *  through its own ContextOwner, as its constructor does on the car. */
    static DisplayManagerMIB2High displayManager(Dsi dsi, Hmi hmi, Map<Integer, int[]> extents) throws Exception {
        DisplayManagerMIB2High dm = (DisplayManagerMIB2High) alloc(DisplayManagerMIB2High.class);
        set(dm, "framework", framework(1, hmi));
        set(dm, "log", silentLog());
        set(dm, "dsiDispMgmt", Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{DSIDisplayManagement.class}, dsi));
        set(dm, "initialized", Boolean.TRUE);
        set(dm, "displayListeners", new IDisplayListener[8]);
        set(dm, "sessionIDs", new int[8]);
        set(dm, "waitForContextSwitch", new boolean[8]);
        boolean[] answered = new boolean[8];
        java.util.Arrays.fill(answered, true);
        set(dm, "lastRequestAnswered", answered);
        int[] kdks = new int[8];
        java.util.Arrays.fill(kdks, -1);
        set(dm, "visibleKDKs", kdks);
        set(dm, "displayableExtents", extents);
        /* constructor-only initialisers (the car runs them; allocateInstance does not) */
        set(dm, "lastClusterRequest", Integer.valueOf(-1));
        set(dm, "lastBlockedCarPlayContext", Integer.valueOf(-1));
        dsi.hmi = hmi;
        dsi.dm = dm;
        MostPresentation.bind(dm, realContextOwner(dm));
        return dm;
    }

    /** DisplayManagerMIB2High's constructor binds an anonymous ContextOwner; use that exact class. */
    static MostPresentation.ContextOwner realContextOwner(DisplayManagerMIB2High dm) throws Exception {
        for (int i = 1; i < 10; i++) {
            Class<?> c;
            try { c = Class.forName("de.audi.tghu.fwhmi.DisplayManagerMIB2High$" + i); }
            catch (ClassNotFoundException e) { continue; }
            if (!MostPresentation.ContextOwner.class.isAssignableFrom(c)) continue;
            Constructor<?> ctor = c.getDeclaredConstructors()[0];
            ctor.setAccessible(true);
            return (MostPresentation.ContextOwner) ctor.newInstance(dm);
        }
        throw new AssertionError("DisplayManagerMIB2High has no ContextOwner");
    }

    /** A MOST CarPlay session as ScreenModule.start() leaves it. */
    static void mostSession(boolean connected) throws Exception {
        ClusterPlatform.bind(framework(1));
        setStatic(ScreenModule.class, "platformSupported", Boolean.TRUE);
        setStatic(ScreenModule.class, "connected", Boolean.valueOf(connected));
        setStatic(ScreenModule.class, "clusterContextsOwned", Boolean.FALSE);
        setStatic(ScreenModule.class, "navActive", Boolean.FALSE);
        setStatic(ScreenModule.class, "navHidePending", Boolean.FALSE);
    }

    static void endSession() throws Exception {
        setStatic(ScreenModule.class, "connected", Boolean.FALSE);
        setStatic(ScreenModule.class, "clusterContextsOwned", Boolean.FALSE);
        setStatic(ScreenModule.class, "navActive", Boolean.FALSE);
        setStatic(ScreenModule.class, "navHidePending", Boolean.FALSE);
    }

    static void stockSwitch(final Hmi hmi, final DisplayManagerMIB2High dm, final int ctx, final int terminal,
                            final IDisplayListener listener) throws Exception {
        hmi.run(new Runnable() { public void run() { dm.switchContext(ctx, terminal, listener); } });
    }

    static void release(Hmi hmi) throws Exception {
        MostPresentation.setActive(false);
        hmi.idle();
    }

    static boolean has(List<String> calls, String entry) { return calls.contains(entry); }

    static boolean hasPrefix(List<String> calls, String prefix) {
        for (int i = 0; i < calls.size(); i++) if (calls.get(i).startsWith(prefix)) return true;
        return false;
    }

    static int count(List<String> calls, String prefix) {
        int n = 0;
        for (int i = 0; i < calls.size(); i++) if (calls.get(i).startsWith(prefix)) n++;
        return n;
    }

    /** The Log lines ("[CP/W][Tag] message") written since Log.w("Test", marker + " start"),
     *  once the writer thread has written the end marker this call adds. */
    static List<String> logSince(String marker) throws Exception {
        Log.w("Test", marker + " end");
        for (int attempt = 0; attempt < 500; attempt++) {
            StringBuilder text = new StringBuilder();
            for (String path : new String[]{"/tmp/carplay_java.log.1", "/tmp/carplay_java.log"}) {
                File f = new File(path);
                if (f.exists()) text.append(new String(java.nio.file.Files.readAllBytes(f.toPath()), "UTF-8"));
            }
            int start = text.lastIndexOf("[Test] " + marker + " start");
            int end = text.lastIndexOf("[Test] " + marker + " end");
            if (start >= 0 && end > start) {
                List<String> lines = new ArrayList<String>();
                for (String line : text.substring(start, end).split("\n")) {
                    if (line.indexOf("[CP/") >= 0) lines.add(line.substring(line.indexOf("[CP/")));
                }
                return lines;
            }
            Thread.sleep(10);
        }
        throw new AssertionError("log markers for " + marker + " never written");
    }

    static int countContaining(List<String> lines, String text) {
        int n = 0;
        for (int i = 0; i < lines.size(); i++) if (lines.get(i).indexOf(text) >= 0) n++;
        return n;
    }

    /* MostPresentation's request to maneuver_render (protocol.h CR_MOST_OUTPUT_PATH) goes to a
     * scratch file on the PC. */
    static File outputRequestFile;

    static void freshOutputRequest() throws Exception {
        File dir = new File(System.getProperty("java.io.tmpdir"), "most-output-" + System.nanoTime());
        if (!dir.mkdirs()) throw new AssertionError("cannot create " + dir);
        dir.deleteOnExit();
        outputRequestFile = new File(dir, "carplay_most_output");
        outputRequestFile.deleteOnExit();
        setStatic(MostPresentation.class, "outputRequestPath", outputRequestFile.getPath());
        /* An instant renderer: it presents whatever was requested (the gate has its own test). */
        setStatic(MostPresentation.class, "outputReadyPath", outputRequestFile.getPath());
        setStatic(MostPresentation.class, "lastOutputRequest", null);
        setStatic(MostPresentation.class, "outputWritten", Boolean.FALSE);
    }

    static void write(File f, String text) throws Exception {
        java.io.FileOutputStream out = new java.io.FileOutputStream(f);
        try { out.write(text.getBytes("US-ASCII")); } finally { out.close(); }
    }

    /** ctx 81 is composed, and 98 cropped, only once the renderer reports the requested window;
     *  a new size while composed hands the view back to stock 73 until it does (a real context
     *  switch, re-pointing the encoder); an unconfirmed size is composed after the wait, once. */
    static void composesOnlyOnceRendererPresents() throws Exception {
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        int[] kdk = new int[]{400, 220};
        extents.put(Integer.valueOf(20), kdk);
        DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        mostSession(true);
        release(hmi);
        freshOutputRequest();
        File ready = new File(outputRequestFile.getParentFile(), "carplay_most_output_ready");
        setStatic(MostPresentation.class, "outputReadyPath", ready.getPath());
        write(ready, "0328 0181\n");                    /* the renderer's boot-time window */
        stockSwitch(hmi, dm, 73, 1, null);
        dsi.drain();

        MostPresentation.setActive(true);
        hmi.idle();
        List<String> c = dsi.drain();
        check(!MostPresentation.isActive() && !has(c, "switch 81 term 4") && !hasPrefix(c, "crop"),
            "not composed or cropped before the renderer presents 400x220: " + c);
        check("0400 0220\n".equals(outputRequest()), "renderer asked for 400x220: " + outputRequest());
        check(field(MostPresentation.class, "waitingOutputRequest").get(null) != null,
            "ready waiter started");
        final CountDownLatch hmiBlocked = new CountDownLatch(1);
        final CountDownLatch resumeHmi = new CountDownLatch(1);
        hmi.queue.add(new Runnable() {
            public void run() {
                hmiBlocked.countDown();
                try { resumeHmi.await(5L, TimeUnit.SECONDS); } catch (InterruptedException e) { }
            }
        });
        check(hmiBlocked.await(1L, TimeUnit.SECONDS), "HMI barrier started");
        write(ready, "0400 0220\n");                  /* worker sees it before HMI does */
        long readyDeadline = System.currentTimeMillis() + 1000L;
        while (field(MostPresentation.class, "waitingOutputRequest").get(null) != null
               && System.currentTimeMillis() < readyDeadline) Thread.sleep(10L);
        check(field(MostPresentation.class, "waitingOutputRequest").get(null) == null,
            "ready worker retired before its HMI callback");
        write(ready, "0400 022");                       /* marker disappears before HMI reads */
        resumeHmi.countDown();
        hmi.idle();
        check(!MostPresentation.isActive(), "a torn marker after the worker wakes is not confirmation");
        write(ready, "0400 0220\n");
        check(waitFor(dsi, "switch 81 term 4", 1000L),
            "a replacement waiter composes once the renderer confirms: " + dsi.calls);
        hmi.idle();
        c = dsi.drain();
        check(has(c, "crop 98 src(0,0 400x220) dst(0,0 400x220)") && MostPresentation.isActive(),
            "cropped at the confirmed size: " + c);

        kdk[0] = 656;                                   /* stock now reports another size */
        kdk[1] = 360;
        hmi.run(new Runnable() { public void run() { MostPresentation.onCarPlayContextApplied(); } });
        hmi.idle();
        hmi.idle();
        c = dsi.drain();
        check(!MostPresentation.isActive() && has(c, "switch 73 term 4") && !has(c, "crop 98 src(0,0 656x360) dst(0,0 656x360)"),
            "new size while composed: stock 73 back, no crop past 98's buffer: " + c);
        check("0656 0360\n".equals(outputRequest()), "renderer asked for 656x360: " + outputRequest());
        write(ready, "0656 0360\n");
        check(waitFor(dsi, "switch 81 term 4", 1000L), "re-composed (a real 73 -> 81 switch) once resized: " + dsi.calls);
        hmi.idle();
        c = dsi.drain();
        check(has(c, "crop 98 src(0,0 656x360) dst(0,0 656x360)"), "cropped at the new size: " + c);

        kdk[0] = 700;
        kdk[1] = 380;
        write(ready, "0700 0380\n");                  /* fast renderer confirmation */
        hmi.run(new Runnable() { public void run() { MostPresentation.onCarPlayContextApplied(); } });
        hmi.idle();
        c = dsi.drain();
        check(has(c, "switch 73 term 4") && has(c, "switch 81 term 4")
              && has(c, "crop 98 src(0,0 700x380) dst(0,0 700x380)"),
            "a confirmed new size still re-points the encoder through 73 -> 81: " + c);
        release(hmi);
        dsi.drain();

        kdk[0] = 800;                                   /* a renderer that never confirms */
        kdk[1] = 252;
        MostPresentation.setActive(true);
        hmi.idle();
        check(!MostPresentation.isActive(), "unconfirmed: waiting");
        check(waitFor(dsi, "switch 81 term 4", MostPresentation.READY_WAIT_MS + 1500L),
            "composed anyway after the wait: " + dsi.calls);
        hmi.run(new Runnable() { public void run() { MostPresentation.onCarPlayContextApplied(); } });
        Thread.sleep(300L);
        hmi.idle();
        c = dsi.drain();
        check(MostPresentation.isActive() && count(c, "switch 81 term 4") == 1 && !has(c, "switch 73 term 4"),
            "composed anyway stays composed (no hand-back loop): " + c);
        release(hmi);

        write(ready, "0328 0181\n");
        MostPresentation.setActive(true);
        hmi.idle();
        check(!MostPresentation.isActive(), "another route waits for the renderer again");
        release(hmi);                                   /* route ends before confirmation */
        dsi.drain();
        write(ready, "0800 0252\n");
        Thread.sleep(300L);
        hmi.idle();
        c = dsi.drain();
        check(!MostPresentation.isActive() && !has(c, "switch 81 term 4"),
            "late confirmation after route end cannot compose the maneuver: " + c);
        check(dsi.offHmi.isEmpty(), "display service touched on the HMI thread: " + dsi.offHmi);
        endSession();
        freshOutputRequest();
    }


    /** Readiness edges: a report cannot confirm a request that was never written; extents the
     *  renderer would refuse fall back to KVS_Most; a route stop/start before the HMI thread
     *  runs still gets a live waiter for the new route. */
    static void readinessEdges() throws Exception {
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        int[] kdk = new int[]{400, 220};
        extents.put(Integer.valueOf(20), kdk);
        DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        mostSession(true);
        release(hmi);
        freshOutputRequest();
        File ready = new File(outputRequestFile.getParentFile(), "carplay_most_output_ready");
        setStatic(MostPresentation.class, "outputReadyPath", ready.getPath());
        stockSwitch(hmi, dm, 73, 1, null);              /* stock's arrows view is up */

        File missing = new File(outputRequestFile.getParentFile(), "absent" + File.separator + "carplay_most_output");
        setStatic(MostPresentation.class, "outputRequestPath", missing.getPath());
        write(ready, "0400 0220\n");                    /* a report left over from earlier */
        dsi.drain();
        MostPresentation.setActive(true);
        hmi.idle();
        Thread.sleep(300L);
        hmi.idle();
        check(!MostPresentation.isActive() && !has(dsi.drain(), "switch 81 term 4"),
            "a report cannot confirm a request that was never written");
        check(missing.getParentFile().mkdirs(), "request directory created");
        check(waitFor(dsi, "switch 81 term 4", 1000L), "composed once the request is written and reported: " + dsi.calls);
        check("0400 0220\n".equals(read(missing)), "request written on retry: " + read(missing));
        release(hmi);
        dsi.drain();
        missing.delete();
        missing.getParentFile().delete();
        setStatic(MostPresentation.class, "outputRequestPath", outputRequestFile.getPath());

        kdk[0] = 4000;                                  /* beyond protocol.h CR_OUTPUT_MAX */
        kdk[1] = 252;
        write(ready, "0800 0252\n");
        MostPresentation.setActive(true);
        hmi.idle();
        List<String> c = dsi.drain();
        check("0800 0252\n".equals(outputRequest()) && has(c, "crop 98 src(0,0 800x252) dst(0,0 800x252)"),
            "extents the renderer would refuse fall back to KVS_Most 800x252: " + outputRequest() + " " + c);
        check(MostPresentation.describe().indexOf("stock KDK 4000x252 outside the renderer's 64-2048 range") >= 0,
            "out-of-range extents are named as such, not as unknown: " + MostPresentation.describe());
        release(hmi);
        dsi.drain();

        kdk[0] = 656;
        kdk[1] = 360;
        write(ready, "0328 0181\n");
        MostPresentation.setActive(true);
        hmi.idle();
        check(!MostPresentation.isActive(), "restart: first route waiting");
        MostPresentation.setActive(false);              /* stop and start before the HMI runs */
        MostPresentation.setActive(true);
        hmi.idle();
        write(ready, "0656 0360\n");
        check(waitFor(dsi, "switch 81 term 4", 1000L), "the new route's waiter composes once confirmed: " + dsi.calls);
        release(hmi);
        check(dsi.offHmi.isEmpty(), "display service touched on the HMI thread: " + dsi.offHmi);
        endSession();
        freshOutputRequest();
    }

    /** The renderer's report carries a per-window token: a window it replaces on its own
     *  (swap failure, loss recovery, restart, or a resize that lands after the wait) is
     *  followed by a hand-back and a real 73 -> 81 switch.  Waiting neither floods the HMI
     *  thread nor re-requests DSI extents. */
    static void windowTokenAndQuietWait() throws Exception {
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        int[] kdk = new int[]{800, 252};
        extents.put(Integer.valueOf(20), kdk);
        DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        mostSession(true);
        release(hmi);
        freshOutputRequest();
        File ready = new File(outputRequestFile.getParentFile(), "carplay_most_output_ready");
        setStatic(MostPresentation.class, "outputReadyPath", ready.getPath());
        stockSwitch(hmi, dm, 73, 1, null);
        write(ready, "0800 0252 0000000100.0000000001\n");
        dsi.drain();
        MostPresentation.setActive(true);
        hmi.idle();
        check(MostPresentation.isActive() && has(dsi.drain(), "switch 81 term 4"), "composed over window token 1");

        write(ready, "0800 0252 0000000100.0000000002\n");  /* renderer recreated 98 itself */
        check(waitFor(dsi, "switch 73 term 4", 2000L), "a new window token hands the view back: " + dsi.calls);
        check(waitFor(dsi, "switch 81 term 4", 1000L), "and re-composes over the new window (encoder re-pointed): " + dsi.calls);
        hmi.idle();
        List<String> c = dsi.drain();
        check(c.indexOf("switch 73 term 4") < c.lastIndexOf("switch 81 term 4") && MostPresentation.isActive(),
            "73 before 81: a real context switch: " + c);

        check(ready.delete(), "report withdrawn (a recreation in progress)");
        Thread.sleep(1200L);
        hmi.idle();
        check(MostPresentation.isActive() && !has(dsi.drain(), "switch 73 term 4"), "a withdrawn report is waited out");
        write(ready, "0800 0252 0000000200.0000000001\n");  /* a restarted renderer */
        check(waitFor(dsi, "switch 81 term 4", 2500L), "a restarted renderer's window is re-pointed too: " + dsi.calls);
        release(hmi);
        dsi.drain();

        extents.remove(Integer.valueOf(20));                /* cold extents cache while waiting */
        write(ready, "0328 0181 0000000200.0000000002\n");
        int postsBefore = hmi.posted.get();
        MostPresentation.setActive(true);
        hmi.idle();
        Thread.sleep(1500L);
        hmi.idle();
        int postsWaiting = hmi.posted.get() - postsBefore;
        for (int i = 0; i < 3; i++) {                        /* repeated start requests re-converge */
            MostPresentation.setActive(true);
            hmi.idle();
        }
        c = dsi.drain();
        check(count(c, "getExtents 20") == 1, "extents requested once while waiting, not per poll: " + c);
        check(postsWaiting <= 4, "waiting does not flood the HMI thread: " + postsWaiting + " posts in 1.5 s");
        check(!MostPresentation.isActive(), "still waiting for 800x252");
        release(hmi);
        dsi.drain();

        extents.put(Integer.valueOf(20), kdk);              /* a slow renderer: 4.5 s, inside the wait */
        write(ready, "0328 0181 0000000200.0000000002\n");
        MostPresentation.setActive(true);
        hmi.idle();
        Thread.sleep(4500L);
        write(ready, "0800 0252 0000000200.0000000003\n");
        check(waitFor(dsi, "switch 81 term 4", 1000L), "a slow renderer is composed once it reports: " + dsi.calls);
        Thread.sleep(700L);
        hmi.idle();
        c = dsi.drain();
        check(count(c, "switch 81 term 4") == 1 && !has(c, "switch 73 term 4"),
            "a resize inside the renderer's worst case is not composed early and re-pointed: " + c);
        release(hmi);
        dsi.drain();

        write(ready, "0328 0181 0000000200.0000000004\n"); /* never confirmed, then a late resize */
        MostPresentation.setActive(true);
        hmi.idle();
        check(waitFor(dsi, "switch 81 term 4", MostPresentation.READY_WAIT_MS + 1500L),
            "composed once for diagnosis after the wait: " + dsi.calls);
        dsi.drain();
        write(ready, "0800 0252 0000000200.0000000005\n");  /* the resize lands late */
        check(waitFor(dsi, "switch 73 term 4", 2000L) && waitFor(dsi, "switch 81 term 4", 2000L),
            "a late resize after the diagnostic compose is re-pointed: " + dsi.calls);
        release(hmi);
        check(dsi.offHmi.isEmpty(), "display service touched on the HMI thread: " + dsi.offHmi);
        endSession();
        freshOutputRequest();
    }

    /** The request file's content, or null when there is none. */
    static String read(File f) throws Exception {
        if (!f.isFile()) return null;
        byte[] b = new byte[(int) f.length()];
        FileInputStream in = new FileInputStream(f);
        try {
            int n = 0;
            while (n < b.length) {
                int r = in.read(b, n, b.length - n);
                if (r < 0) break;
                n += r;
            }
        } finally { in.close(); }
        return new String(b, "US-ASCII");
    }

    static String outputRequest() throws Exception { return read(outputRequestFile); }

    static void requestOutput(int w, int h) throws Exception {
        Method m = MostPresentation.class.getDeclaredMethod("requestRendererOutput", Integer.TYPE, Integer.TYPE);
        m.setAccessible(true);
        m.invoke(null, Integer.valueOf(w), Integer.valueOf(h));
    }

    static boolean waitFor(Dsi dsi, String entry, long ms) throws Exception {
        long deadline = System.currentTimeMillis() + ms;
        while (System.currentTimeMillis() < deadline) {
            if (has(new ArrayList<String>(dsi.calls), entry)) return true;
            Thread.sleep(20L);
        }
        return has(new ArrayList<String>(dsi.calls), entry);
    }

    /** Stock's arrows view (ctx 73) carries CarPlay's maneuver only while CarPlay guides. */
    static void arrowsViewReachesTheDisplayService() throws Exception {
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        extents.put(Integer.valueOf(20), new int[]{400, 220});   /* stock KDK as the DM reports it */
        DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        mostSession(true);
        release(hmi);
        dsi.drain();

        stockSwitch(hmi, dm, 72, 1, null);
        stockSwitch(hmi, dm, 72, 1, null);
        stockSwitch(hmi, dm, 73, 1, null);
        List<String> c = dsi.drain();
        check(has(c, "switch 73 term 4"), "idle: stock arrows view is stock ctx 73: " + c);

        MostPresentation.setActive(true);
        hmi.idle();
        c = dsi.drain();
        check(MostPresentation.isActive(), "arrows view carries CarPlay once the HMI thread applied it");
        check(has(c, "switch 81 term 4"), "CarPlay start re-issues the stock request as ctx 81 = {98}: " + c);
        check(has(c, "crop 98 src(0,0 400x220) dst(0,0 400x220)"),
            "98 is the whole KOMO stream at stock KDK's 400x220, identity-cropped: " + c);
        check("0400 0220\n".equals(outputRequest()), "renderer asked for a 400x220 window: " + outputRequest());
        check(c.indexOf("opacity 98=0") >= 0 && c.indexOf("opacity 98=0") < c.indexOf("opacity 98=100"),
            "98 written 0 then 100, past the DM's boot-time opacity cache: " + c);
        check(c.indexOf("crop 98 src(0,0 400x220) dst(0,0 400x220)") < c.indexOf("switch 81 term 4"),
            "98 is placed before ctx 81 makes it visible: " + c);
        check(!hasPrefix(c, "getExtents"), "cached extents are not re-requested: " + c);
        check(count(c, "crop 98 ") == 1, "one setCropping per start, not re-sent when 81 is applied: " + c);

        stockSwitch(hmi, dm, 72, 1, null);
        c = dsi.drain();
        check(has(c, "switch 72 term 4") && !hasPrefix(c, "crop"), "map view stays the stock map: " + c);

        stockSwitch(hmi, dm, 73, 1, null);
        c = dsi.drain();
        check(has(c, "switch 81 term 4"), "every stock arrows request carries CarPlay while active: " + c);

        stockSwitch(hmi, dm, 73, 0, null);
        c = dsi.drain();
        check(has(c, "switch 73 term 0"), "only the cluster terminal is substituted: " + c);

        stockSwitch(hmi, dm, 73, 1, null);
        dsi.drain();
        release(hmi);
        c = dsi.drain();
        check(has(c, "opacity 98=0") && has(c, "switch 73 term 4"),
            "CarPlay stop hides 98 and hands the arrows view back to stock ctx 73: " + c);
        check(c.indexOf("switch 73 term 4") < c.indexOf("opacity 98=0"),
            "stock's 73 is back before 98 is hidden, so the arrows view is never an empty ctx 81: " + c);
        check(MostPresentation.substitute(1, 73) == 73, "inactive substitution is the identity");
        check(dsi.offHmi.isEmpty(), "every display-service call is made on the HMI thread: " + dsi.offHmi);
        endSession();
    }

    /** The cluster stream rate.  The HMI's setUpdateRate reaches the display service with the
     *  internal id and is remembered per terminal; while CarPlay's arrows view is composed,
     *  stock's full rate 10 is sent as 30 (1 and 0 pass), set on composition and handed back on
     *  release.  ClusterStreamRate registers one display-management listener, asks the cluster
     *  display (internal 4) for its rate at connect and on each arrows edge, records the answer,
     *  survives a failing service and unregisters at disconnect. */
    static void streamRateLoggedAndRaisedForArrows() throws Exception {
        Hmi hmi = new Hmi();
        final Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        extents.put(Integer.valueOf(20), new int[]{400, 220});
        DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        mostSession(true);
        release(hmi);
        dsi.drain();
        Log.setLevel(Log.W);                                  /* the car's default level */
        Log.w("Test", "stream-rate start");

        check(DisplayManagerMIB2High.requestedUpdateRate(1) == -1, "no rate requested yet");
        dm.setUpdateRate(1, 10);
        dm.setUpdateRate(1, 10);
        dm.setUpdateRate(0, 30);
        List<String> c = dsi.drain();
        check(c.equals(java.util.Arrays.asList("setUpdateRate 4 10", "setUpdateRate 4 10", "setUpdateRate 0 30")),
            "every HMI rate request reaches the display service unchanged: " + c);
        check(DisplayManagerMIB2High.requestedUpdateRate(1) == 10 && DisplayManagerMIB2High.requestedUpdateRate(0) == 30
            && DisplayManagerMIB2High.requestedUpdateRate(8) == -1 && DisplayManagerMIB2High.requestedUpdateRate(-1) == -1,
            "requested rates remembered per terminal");

        final List<String> registry = Collections.synchronizedList(new ArrayList<String>());
        final Object[] listener = new Object[1];
        final Object service = Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{DSIDisplayManagement.class}, dsi);
        final Object reference = Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{org.osgi.framework.ServiceReference.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) { return null; }
            });
        final Object registration = Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{org.osgi.framework.ServiceRegistration.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) {
                    registry.add(m.getName());
                    return null;
                }
            });
        final Object manager = Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{de.audi.app.terminalmode.osgi.IServiceManager.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) {
                    String n = m.getName();
                    if (n.equals("getServiceReferences")) {
                        registry.add("lookup " + ((Class<?>) a[0]).getName());
                        Object[] refs = (Object[]) java.lang.reflect.Array.newInstance(
                            org.osgi.framework.ServiceReference.class, 1);
                        refs[0] = reference;
                        return refs;
                    }
                    if (n.equals("getService")) return a[0] == reference ? service : null;
                    if (n.equals("registerDSIListener")) {
                        registry.add("register " + a[0] + " " + a[1]);
                        listener[0] = a[2];
                        return registration;
                    }
                    registry.add(n);
                    return null;
                }
            });
        final IFrameworkAccess fw = framework(1);
        Object context = Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{de.audi.app.terminalmode.IContext.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) {
                    if (m.getName().equals("getServiceManager")) return manager;
                    if (m.getName().equals("getFramework")) return fw;
                    return null;
                }
            });

        com.luka.carplay.cluster.ClusterStreamRate.query("before start");
        check(dsi.drain().isEmpty(), "no query before the listener is registered");

        com.luka.carplay.cluster.ClusterStreamRate probe = new com.luka.carplay.cluster.ClusterStreamRate();
        probe.start(new FrameworkRef((de.audi.app.terminalmode.IContext) context));
        check(registry.contains("lookup org.dsi.ifc.displaymanagement.DSIDisplayManagement")
            && registry.contains("register 0 org.dsi.ifc.displaymanagement.DSIDisplayManagementListener")
            && listener[0] instanceof org.dsi.ifc.displaymanagement.DSIDisplayManagementListener,
            "one instance-0 display-management listener registered: " + registry);
        c = dsi.drain();
        check(c.equals(java.util.Arrays.asList("getUpdateRate 4")), "connect asks the cluster display only: " + c);
        probe.start(new FrameworkRef((de.audi.app.terminalmode.IContext) context));
        check(count(registry, "register ") == 1 && dsi.drain().isEmpty(), "a second start registers nothing more");

        /* A second query before the first answer: each answer is logged under its own trigger. */
        com.luka.carplay.cluster.ClusterStreamRate.query("KOMO data rate 2");
        check(dsi.drain().equals(java.util.Arrays.asList("getUpdateRate 4")), "an overlapping query is sent");
        org.dsi.ifc.displaymanagement.DSIDisplayManagementListener l =
            (org.dsi.ifc.displaymanagement.DSIDisplayManagementListener) listener[0];
        l.getUpdateRateResult(4, 10);
        l.getUpdateRateResult(4, 10);
        for (int i = 0; i < 4; i++) l.setUpdateRateResult(4, 0);
        l.setUpdateRateResult(4, 30);
        l.activeContext(81, 4, 1);
        l.asyncException(1, "other request", org.dsi.ifc.displaymanagement.DSIDisplayManagement.RT_SETUPDATERATE + 1000);
        check("streamRate{requested=10 sent=10 dsi=(4, 10)}".equals(com.luka.carplay.cluster.ClusterStreamRate.describe()),
            "answer and HMI request recorded: " + com.luka.carplay.cluster.ClusterStreamRate.describe());
        check(dsi.drain().isEmpty(), "replies trigger no display-service call");

        /* Stock is at its full rate (10) when CarPlay composes the arrows view: 30 is sent once,
         * on the HMI thread, after 81 is composed; the rate query follows. */
        stockSwitch(hmi, dm, 73, 1, null);
        dsi.drain();
        MostPresentation.setActive(true);
        hmi.idle();
        c = dsi.drain();
        check(has(c, "switch 81 term 4") && count(c, "setUpdateRate ") == 1 && has(c, "setUpdateRate 4 30")
            && c.indexOf("switch 81 term 4") < c.indexOf("setUpdateRate 4 30")
            && count(c, "getUpdateRate ") == 1 && c.indexOf("setUpdateRate 4 30") < c.indexOf("getUpdateRate 4"),
            "arrows-on raises stock's 10 to 30 once, after 81, then asks: " + c);
        check(DisplayManagerMIB2High.requestedUpdateRate(1) == 10, "stock's own request stays recorded as 10");
        check(com.luka.carplay.cluster.ClusterStreamRate.describe().startsWith("streamRate{requested=10 sent=30 "),
            "the state trace shows the substituted rate: " + com.luka.carplay.cluster.ClusterStreamRate.describe());

        /* While composed: stock's 1 and 0 pass through, its 10 becomes 30, other terminals are untouched. */
        final DisplayManagerMIB2High dmRef = dm;
        String[] stockRequests = {"1 1", "1 10", "1 0", "1 10", "0 10"};
        String[] expected = {"setUpdateRate 4 1", "setUpdateRate 4 30", "setUpdateRate 4 0", "setUpdateRate 4 30",
            "setUpdateRate 0 10"};
        for (int i = 0; i < stockRequests.length; i++) {
            final int terminal = Integer.parseInt(stockRequests[i].split(" ")[0]);
            final int rate = Integer.parseInt(stockRequests[i].split(" ")[1]);
            hmi.run(new Runnable() { public void run() { dmRef.setUpdateRate(terminal, rate); } });
            c = dsi.drain();
            check(c.equals(java.util.Arrays.asList(expected[i])),
                "stock " + stockRequests[i] + " while composed sends " + expected[i] + ": " + c);
        }
        check(DisplayManagerMIB2High.requestedUpdateRate(1) == 10 && DisplayManagerMIB2High.requestedUpdateRate(0) == 10,
            "requests recorded as stock sent them");

        release(hmi);
        c = dsi.drain();
        check(has(c, "switch 73 term 4") && count(c, "setUpdateRate ") == 1 && has(c, "setUpdateRate 4 10")
            && c.indexOf("switch 73 term 4") < c.indexOf("setUpdateRate 4 10") && count(c, "getUpdateRate ") == 1,
            "arrows-off hands stock's 10 back once, after 73, then asks: " + c);
        hmi.run(new Runnable() { public void run() { dmRef.setUpdateRate(1, 10); } });
        check(dsi.drain().equals(java.util.Arrays.asList("setUpdateRate 4 10")), "stock's 10 passes after arrows-off");

        /* Stock at its reduced rate (1) when the view is composed and released: nothing is sent. */
        hmi.run(new Runnable() { public void run() { dmRef.setUpdateRate(1, 1); } });
        dsi.drain();
        MostPresentation.setActive(true);
        hmi.idle();
        c = dsi.drain();
        check(has(c, "switch 81 term 4") && !hasPrefix(c, "setUpdateRate"), "arrows-on at stock's 1 sends no rate: " + c);
        release(hmi);
        c = dsi.drain();
        check(has(c, "switch 73 term 4") && !hasPrefix(c, "setUpdateRate"), "arrows-off at stock's 1 sends no rate: " + c);

        dsi.failRateQuery = true;
        com.luka.carplay.cluster.ClusterStreamRate.query("failing service");
        dsi.failRateQuery = false;
        check(dsi.drain().isEmpty(), "a failing query is swallowed");

        probe.stop();
        probe.stop();
        check(count(registry, "unregister") == 1, "disconnect unregisters the listener once: " + registry);
        com.luka.carplay.cluster.ClusterStreamRate.query("after stop");
        check(dsi.drain().isEmpty(), "no query after stop");
        boolean onlyQueriesOffHmi = true;
        synchronized (dsi.offHmi) {
            for (int i = 0; i < dsi.offHmi.size(); i++) {
                String e = dsi.offHmi.get(i);
                onlyQueriesOffHmi &= e.startsWith("getUpdateRate ") || e.startsWith("setUpdateRate ");
            }
        }
        check(onlyQueriesOffHmi, "only rate calls (the read-only query, the test's own requests) leave the HMI thread: "
            + dsi.offHmi);
        List<String> log = logSince("stream-rate");
        Log.setLevel(-1);
        check(countContaining(log, "getUpdateRateResult(4, 10) for display 4 after CarPlay connect;") == 1
            && countContaining(log, "getUpdateRateResult(4, 10) for display 4 after KOMO data rate 2;") == 1,
            "overlapping answers are logged under their own triggers: " + log);
        check(count(log, "[CP/W][StreamRate] setUpdateRateResult(4, 0)") == 1
            && count(log, "[CP/W][StreamRate] setUpdateRateResult(4, 30)") == 1,
            "repeated setUpdateRateResult replies are logged once: " + log);
        check(countContaining(log, "setUpdateRate terminal ") == 2
            && count(log, "[CP/W][DisplayManager] setUpdateRate terminal 1 rate 10 -> 30 for the CarPlay arrows view") == 2,
            "at WARN only stock's two substituted requests are logged, not its plain rate changes: " + log);
        /* The rate records are static (they must exist before the constructor runs): clear them
         * so later scenarios start from "no request since HMI start". */
        java.util.Arrays.fill((int[]) field(DisplayManagerMIB2High.class, "REQUESTED_RATES").get(null), -1);
        java.util.Arrays.fill((int[]) field(DisplayManagerMIB2High.class, "SENT_RATES").get(null), -1);
        endSession();
    }

    /** Stock's listener (model 168 via DisplayControllerEvo) is told the context it asked for,
     *  on the HMI thread, whether the DSI confirms within the DM's 200 ms wait or later. */
    static void stockListenerSeesItsOwnContext() throws Exception {
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        extents.put(Integer.valueOf(20), new int[]{400, 220});
        DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        mostSession(true);
        release(hmi);
        Listener stock = new Listener();
        stock.hmi = hmi;

        stockSwitch(hmi, dm, 72, 1, null);            /* leave 73 so the next request is a real switch */
        stockSwitch(hmi, dm, 73, 1, stock);
        check(stock.seen.equals(java.util.Arrays.asList("73 term 1")), "stock ctx 73 confirmed in time: " + stock.seen);
        stock.seen.clear();

        MostPresentation.setActive(true);
        hmi.idle();
        check(has(dsi.drain(), "switch 81 term 4"), "CarPlay start composes ctx 81");
        check(stock.seen.equals(java.util.Arrays.asList("73 term 1")),
            "CarPlay start (reapply) reports 73 to stock's listener, never 81 or 81 % 79 = 2: " + stock.seen);
        check(field(DisplayManagerMIB2High.class, "lastClusterListener").get(dm) == stock,
            "the recorded listener is stock's own, not the reporting wrapper");
        stock.seen.clear();

        /* Late confirmation: stock posts an ActiveContextEvent to the HMI queue. */
        dsi.answerDelayMs = 350L;
        stockSwitch(hmi, dm, 72, 1, stock);
        stockSwitch(hmi, dm, 73, 1, stock);
        Thread.sleep(1000L);
        hmi.idle();
        check(stock.seen.contains("73 term 1"), "late-confirmed ctx 81 reported to stock as 73: " + stock.seen);
        check(!stock.seen.contains("2 term 1") && !stock.seen.contains("81 term 1"),
            "stock never sees CarPlay's context number: " + stock.seen);
        dsi.answerDelayMs = 5L;
        stock.seen.clear();

        release(hmi);
        check(has(dsi.drain(), "switch 73 term 4"), "CarPlay stop returns stock ctx 73");
        check(stock.seen.equals(java.util.Arrays.asList("73 term 1")), "stop reapply reports stock ctx 73: " + stock.seen);
        IDisplayListener[] listeners = (IDisplayListener[]) field(DisplayManager.class, "displayListeners").get(dm);
        check(listeners[1] == stock, "after stop the DM holds stock's own listener again");
        check(stock.offHmi.isEmpty(), "stock's listener only ever runs on the HMI thread: " + stock.offHmi);
        check(dsi.offHmi.isEmpty(), "every display-service call is made on the HMI thread: " + dsi.offHmi);
        endSession();
    }

    /** A switch made while the display service is away is buffered by stock with the listener it
     *  was given and replayed by DisplayManager.addingService when the service (re)appears. */
    static void bufferedReplayAfterDisplayServiceRestart() throws Exception {
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        extents.put(Integer.valueOf(20), new int[]{400, 220});
        final DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        mostSession(true);
        release(hmi);
        final Listener stock = new Listener();
        stock.hmi = hmi;
        stockSwitch(hmi, dm, 72, 1, null);
        MostPresentation.setActive(true);
        hmi.idle();

        set(dm, "initialized", Boolean.FALSE);                   /* display service gone */
        stockSwitch(hmi, dm, 73, 1, stock);                      /* driver picks the arrows view */
        check(((Integer) field(DisplayManager.class, "bufferedContext").get(dm)).intValue() == 81,
            "stock buffers the composed context");
        dsi.drain();
        stock.seen.clear();

        set(dm, "initialized", Boolean.TRUE);                    /* display service back */
        final Runnable replay = new Runnable() {                 /* DisplayManager.addingService */
            public void run() {
                try {
                    int ctx = ((Integer) field(DisplayManager.class, "bufferedContext").get(dm)).intValue();
                    int term = ((Integer) field(DisplayManager.class, "bufferedTerm").get(dm)).intValue();
                    IDisplayListener[] held = (IDisplayListener[]) field(DisplayManager.class, "displayListeners").get(dm);
                    dm.switchContext(ctx, term, held[term]);
                } catch (Exception e) { throw new RuntimeException(e); }
            }
        };
        hmi.run(replay);
        List<String> c = dsi.drain();
        check(has(c, "switch 81 term 4"), "replay while CarPlay guides: CarPlay's maneuver: " + c);
        check(stock.seen.equals(java.util.Arrays.asList("73 term 1")), "replay reports stock's own context: " + stock.seen);
        check(((Integer) field(DisplayManagerMIB2High.class, "lastClusterRequest").get(dm)).intValue() == 73
            && field(DisplayManagerMIB2High.class, "lastClusterListener").get(dm) == stock,
            "replay recorded as stock's original request and listener");

        set(dm, "initialized", Boolean.FALSE);                   /* gone again, ctx 81 buffered... */
        stockSwitch(hmi, dm, 73, 1, stock);
        MostPresentation.setActive(false);                       /* ...and CarPlay stops meanwhile */
        hmi.idle();
        IDisplayListener[] held = (IDisplayListener[]) field(DisplayManager.class, "displayListeners").get(dm);
        check(((Integer) field(DisplayManager.class, "bufferedContext").get(dm)).intValue() == 73 && held[1] == stock,
            "CarPlay stop while the service is away re-buffers stock ctx 73 with stock's listener");

        field(DisplayManager.class, "bufferedContext").set(dm, Integer.valueOf(81));   /* even a stale 81 replay */
        held[1] = stockReporter(dm, stock);
        set(dm, "initialized", Boolean.TRUE);
        dsi.drain();
        stock.seen.clear();
        hmi.run(replay);
        c = dsi.drain();
        check(has(c, "switch 73 term 4") && !has(c, "switch 81 term 4"),
            "a stale ctx-81 replay after CarPlay stopped becomes stock ctx 73: " + c);
        check(stock.seen.equals(java.util.Arrays.asList("73 term 1")), "stale replay reports 73: " + stock.seen);
        check(dsi.offHmi.isEmpty(), "every display-service call is made on the HMI thread: " + dsi.offHmi);
        endSession();
    }

    /** The DM's own reporting wrapper, as stock would hold it after a substituted switch. */
    static IDisplayListener stockReporter(DisplayManagerMIB2High dm, IDisplayListener stock) throws Exception {
        Class<?> c = Class.forName("de.audi.tghu.fwhmi.DisplayManagerMIB2High$StockContextReporter");
        Constructor<?> ctor = c.getDeclaredConstructor(IDisplayListener.class, Integer.TYPE, Integer.TYPE);
        ctor.setAccessible(true);
        return (IDisplayListener) ctor.newInstance(stock, Integer.valueOf(81), Integer.valueOf(73));
    }

    /** 98 is the whole stream: at its origin whatever position stock gave KDK, sized to KDK's
     *  extents; a non-positive extents answer keeps waiting on the KVS_Most default; every
     *  start re-places. */
    static void placementSizeAndRefresh() throws Exception {
        int[][][] positions = (int[][][]) field(DisplayManager.class, "displayablePositions").get(null);
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        int[] kdk = new int[]{400, 220};
        extents.put(Integer.valueOf(20), kdk);
        DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        mostSession(true);
        release(hmi);
        stockSwitch(hmi, dm, 72, 1, null);
        stockSwitch(hmi, dm, 73, 1, null);
        try {
            positions[4][20][0] = 10;                             /* stock setCropping(20, 1, ..., 10, 20, ...) */
            positions[4][20][1] = 20;
            dsi.drain();
            MostPresentation.setActive(true);
            hmi.idle();
            List<String> c = dsi.drain();
            check(has(c, "crop 98 src(0,0 400x220) dst(0,0 400x220)"),
                "98 at the stream origin, not at stock KDK's (10,20): " + c);
            release(hmi);
            check(MostPresentation.describe().indexOf("placement={none}") >= 0, "stop forgets the placement: " + MostPresentation.describe());
            dsi.drain();

            MostPresentation.setActive(true);                     /* second route, same placement */
            hmi.idle();
            c = dsi.drain();
            check(count(c, "crop 98 ") == 1 && has(c, "opacity 98=100"), "second start places and shows 98 again: " + c);
            check(MostPresentation.describe().indexOf("window 400x220 from stock KDK") >= 0,
                "second start records (and logs) its placement: " + MostPresentation.describe());
            release(hmi);
        } finally {
            positions[4][20][0] = 0;
            positions[4][20][1] = 0;
        }

        kdk[0] = 0;                                               /* display service reports 0x0 */
        kdk[1] = 0;
        dsi.drain();
        MostPresentation.setActive(true);
        hmi.idle();
        List<String> c = dsi.drain();
        check(has(c, "crop 98 src(0,0 800x252) dst(0,0 800x252)")
            && MostPresentation.describe().indexOf("KVS_Most default (KDK extents unknown)") >= 0,
            "0x0 extents are unknown, not 'from stock KDK 0x0': " + MostPresentation.describe());
        check("0800 0252\n".equals(outputRequest()), "unknown extents: renderer asked for KVS_Most 800x252: " + outputRequest());
        kdk[0] = 400;                                             /* stock updates the cached array in place */
        kdk[1] = 220;
        check(waitFor(dsi, "crop 98 src(0,0 400x220) dst(0,0 400x220)", 2000L),
            "the retry keeps waiting past a 0x0 answer and re-sizes: " + dsi.calls);
        check("0400 0220\n".equals(outputRequest()), "re-size reaches the renderer: " + outputRequest());
        release(hmi);
        check(dsi.offHmi.isEmpty(), "every display-service call is made on the HMI thread: " + dsi.offHmi);
        endSession();
    }

    /** A stock re-issue of the current context (81 while CarPlay composes) means stock's arrows
     *  view, so CarPlay's stop still returns 73. */
    static void reissuedCarPlayContextMeansStockArrows() throws Exception {
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        extents.put(Integer.valueOf(20), new int[]{400, 220});
        DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        mostSession(true);
        release(hmi);
        stockSwitch(hmi, dm, 72, 1, null);
        stockSwitch(hmi, dm, 73, 1, null);
        MostPresentation.setActive(true);
        hmi.idle();
        stockSwitch(hmi, dm, dm.getCurrentContextID(1), 1, null);  /* e.g. setKDKVisible re-issuing current */
        check(((Integer) field(DisplayManagerMIB2High.class, "lastClusterRequest").get(dm)).intValue() == 73,
            "a re-issued 81 is recorded as stock's 73");
        dsi.drain();
        release(hmi);
        List<String> c = dsi.drain();
        check(has(c, "switch 73 term 4"), "CarPlay stop still returns stock ctx 73: " + c);
        endSession();
    }

    /** claimKomoGuidance racing itself (RGI start vs teardown) never leaves the flag and the
     *  KOMO service disagreeing. */
    static void komoClaimIsAtomic() throws Exception {
        installCarPlayFramework(framework(1));
        final Method claim = BAPBridge.class.getDeclaredMethod("claimKomoGuidance", Boolean.TYPE);
        claim.setAccessible(true);
        final Caller caller = caller();
        final CarPlayKOMOService k = komo(caller);
        ClusterService cs = komoClusterService(k);
        final BAPBridge bridge = new BAPBridge();
        set(bridge, "csRef", cs);
        Field owned = field(BAPBridge.class, "komoGuidanceOwned");
        final java.util.concurrent.CyclicBarrier go = new java.util.concurrent.CyclicBarrier(3);
        final java.util.concurrent.CyclicBarrier done = new java.util.concurrent.CyclicBarrier(3);
        final int rounds = 3000;
        for (int t = 0; t < 2; t++) {
            final Boolean value = Boolean.valueOf(t == 0);
            Thread th = new Thread(new Runnable() {
                public void run() {
                    try {
                        for (int r = 0; r < rounds; r++) {
                            go.await();
                            claim.invoke(bridge, value);
                            done.await();
                        }
                    } catch (Exception e) { throw new RuntimeException(e); }
                }
            });
            th.setDaemon(true);
            th.start();
        }
        int mismatches = 0;
        for (int r = 0; r < rounds; r++) {
            claim.invoke(bridge, Boolean.FALSE);                  /* each round starts released */
            go.await(5, TimeUnit.SECONDS);
            done.await(5, TimeUnit.SECONDS);
            if (owned.getBoolean(bridge) != k.isCarPlayOwned()) mismatches++;
            synchronized (caller) { caller.calls.clear(); }
        }
        check(mismatches == 0, "claim/release interleavings left flag and KOMO owner disagreeing " + mismatches + " times");
        claim.invoke(bridge, Boolean.FALSE);
    }

    /** The coding is read once a session start resolves it; the hot no-argument queries then do
     *  no framework lookups.  A new binding clears it. */
    static void platformResolvedOnce() throws Exception {
        final int[] reads = {0};
        IFrameworkAccess counting = (IFrameworkAccess) Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{IFrameworkAccess.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) {
                    if (m.getName().equals("getSysConst")) { reads[0]++; return Integer.valueOf(1); }
                    if (m.getReturnType() == Integer.TYPE) return Integer.valueOf(0);
                    if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
                    return null;
                }
            });
        ClusterPlatform.bind(counting);
        check(ClusterPlatform.isMost() && reads[0] == 1, "unresolved: read live");
        ClusterPlatform.resolve(counting);
        int resolvedAt = reads[0];
        for (int i = 0; i < 100; i++) { ClusterPlatform.isMost(); ClusterPlatform.ownsContexts(); }
        check(reads[0] == resolvedAt, "resolved: no sysConst reads on the hot path (" + (reads[0] - resolvedAt) + ")");
        check(ClusterPlatform.isMost() && !ClusterPlatform.ownsContexts(), "resolved MOST coding answers as MOST");
        ClusterPlatform.bind(framework(2));
        check(ClusterPlatform.ownsContexts() && !ClusterPlatform.isMost(), "a new binding clears the resolved coding");
    }

    /** The request maneuver_render reads every second: exactly "<w> <h>\n", written in place
     *  (the car's /tmp cannot rename: no temp file), rewritten only on change, and retried
     *  after a failed write. */
    static void rendererOutputRequestFile() throws Exception {
        freshOutputRequest();
        check(MostPresentation.KVS_MOST_W == 800 && MostPresentation.KVS_MOST_H == 252, "KVS_Most default is 800x252");
        File blocker = new File(outputRequestFile.getPath() + ".tmp");
        check(blocker.mkdir(), "a directory where a temp file would go");
        requestOutput(800, 252);
        check("0800 0252\n".equals(outputRequest()), "request written without a temp file or rename: " + outputRequest());
        check(blocker.isDirectory() && blocker.list().length == 0, "temp path untouched");
        check(blocker.delete(), "blocker removed");
        check(outputRequestFile.delete(), "request removable");
        requestOutput(800, 252);
        check(outputRequest() == null, "an unchanged request is not rewritten");
        requestOutput(656, 360);
        check("0656 0360\n".equals(outputRequest()), "a new size is written: " + outputRequest());
        requestOutput(800, 252);
        check("0800 0252\n".equals(outputRequest()), "an existing request is replaced: " + outputRequest());

        File missing = new File(outputRequestFile.getParentFile(), "absent" + File.separator + "carplay_most_output");
        setStatic(MostPresentation.class, "outputRequestPath", missing.getPath());
        requestOutput(400, 220);                       /* directory absent: logged, not thrown */
        check(!missing.exists(), "failed write leaves nothing");
        check(missing.getParentFile().mkdirs(), "directory created");
        requestOutput(400, 220);
        check("0400 0220\n".equals(read(missing)), "a failed request is retried with the same size: " + read(missing));
        missing.delete();
        missing.getParentFile().delete();
        freshOutputRequest();
    }

    /** Only a MOST session drives the arrows view; VC and RGI-only never touch it. */
    static void screenModuleActivation() throws Exception {
        int[] modes = {1, 0, 2};
        for (int i = 0; i < modes.length; i++) {
            int mode = modes[i];
            Hmi hmi = new Hmi();
            Dsi dsi = new Dsi();
            displayManager(dsi, hmi, new HashMap<Integer, int[]>());
            release(hmi);
            ClusterPlatform.bind(framework(mode));
            setStatic(ScreenModule.class, "platformSupported", Boolean.TRUE);
            setStatic(ScreenModule.class, "connected", Boolean.TRUE);
            setStatic(ScreenModule.class, "clusterContextsOwned", Boolean.valueOf(mode == 2));
            ScreenModule.setNavActive(true);
            hmi.idle();
            check(MostPresentation.isActive() == (mode == 1), "541=" + mode + ": arrows view active=" + MostPresentation.isActive());
            ScreenModule.setNavActive(false);
            hmi.idle();
            check(!MostPresentation.isActive(), "541=" + mode + ": route end releases immediately");
            check(!ScreenModule.isNavActive() || mode == 2, "541=" + mode + ": no VC-style hold on a non-VC cluster");
            check(dsi.offHmi.isEmpty(), "541=" + mode + ": display service touched only on the HMI thread: " + dsi.offHmi);
            endSession();
        }
    }

    /** The CarPlay route-guidance edge itself (ScreenModule.setNavActive, as RouteGuidance calls it)
     *  reaches the display service, including when stock is already showing the arrows view. */
    static void sessionEdgeDrivesTheDisplayService() throws Exception {
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        release(hmi);
        mostSession(true);
        dsi.extentsAnswers.put(Integer.valueOf(20), new int[]{656, 360});

        stockSwitch(hmi, dm, 72, 1, null);             /* driver has the map view up */
        stockSwitch(hmi, dm, 73, 1, null);             /* ...then picks the arrows view */
        dsi.drain();
        ScreenModule.setNavActive(true);               /* CarPlay route guidance starts */
        hmi.idle();
        List<String> c = dsi.drain();
        check(has(c, "switch 81 term 4"), "RGI start swaps the showing arrows view to CarPlay: " + c);
        check(has(c, "getExtents 20"), "stock KDK extents requested from the display service: " + c);
        check(has(c, "crop 98 src(0,0 800x252) dst(0,0 800x252)"), "unknown KDK extents: KVS_Most 800x252 default: " + c);

        check(waitFor(dsi, "crop 98 src(0,0 656x360) dst(0,0 656x360)", 2000L),
            "placement re-sized once the DM caches stock KDK 656x360: " + dsi.calls);
        Thread.sleep(300L);
        hmi.idle();
        c = dsi.drain();
        check(!hasPrefix(c, "getExtents"), "the retry only reads the cache, never re-requests: " + c);
        int fits = 0;
        for (int i = 0; i < c.size(); i++) if (c.get(i).equals("crop 98 src(0,0 656x360) dst(0,0 656x360)")) fits++;
        check(fits == 1, "re-sized exactly once: " + c);
        check("0656 0360\n".equals(outputRequest()), "renderer asked for 656x360: " + outputRequest());

        ScreenModule.setNavActive(false);              /* route end */
        hmi.idle();
        c = dsi.drain();
        check(has(c, "switch 73 term 4") && has(c, "opacity 98=0"), "route end returns stock ctx 73: " + c);

        ScreenModule.setNavActive(true);
        hmi.idle();
        dsi.drain();
        setStatic(ScreenModule.class, "connected", Boolean.FALSE);   /* phone disconnect mid-route (ScreenModule.stop) */
        setStatic(ScreenModule.class, "navActive", Boolean.FALSE);
        MostPresentation.setActive(false);
        hmi.idle();
        c = dsi.drain();
        check(has(c, "switch 73 term 4") && has(c, "opacity 98=0"), "disconnect returns stock ctx 73: " + c);
        check(dsi.offHmi.isEmpty(), "every display-service call is made on the HMI thread: " + dsi.offHmi);
        endSession();
    }

    /** A start that reaches the HMI thread after the phone has gone composes nothing; the
     *  next start in a new session still works. */
    static void lateStartAfterDisconnect() throws Exception {
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        extents.put(Integer.valueOf(20), new int[]{400, 220});
        DisplayManagerMIB2High dm = displayManager(dsi, hmi, extents);
        release(hmi);
        mostSession(false);
        stockSwitch(hmi, dm, 73, 1, null);
        dsi.drain();

        MostPresentation.setActive(true);              /* queued, but the session is already gone */
        hmi.idle();
        List<String> c = dsi.drain();
        check(!MostPresentation.isActive() && !has(c, "switch 81 term 4") && !hasPrefix(c, "opacity"),
            "disconnected: nothing composed: " + c);

        setStatic(ScreenModule.class, "connected", Boolean.TRUE);   /* next session, route already requested */
        MostPresentation.setActive(true);
        hmi.idle();
        c = dsi.drain();
        check(MostPresentation.isActive() && has(c, "switch 81 term 4"), "a repeated request in a live session applies: " + c);
        release(hmi);
        endSession();
    }

    /** MOST connect warms the extents cache on the HMI thread and sizes the renderer's window
     *  before any route; no extents request when the cache is warm. */
    static void prefetchOnConnect() throws Exception {
        Hmi hmi = new Hmi();
        Dsi dsi = new Dsi();
        Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        displayManager(dsi, hmi, extents);
        release(hmi);
        dsi.drain();
        freshOutputRequest();
        dsi.extentsAnswers.put(Integer.valueOf(20), new int[]{400, 220});
        MostPresentation.prefetchExtents();
        hmi.idle();
        check(has(dsi.drain(), "getExtents 20"), "cold cache: extents requested at connect");
        check("0800 0252\n".equals(outputRequest()), "cold cache: renderer sized for KVS_Most 800x252 at connect: " + outputRequest());
        Thread.sleep(150L);
        hmi.idle();
        check(extents.get(Integer.valueOf(20)) != null, "answer cached by the HMI-thread runnable");
        MostPresentation.prefetchExtents();
        hmi.idle();
        check(dsi.drain().isEmpty(), "warm cache: nothing requested");
        check("0400 0220\n".equals(outputRequest()), "warm cache: renderer sized for stock KDK 400x220: " + outputRequest());
        check(dsi.offHmi.isEmpty(), "prefetch runs on the HMI thread: " + dsi.offHmi);
    }

    /* ---------------- KOMO rig ---------------- */

    public static final class Caller extends KOMOCaller {
        List<String> calls;
        Caller() { super(null, null); }
        public synchronized void setDistanceToNextManeuver(long v, int u, boolean ok) { calls.add("dist " + v + " " + u + " " + ok); }
        public synchronized void setTurnToStreet(String a, String b) { calls.add("turn [" + a + "][" + b + "]"); }
        public synchronized void setCurrentStreet(String s) { calls.add("street [" + s + "]"); }
        public synchronized void setETA(int f, short d, short h, short m, boolean ok, boolean fl) { calls.add("eta " + f + " " + d + " " + h + ":" + m + " " + ok); }
        public synchronized void setRTT(short h, short m, boolean ok) { calls.add("rtt " + h + ":" + m + " " + ok); }
        public synchronized void setDistanceToDestination(long v, int u, boolean ok) { calls.add("dest " + v + " " + u + " " + ok); }
        List<String> drain() { List<String> out = new ArrayList<String>(calls); calls.clear(); return out; }
    }

    /** Stock view-mode object reduced to the KOMO visibility it is told (CarPlayKOMOService hold). */
    public static final class ViewStub extends de.audi.tghu.navi.app.cluster.ClusterViewMode {
        boolean visible;
        ViewStub() { super(null, null); }
        public void setKOMOViewVisible(boolean v) { visible = v; }
    }

    /** A ClusterService with only what the KOMO paths touch; the constructor-built locks included. */
    static ClusterService komoServiceShell() throws Exception {
        ClusterService cs = (ClusterService) alloc(ClusterService.class);
        LogChannel log = silentLog();
        set(cs, "logChannel", log);
        set(cs, "bapDistanceFormatter", new BAPDistanceFormatter(log));
        set(cs, "clusterViewMode", alloc(ViewStub.class));
        set(cs, "galLock", new Object());
        return cs;
    }

    static CarPlayKOMOService komo(Caller caller) throws Exception {
        CarPlayKOMOService k = (CarPlayKOMOService) alloc(CarPlayKOMOService.class);
        set(k, "fieldLock", new Object());
        set(k, "visibilityLock", new Object());
        set(k, "logChannel", silentLog());
        set(k, "komoCaller", caller);
        set(k, "service", komoServiceShell());
        set(k, "clusterKDKHandler", Proxy.newProxyInstance(MostArrowsTest.class.getClassLoader(),
            new Class<?>[]{de.audi.tghu.navi.app.cluster.ClusterKDKHandler.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) { return null; }
            }));
        return k;
    }

    static Caller caller() throws Exception {
        Caller c = (Caller) alloc(Caller.class);
        c.calls = new ArrayList<String>();
        return c;
    }

    /** Stock's KOMO text is held while CarPlay guides and replayed, newest value, on release. */
    static void komoOwnershipAndReplay() throws Exception {
        Caller caller = caller();
        CarPlayKOMOService k = komo(caller);
        k.setDistanceToNextManeuver(500L, 1, true);
        k.setTurnToStreet("Old Rd", "");
        check(caller.drain().size() == 2, "stock writes pass while stock owns the fields");

        k.setCarPlayOwned(true);
        k.setDistanceToNextManeuver(300L, 1, true);
        k.setCurrentStreet("Stock St");
        check(caller.drain().isEmpty(), "stock writes are held while CarPlay owns the fields");

        Method dist = CarPlayKOMOService.class.getDeclaredMethod("carPlayDistanceToNextManeuver", Long.TYPE, Integer.TYPE, Boolean.TYPE);
        dist.setAccessible(true);
        dist.invoke(k, Long.valueOf(120L), Integer.valueOf(1), Boolean.TRUE);
        dist.invoke(k, Long.valueOf(120L), Integer.valueOf(1), Boolean.TRUE);
        List<String> c = caller.drain();
        check(c.size() == 1 && c.get(0).equals("dist 120 1 true"), "CarPlay distance goes out once per change: " + c);

        Method rtt = CarPlayKOMOService.class.getDeclaredMethod("carPlayRtt", Short.TYPE, Short.TYPE, Boolean.TYPE);
        rtt.setAccessible(true);
        rtt.invoke(k, Short.valueOf((short) 1), Short.valueOf((short) 5), Boolean.TRUE);
        rtt.invoke(k, Short.valueOf((short) 1), Short.valueOf((short) 5), Boolean.TRUE);
        c = caller.drain();
        check(c.size() == 1 && c.get(0).equals("rtt 1:5 true"), "CarPlay remaining time goes out once per change: " + c);

        k.setCarPlayOwned(false);
        c = caller.drain();
        check(c.contains("dist 300 1 true"), "release replays stock's newest distance: " + c);
        check(c.contains("turn [Old Rd][]"), "release replays stock's turn-to street: " + c);
        check(c.contains("street [Stock St]"), "release replays a stock write made during CarPlay: " + c);
        check(c.contains("eta 0 0 0:0 false") && c.contains("dest -1 -1 false") && c.contains("rtt 0:0 false"),
            "fields stock never wrote are cleared with stock's invalid encodings, CarPlay's RTT included: " + c);
        dist.invoke(k, Long.valueOf(99L), Integer.valueOf(1), Boolean.TRUE);
        rtt.invoke(k, Short.valueOf((short) 2), Short.valueOf((short) 0), Boolean.TRUE);
        check(caller.drain().isEmpty(), "CarPlay cannot write once released");

        k.setCarPlayOwned(true);
        k.setRTT((short) 0, (short) 42, true);
        check(caller.drain().isEmpty(), "stock's remaining time is held while CarPlay owns the fields");
        k.setCarPlayOwned(false);
        check(caller.drain().contains("rtt 0:42 true"), "release replays stock's remaining time");
    }

    static ClusterService komoClusterService(CarPlayKOMOService k) throws Exception {
        ClusterService cs = komoServiceShell();
        set(cs, "komoService", k);
        set(k, "service", cs);
        return cs;
    }

    /** CarPlay values are formatted by stock's own distance and time helpers. */
    static void komoFormattingMatchesStock() throws Exception {
        Caller caller = caller();
        CarPlayKOMOService k = komo(caller);
        ClusterService cs = komoClusterService(k);
        cs.publishCarPlayKomoDistance(850, false);
        cs.publishCarPlayKomoGuidance("Main St", "High St", 0L, 60000L, 12400);
        check(caller.drain().isEmpty(), "nothing is published before CarPlay owns the fields");
        cs.setCarPlayKomoOwned(true);
        caller.drain();

        long arrivalMs = 1790000000000L;
        long remainingMs = (83L * 60L + 30L) * 1000L;             /* 1 h 23 min 30 s */
        cs.publishCarPlayKomoDistance(850, false);
        cs.publishCarPlayKomoGuidance("Main St", "High St", arrivalMs, remainingMs, 12400);
        List<String> c = caller.drain();

        BAPDistanceFormatter f = new BAPDistanceFormatter(silentLog());
        Method unit = ClusterService.class.getDeclaredMethod("convertBAP2KOMODistanceUnit", Integer.TYPE);
        unit.setAccessible(true);
        boolean systemUnit = Distance.getSystemUnit() == 1;
        BAPDistanceFormatter.BAPDistance turn = f.formatDistanceToTurn(850, systemUnit);
        BAPDistanceFormatter.BAPDistance dest = f.formatDistanceToDestination(12400, systemUnit);
        KOMOTime t = KOMOService.convertTimeToKOMO(arrivalMs);
        int timeFormat = KOMOService.convertTimeFormatToKOMO(de.audi.atip.metrics.DateMetric.timeFormat);
        String expectDist = "dist " + turn.getValue() + " " + unit.invoke(cs, Integer.valueOf(turn.getUnit())) + " true";
        String expectDest = "dest " + dest.getValue() + " " + unit.invoke(cs, Integer.valueOf(dest.getUnit())) + " true";
        String expectEta = "eta " + timeFormat + " " + t.day + " " + t.hour + ":" + t.min + " true";
        check(c.contains(expectDist), "maneuver distance formatted as stock (" + expectDist + "): " + c);
        check(c.contains(expectDest), "destination distance formatted as stock (" + expectDest + "): " + c);
        check(c.contains(expectEta), "arrival time converted as stock (" + expectEta + "): " + c);
        check(c.contains("turn [Main St][]") && c.contains("street [High St]"), "streets passed through: " + c);
        KOMOTime r = KOMOService.convertDurationToKOMO(remainingMs);
        check(c.contains("rtt " + r.hour + ":" + r.min + " true") && r.hour == 1 && r.min == 23,
            "remaining time converted as stock (convertDurationToKOMO): " + c);

        BAPDistanceFormatter.BAPDistance near = f.formatDistanceToTurn(150, systemUnit);
        cs.publishCarPlayKomoDistance(150, true);
        c = caller.drain();
        check(c.contains("dist " + near.getValue() + " " + unit.invoke(cs, Integer.valueOf(near.getUnit())) + " false"),
            "with the approach bargraph shown the number is invalid, as stock (!showBargraph): " + c);

        cs.publishCarPlayKomoDistance(0, false);
        cs.publishCarPlayKomoGuidance("", "", -1L, -1L, 0);
        c = caller.drain();
        check(c.contains("dist -1 255 false") && c.contains("dest -1 -1 false") && c.contains("eta " + timeFormat + " 0 0:0 false")
            && c.contains("rtt 0:0 false"), "unknown values use stock's invalid encodings: " + c);
        check(java.lang.reflect.Modifier.isVolatile(ClusterService.class.getDeclaredField("rgiDataValid").getModifiers()),
            "rgiDataValid is volatile: refreshRGIValid also runs on the CarPlay thread");
    }

    /** BAPBridge owns the KOMO text from RGI start to route end, and only on a MOST cluster. */
    static void bapBridgeClaimPublishRelease() throws Exception {
        Method claim = BAPBridge.class.getDeclaredMethod("claimKomoGuidance", Boolean.TYPE);
        claim.setAccessible(true);
        Method publish = BAPBridge.class.getDeclaredMethod("publishKomoGuidance", RouteGuidance.State.class);
        publish.setAccessible(true);

        RouteGuidance.State s = new RouteGuidance.State();
        s.maneuverCount = 1;
        s.maneuverOrder = new int[]{0};
        s.mType[0] = ManeuverMapper.MT_LEFT_TURN;
        s.mAfterRoad[0] = "Station  Road";
        s.currentRoad = "Market St";
        s.distManeuverM = 240;

        int[] modes = {2, 0, 1};
        for (int i = 0; i < modes.length; i++) {
            installCarPlayFramework(framework(modes[i]));
            Caller caller = caller();
            CarPlayKOMOService k = komo(caller);
            ClusterService cs = komoClusterService(k);
            BAPBridge bridge = new BAPBridge();
            set(bridge, "csRef", cs);
            set(bridge, "hasLastDistM", Boolean.TRUE);      /* the BAP distance already on the cluster */
            set(bridge, "lastDistM", Integer.valueOf(240));
            set(bridge, "lastBarOn", Boolean.FALSE);
            claim.invoke(bridge, Boolean.TRUE);
            publish.invoke(bridge, s);
            List<String> c = caller.drain();
            if (modes[i] != 1) {
                check(!k.isCarPlayOwned() && c.isEmpty(), "541=" + modes[i] + ": KOMO untouched off MOST: " + c);
                continue;
            }
            check(k.isCarPlayOwned(), "MOST: RGI start claims KOMO");
            check(c.contains("turn [Station Road][]") && c.contains("street [Market St]"),
                "MOST: CarPlay maneuver street and road published: " + c);
            BAPDistanceFormatter.BAPDistance d240 = new BAPDistanceFormatter(silentLog())
                .formatDistanceToTurn(240, Distance.getSystemUnit() == 1);
            Method unit = ClusterService.class.getDeclaredMethod("convertBAP2KOMODistanceUnit", Integer.TYPE);
            unit.setAccessible(true);
            String numeric = "dist " + d240.getValue() + " " + unit.invoke(cs, Integer.valueOf(d240.getUnit()));
            check(c.contains(numeric + " true"), "MOST: the claim publishes the BAP distance already sent: " + c);
            Method sent = BAPBridge.class.getDeclaredMethod("publishKomoDistanceLocked", Integer.TYPE, Boolean.TYPE);
            sent.setAccessible(true);
            sent.invoke(bridge, Integer.valueOf(240), Boolean.TRUE);
            check(caller.drain().contains(numeric + " false"), "MOST: KOMO number follows the BAP bargraph (hidden while shown)");
            k.setTurnToStreet("", "");
            check(caller.drain().isEmpty(), "MOST: stock cannot overwrite CarPlay text meanwhile");
            claim.invoke(bridge, Boolean.FALSE);
            c = caller.drain();
            check(!k.isCarPlayOwned() && c.contains("turn [][]"), "MOST: release hands back stock's value: " + c);
        }
    }

    public static void main(String[] args) throws Exception {
        Log.setLevel(-1);
        freshOutputRequest();
        rendererOutputRequestFile();
        arrowsViewReachesTheDisplayService();
        streamRateLoggedAndRaisedForArrows();
        stockListenerSeesItsOwnContext();
        bufferedReplayAfterDisplayServiceRestart();
        screenModuleActivation();
        sessionEdgeDrivesTheDisplayService();
        lateStartAfterDisconnect();
        prefetchOnConnect();
        placementSizeAndRefresh();
        composesOnlyOnceRendererPresents();
        readinessEdges();
        windowTokenAndQuietWait();
        reissuedCarPlayContextMeansStockArrows();
        platformResolvedOnce();
        komoOwnershipAndReplay();
        komoFormattingMatchesStock();
        bapBridgeClaimPublishRelease();
        komoClaimIsAtomic();
        System.out.println("MostArrowsTest: CarPlay maneuver in the MOST arrows view (ctx 73 -> 81, 98 sized to the KOMO stream), "
            + "KOMO guidance text owned/replayed and stock-formatted PASS (" + checks + " checks)");
    }
}
