/*
 * MostPresentation - CarPlay route guidance in the arrows view of a MOST cluster.
 *
 * On a cluster coded for map/KDK over MOST (sysConst 541 == 1) the stock head unit offers two
 * views (ClusterInputListener.validateViewMode): MAP and KDK.  KDK is the "arrows" screen: the
 * navigation's own maneuver image, displayable 20, streamed to the cluster as KOMO video in
 * terminal-1 display context 73 (MapUtils: kombi context 9 -> 73).  The cluster draws the KOMO
 * text fields (distance, street, ETA) itself; those are fed by CarPlayKOMOService.
 *
 * While CarPlay route guidance is active this class makes the DisplayManager compose context
 * 81 = {98} - CarPlay's maneuver_render window - wherever stock asks for 73.  The MOST encoder
 * streams that leading displayable as-is at the KOMO view size (stock KDK 20's extents, 800x252
 * on this car), so the renderer is asked to make window 98 exactly that size and fits its scene
 * inside (25 Sept 2026: a 328x181 window streamed blank).  Everything else stays stock:
 * ClusterViewMode still chooses the view, KOMO still selects it on the cluster (setRgSelect),
 * stock still sets the frame rate.  Deactivation re-issues the stock request, so 73 and
 * displayable 20 return.
 *
 * Renderer handshake (protocol.h CR_MOST_OUTPUT_PATH / CR_MOST_OUTPUT_READY_PATH): Java writes
 * the size it needs; the renderer recreates 98 at that size and reports the size it presents.
 * 81 is composed, and 98 cropped, only once that report matches the request actually written,
 * so 98 is never recreated or cropped past its buffer while the MOST stream shows it.  A
 * changed size while composed hands the view back to stock 73 and re-composes after the
 * report: a real 73 -> 81 switch, which is what re-points the MOST encoder at the new window.
 * An unconfirmed size is composed once after READY_WAIT_MS, for diagnosis.
 *
 * Threading: every DisplayManager call made here runs on the HMI event thread, posted as a stock
 * RunnableEvent (ContextOwner.runOnHmiThread) - the thread stock's own terminal-1 switches and
 * the DSI extents answers use.  CarPlay threads only record the request; the HMI-thread pass
 * converges to "requested AND the session is still connected", so a late start after a
 * disconnect cannot compose 81.
 */
package com.luka.carplay.cluster;

import com.luka.carplay.framework.Log;
import de.audi.tghu.fwhmi.IDisplayManagerKombiControl;

public final class MostPresentation {
    private static final String TAG = "MostArrows";

    public static final int TERMINAL_CLUSTER = com.luka.carplay.core.ScreenModule.TERMINAL_CLUSTER;
    public static final int CTX_STOCK_KDK = 73;      /* stock arrows view: {20, 102, 101}; 101/102 absent on MOST */
    public static final int CTX_CARPLAY_KDK = 81;    /* DisplayManagerMIB2High dc[81] = {98} */
    private static final int MANEUVER = 98;           /* maneuver_render managed window */
    private static final int STOCK_KDK = 20;          /* stock navigation KDK image */
    /* maneuver_render content: protocol.h CR_DEFAULT_WIDTH x (CR_DEFAULT_HEIGHT - 1 ECC row).
     * On MOST the renderer fits its whole 328x181 frame (ECC row included) into window 98. */
    static final int SRC_W = 328;
    static final int SRC_H = 180;
    private static final int EXTENTS_RETRIES = 30;
    private static final long EXTENTS_POLL_MS = 100L;
    /* The renderer checks the request every 1 s; a backoff-suppressed recreate completes on
     * its 5 s health tick when idle.  Wait past that worst case before composing anyway. */
    public static final long READY_WAIT_MS = 7000L;
    private static final long READY_POLL_MS = 100L;
    private static final long WINDOW_WATCH_MS = 500L;
    /* protocol.h CR_OUTPUT_MIN / CR_OUTPUT_MAX: the renderer refuses anything else. */
    private static final int OUTPUT_MIN = 64;
    private static final int OUTPUT_MAX = 2048;

    /** Implemented by DisplayManagerMIB2High.  All but runOnHmiThread are HMI-thread only. */
    public interface ContextOwner {
        /** Queue work on the HMI event thread (stock RunnableEvent). */
        void runOnHmiThread(Runnable work);
        /** Extents the display service already reported for a displayable, or null. */
        int[] cachedExtents(int displayable);
        /** Ask the display service for a displayable's extents; the answer fills the cache. */
        void requestExtents(int displayable);
        /** Re-issue stock's last terminal-1 context request. */
        void reapplyClusterContext();
    }

    private static final Object LOCK = new Object();
    private static IDisplayManagerKombiControl dm;
    private static ContextOwner owner;
    private static boolean requested;                 /* CarPlay route guidance wants the arrows view */
    private static int generation;
    private static volatile boolean applied;          /* HMI thread: what DisplayManager composes */
    private static volatile boolean placedFromExtents;
    private static String lastPlacement = "none";
    private static String placedOutputRequest;        /* the request 98 is cropped for */
    private static String waitingOutputRequest;       /* the request a ready waiter watches */
    private static int readyWaitSerial;               /* retires superseded waiters */
    private static boolean readyWaitExpired;          /* waitingOutputRequest timed out */
    private static String composedToken;              /* the renderer window 81 was composed over */
    private static int composeSerial;                 /* retires superseded window watchers */
    private static int extentsRequestedGen = -1;      /* HMI thread: one DSI request per route */

    /* The MOST encoder streams the context's leading displayable as-is at the KOMO view size
     * (KVS_Most 800x252 here; stock KDK 20 measures exactly that), so window 98 must BE that
     * size.  Default until stock KDK's extents are known. */
    public static final int KVS_MOST_W = 800;
    public static final int KVS_MOST_H = 252;
    static String outputRequestPath = "/tmp/carplay_most_output";
    static String outputReadyPath = "/tmp/carplay_most_output_ready";
    private static String lastOutputRequest;          /* written and not since failed */
    private static boolean outputWritten;
    private static String lastOutputFailure;          /* logged once per distinct failure */
    private static final Object OUTPUT_WRITE_LOCK = new Object();   /* serializes the file write only */

    private MostPresentation() { }

    public static void bind(IDisplayManagerKombiControl displayManager, ContextOwner contextOwner) {
        synchronized (LOCK) {
            dm = displayManager;
            owner = contextOwner;
        }
    }

    /** True while the DisplayManager composes CarPlay's maneuver for the arrows view. */
    public static boolean isActive() { return applied; }

    /** The context the DisplayManager composes for a stock terminal-1 request. */
    public static int substitute(int terminal, int ctx) {
        return terminal == TERMINAL_CLUSTER && ctx == CTX_STOCK_KDK && applied ? CTX_CARPLAY_KDK : ctx;
    }

    /** CarPlay route guidance started/stopped on a MOST cluster (ScreenModule). */
    public static void setActive(boolean on) {
        ContextOwner o;
        boolean changed;
        synchronized (LOCK) {
            changed = requested != on;
            requested = on;
            if (changed) generation++;
            if (!on) retireReadyWaitLocked();
            o = owner;
        }
        if (changed) {
            Log.i(TAG, on ? "CarPlay route guidance: arrows view requested" : "CarPlay route guidance ended: arrows view released");
        }
        /* Also when unchanged but not yet converged (e.g. a start that raced a disconnect). */
        if (changed || applied != on) {
            post(o, new Runnable() { public void run() { converge(); } });
        }
    }

    /** CarPlay connected on a MOST cluster: warm the extents cache and size the renderer's
     *  window for the KOMO stream before any route starts (it checks every second). */
    public static void prefetchExtents() {
        final ContextOwner o;
        synchronized (LOCK) { o = owner; }
        post(o, new Runnable() {
            public void run() {
                int[] extents = null;
                try {
                    extents = o.cachedExtents(STOCK_KDK);
                    if (extents == null) o.requestExtents(STOCK_KDK);
                } catch (Throwable t) { }
                requestRendererOutput(known(extents) ? extents[0] : KVS_MOST_W,
                                      known(extents) ? extents[1] : KVS_MOST_H);
            }
        });
    }

    /** DisplayManagerMIB2High, on the switching thread, right after it composed ctx 81. */
    public static void onCarPlayContextApplied() {
        IDisplayManagerKombiControl d;
        ContextOwner o;
        synchronized (LOCK) { d = dm; o = owner; }
        if (d != null && o != null && applied) refreshPlacement(d, o);
    }

    private static void post(ContextOwner o, Runnable work) {
        if (o == null) return;
        try { o.runOnHmiThread(work); }
        catch (Throwable t) { Log.w(TAG, "HMI post failed: " + t); }
    }

    private static void postConverge(ContextOwner o) {
        post(o, new Runnable() { public void run() { converge(); } });
    }

    /** HMI thread: converge the composition to "requested and still connected", with 98 at the
     *  size the renderer reports presenting. */
    static void converge() {
        IDisplayManagerKombiControl d;
        ContextOwner o;
        boolean want;
        int gen;
        synchronized (LOCK) {
            d = dm;
            o = owner;
            want = requested;
            gen = generation;
        }
        want = want && com.luka.carplay.core.ScreenModule.isConnected();
        if (d == null || o == null) return;

        if (!want) {
            cancelReadyWait();
            if (!applied) return;
            applied = false;
            Log.w(TAG, "arrows view: stock KDK restored (ctx 73)");
            try { o.reapplyClusterContext(); }
            catch (Throwable t) { Log.w(TAG, "context reapply failed: " + t); }
            /* Hidden only after stock's 73 is back, so the arrows view never shows an empty ctx 81;
             * the next start's placement is logged afresh. */
            try { d.setOpacity(MANEUVER, TERMINAL_CLUSTER, 0); } catch (Throwable t) { }
            synchronized (LOCK) {
                lastPlacement = "none";
                placedOutputRequest = null;
                composeSerial++;                      /* retires the window watcher */
            }
            ClusterStateTrace.dump("most-arrows-off");
            return;
        }
        if (applied) {
            refreshPlacement(d, o);
            return;
        }

        int[] extents = null;
        try {
            extents = o.cachedExtents(STOCK_KDK);
            /* Once per route: re-runs while waiting for the renderer only read the cache. */
            if (extents == null && extentsRequestedGen != gen) {
                extentsRequestedGen = gen;
                o.requestExtents(STOCK_KDK);
            }
        } catch (Throwable t) { }
        int w = known(extents) ? extents[0] : KVS_MOST_W;
        int h = known(extents) ? extents[1] : KVS_MOST_H;
        String request = outputLine(w, h);
        requestRendererOutput(w, h);
        String token = readyToken(request);
        if (token == null && !expiredFor(request)) {
            startReadyWait(o, gen, request, w, h);
            return;
        }
        if (token != null) cancelReadyWait();

        applied = true;
        startWindowWatch(o, gen, token);
        /* The stock DisplayManager caches every displayable's opacity as 100 from boot and
         * drops a setOpacity equal to the cache, so a plain 100 would never reach the display
         * service for 98 on this cluster.  0 first (still composed in stock ctx 73, so not
         * visible) makes the following 100 a real write. */
        try { d.setOpacity(MANEUVER, TERMINAL_CLUSTER, 0); } catch (Throwable t) { }
        placedFromExtents = false;
        place(d, o, true);
        if (!placedFromExtents) startExtentsRetry(o, gen);
        /* WARN: one line per composition edge, visible without the verbose marker. */
        Log.w(TAG, "arrows view: CarPlay maneuver composed (ctx 73 -> 81)");
        try { o.reapplyClusterContext(); }
        catch (Throwable t) { Log.w(TAG, "context reapply failed: " + t); }
        /* After the switch: the terminal-1 context and stock's view state (gfxAvailable,
         * komoView*, favored/current view), which decide whether the arrows view is reachable. */
        ClusterStateTrace.dump("most-arrows-on");
    }

    /** Extents the renderer can present (protocol.h range); anything else is "unknown". */
    private static boolean known(int[] extents) {
        return extents != null && extents.length >= 2
            && extents[0] >= OUTPUT_MIN && extents[1] >= OUTPUT_MIN
            && extents[0] <= OUTPUT_MAX && extents[1] <= OUTPUT_MAX;
    }

    /** The display service answered with a real size (it reports 0x0 until it knows). */
    private static boolean reported(int[] extents) {
        return extents != null && extents.length >= 2 && extents[0] > 0 && extents[1] > 0;
    }

    /** Why known() refused stock's extents: never reported, or outside the renderer's range. */
    private static String describeExtents(int[] extents) {
        if (!reported(extents)) return "KDK extents unknown";
        return "stock KDK " + extents[0] + "x" + extents[1] + " outside the renderer's "
            + OUTPUT_MIN + "-" + OUTPUT_MAX + " range";
    }

    private static String fourDigits(int v) {
        String s = Integer.toString(v);
        StringBuffer b = new StringBuffer();
        for (int i = s.length(); i < 4; i++) b.append('0');
        return b.append(s).toString();
    }

    /** "%04d %04d\n": fixed width, so a rewrite is whole even if truncation misbehaves. */
    private static String outputLine(int w, int h) {
        return fourDigits(w) + " " + fourDigits(h) + "\n";
    }

    /** Ask the renderer for a w x h window, written in place in one write.  This unit's /tmp
     *  is procnto shared memory, which cannot rename (car log, 25 Sept 2026: "rename
     *  failed"); the renderer acts only on a complete line, so a read racing this write is
     *  ignored, never misread.  A failed write is retried on the next call and logged once
     *  per distinct failure. */
    static void requestRendererOutput(int w, int h) {
        String line = outputLine(w, h);
        String logLine = null;
        /* The file write is serialized on its own lock, never under LOCK, so a slow /tmp or
         * logger never blocks setActive() or a ClusterStateTrace dump. */
        synchronized (OUTPUT_WRITE_LOCK) {
            String path;
            synchronized (LOCK) {
                if (line.equals(lastOutputRequest) && outputWritten) return;
                lastOutputRequest = null;             /* in flight: nothing counts as written */
                outputWritten = false;
                path = outputRequestPath;
            }
            java.io.FileOutputStream out = null;
            Throwable failure = null;
            try {
                out = new java.io.FileOutputStream(path);
                out.write(line.getBytes("US-ASCII"));
                out.close();
                out = null;
            } catch (Throwable t) {
                failure = t;
            } finally {
                if (out != null) { try { out.close(); } catch (Throwable t) { } }
            }
            synchronized (LOCK) {
                if (failure == null) {
                    lastOutputRequest = line;
                    outputWritten = true;
                    lastOutputFailure = null;
                    logLine = "renderer output " + w + "x" + h + " requested (MOST KOMO stream)";
                } else {
                    String why = String.valueOf(failure);
                    if (!why.equals(lastOutputFailure)) {
                        lastOutputFailure = why;
                        logLine = "renderer output request failed (retried quietly): " + why;
                    }
                }
            }
        }
        if (logLine != null) Log.w(TAG, logLine);
    }

    /** The token of the renderer window presenting exactly the request that was written, or
     *  null: a stale report cannot confirm a request that never reached the renderer.  The
     *  report is the request's size plus " <pid>.<serial>" (protocol.h), new for every window;
     *  a bare size (earlier renderers) confirms with an empty token. */
    private static String readyToken(String request) {
        String path;
        synchronized (LOCK) {
            if (!outputWritten || !request.equals(lastOutputRequest)) return null;
            path = outputReadyPath;
        }
        java.io.FileInputStream in = null;
        try {
            in = new java.io.FileInputStream(path);
            byte[] b = new byte[64];
            int n = 0, r;
            while (n < b.length && (r = in.read(b, n, b.length - n)) > 0) n += r;
            String report = new String(b, 0, n, "US-ASCII");
            if (report.equals(request)) return "";
            String size = request.substring(0, request.length() - 1) + " ";
            if (n >= b.length || !report.startsWith(size) || !report.endsWith("\n")) return null;
            String token = report.substring(size.length(), report.length() - 1);
            if (token.length() == 0) return null;
            for (int i = 0; i < token.length(); i++) {
                char c = token.charAt(i);
                if ((c < '0' || c > '9') && c != '.') return null;
            }
            return token;
        } catch (Throwable t) {
            return null;
        } finally {
            if (in != null) { try { in.close(); } catch (Throwable t) { } }
        }
    }

    private static boolean outputReady(String request) {
        return readyToken(request) != null;
    }

    private static boolean expiredFor(String request) {
        synchronized (LOCK) { return readyWaitExpired && request.equals(waitingOutputRequest); }
    }

    private static void retireReadyWaitLocked() {
        waitingOutputRequest = null;
        readyWaitExpired = false;
        readyWaitSerial++;
    }

    private static void cancelReadyWait() {
        synchronized (LOCK) { retireReadyWaitLocked(); }
    }

    private static void cancelWaitForOtherOutput(String request) {
        synchronized (LOCK) {
            if (waitingOutputRequest != null && !request.equals(waitingOutputRequest)) retireReadyWaitLocked();
        }
    }

    /** Watch for the renderer's report of `request`; converge on the HMI thread once it appears
     *  (converge re-reads it there, so a torn report is not taken), or once READY_WAIT_MS
     *  passes (composed once, for diagnosis).  One waiter per request; a newer request, the
     *  route ending or the phone disconnecting retires it. */
    private static void startReadyWait(final ContextOwner o, final int gen, final String request,
                                       final int w, final int h) {
        final int serial;
        synchronized (LOCK) {
            if (request.equals(waitingOutputRequest)) return;
            waitingOutputRequest = request;
            readyWaitExpired = false;
            serial = ++readyWaitSerial;
        }
        Log.w(TAG, "arrows view: waiting for the renderer's " + w + "x" + h + " window");
        Thread waiter = new Thread(new Runnable() {
            public void run() {
                long start = System.currentTimeMillis();
                while (System.currentTimeMillis() - start < READY_WAIT_MS) {
                    try { Thread.sleep(READY_POLL_MS); } catch (InterruptedException e) { return; }
                    synchronized (LOCK) {
                        if (generation != gen || !requested || readyWaitSerial != serial) return;
                    }
                    if (!com.luka.carplay.core.ScreenModule.isConnected()) {
                        synchronized (LOCK) {
                            if (generation == gen && readyWaitSerial == serial) retireReadyWaitLocked();
                        }
                        return;
                    }
                    /* Retries a failed request write here, off the HMI thread; the HMI thread is
                     * only involved once the report is there (or the wait expires). */
                    requestRendererOutput(w, h);
                    if (outputReady(request)) {
                        synchronized (LOCK) {
                            if (generation != gen || readyWaitSerial != serial) return;
                            retireReadyWaitLocked();
                        }
                        postConverge(o);
                        return;
                    }
                }
                if (!com.luka.carplay.core.ScreenModule.isConnected()) return;
                synchronized (LOCK) {
                    if (generation != gen || !requested || readyWaitSerial != serial) return;
                    readyWaitExpired = true;
                }
                Log.w(TAG, "renderer output not confirmed within " + READY_WAIT_MS
                    + " ms; composing once for diagnosis");
                postConverge(o);
            }
        }, "carplay-most-ready");
        waiter.setDaemon(true);
        waiter.start();
    }

    /** HMI thread (or the DisplayManager's switching thread, via onCarPlayContextApplied):
     *  keep 98 in step with the size the arrows view needs.  A different size than 98 was
     *  cropped for means 98 is recreated: the view is handed back to stock's 73 and 81 is
     *  re-composed once the renderer reports the new size - even if it already has, as only a
     *  real 73 -> 81 switch re-points the MOST encoder - and 98 is never cropped past its
     *  buffer meanwhile. */
    private static void refreshPlacement(IDisplayManagerKombiControl d, ContextOwner o) {
        try {
            int[] extents = o.cachedExtents(STOCK_KDK);
            int w = known(extents) ? extents[0] : KVS_MOST_W;
            int h = known(extents) ? extents[1] : KVS_MOST_H;
            String request = outputLine(w, h);
            requestRendererOutput(w, h);
            String placed;
            synchronized (LOCK) { placed = placedOutputRequest; }
            cancelWaitForOtherOutput(request);
            if (!request.equals(placed)) {
                postHandBack(o, "renderer output changes to " + w + "x" + h);
                return;
            }
            if (outputReady(request)) cancelReadyWait();
            place(d, o, false);
        } catch (Throwable t) {
            Log.w(TAG, "placement refresh failed: " + t);
        }
    }

    /** Hand the arrows view back to stock's 73 and re-compose 81 (a real context switch, which
     *  re-points the MOST encoder at a recreated window 98).  Posted: callers include the
     *  DisplayManager's own switchContext tail and the window watcher, and a nested switch
     *  must not run inside another. */
    private static void postHandBack(final ContextOwner o, final String reason) {
        post(o, new Runnable() {
            public void run() {
                if (!applied) return;
                applied = false;
                synchronized (LOCK) {
                    lastPlacement = "none";
                    placedOutputRequest = null;
                    composeSerial++;                  /* retires the window watcher */
                }
                placedFromExtents = false;
                Log.w(TAG, "arrows view: stock KDK restored while " + reason);
                try { o.reapplyClusterContext(); }
                catch (Throwable t) { Log.w(TAG, "context reapply failed: " + t); }
                ClusterStateTrace.dump("most-arrows-wait");
                converge();                           /* re-composes now if already reported */
            }
        });
    }

    /** While 81 is composed, watch the renderer's report for 98's placed size: a new token
     *  means the renderer replaced the window on its own (swap failure, loss recovery, a
     *  restart, or a resize that completed after the wait expired), which the MOST encoder
     *  only follows through a real 73 -> 81 switch.  A withdrawn report (a recreation in
     *  progress) is waited out. */
    private static void startWindowWatch(final ContextOwner o, final int gen, final String token) {
        final int serial;
        synchronized (LOCK) {
            composedToken = token;
            serial = ++composeSerial;
        }
        Thread watcher = new Thread(new Runnable() {
            public void run() {
                while (true) {
                    try { Thread.sleep(WINDOW_WATCH_MS); } catch (InterruptedException e) { return; }
                    String placed;
                    String composed;
                    synchronized (LOCK) {
                        if (generation != gen || !requested || composeSerial != serial) return;
                        placed = placedOutputRequest;
                        composed = composedToken;
                    }
                    if (!applied || placed == null) continue;
                    String now = readyToken(placed);
                    if (now != null && !now.equals(composed)) {
                        postHandBack(o, "the renderer replaced its window");
                        return;
                    }
                }
            }
        }, "carplay-most-window");
        watcher.setDaemon(true);
        watcher.start();
    }

    /** HMI thread.  force: write even when the placement is unchanged (the start edge, whose
     *  opacity-0 write must be followed by a real 100); otherwise an unchanged placement is not
     *  re-sent to the display service.  98 is the whole stream: identity cropping at its size. */
    private static void place(IDisplayManagerKombiControl d, ContextOwner o, boolean force) {
        try {
            int[] extents = o.cachedExtents(STOCK_KDK);
            boolean fromExtents = known(extents);
            int w = fromExtents ? extents[0] : KVS_MOST_W;
            int h = fromExtents ? extents[1] : KVS_MOST_H;
            requestRendererOutput(w, h);
            String placement = "window " + w + "x" + h + " from "
                + (fromExtents ? "stock KDK" : "KVS_Most default (" + describeExtents(extents) + ")");
            boolean changed;
            synchronized (LOCK) {
                changed = !placement.equals(lastPlacement);
                if (!changed && !force) return;
                placedOutputRequest = outputLine(w, h);
            }
            d.setCropping(MANEUVER, TERMINAL_CLUSTER, 0, 0, w, h, 0, 0, w, h);
            d.setOpacity(MANEUVER, TERMINAL_CLUSTER, 100);
            synchronized (LOCK) { lastPlacement = placement; }
            placedFromExtents = fromExtents;
            if (changed) Log.w(TAG, "maneuver " + placement);
        } catch (Throwable t) {
            Log.w(TAG, "placement failed: " + t);
        }
    }

    /** The extents answer arrives asynchronously on the HMI thread; re-place once it has.
     *  This thread only sleeps and queues HMI-thread checks - it never touches the DM. */
    private static void startExtentsRetry(final ContextOwner o, final int gen) {
        Thread waiter = new Thread(new Runnable() {
            public void run() {
                for (int i = 0; i < EXTENTS_RETRIES; i++) {
                    try { Thread.sleep(EXTENTS_POLL_MS); } catch (InterruptedException e) { return; }
                    synchronized (LOCK) {
                        if (generation != gen || !requested) return;
                    }
                    if (placedFromExtents) return;
                    post(o, new Runnable() {
                        public void run() {
                            IDisplayManagerKombiControl d;
                            synchronized (LOCK) {
                                if (generation != gen) return;
                                d = dm;
                            }
                            try {
                                if (d != null && applied && !placedFromExtents) refreshPlacement(d, o);
                            } catch (Throwable t) { }
                        }
                    });
                }
                if (!placedFromExtents) {
                    int[] extents = null;
                    try { extents = o.cachedExtents(STOCK_KDK); } catch (Throwable t) { }
                    Log.w(TAG, (!reported(extents) ? "stock KDK extents not reported within "
                            + (EXTENTS_RETRIES * EXTENTS_POLL_MS) + " ms"
                            : describeExtents(extents)) + "; keeping fallback placement");
                }
            }
        }, "carplay-most-extents");
        waiter.setDaemon(true);
        waiter.start();
    }

    public static String describe() {
        synchronized (LOCK) {
            return "mostArrows=" + (applied ? "carplay" : "stock") + (requested ? "/requested" : "")
                + " placement={" + lastPlacement + "}";
        }
    }
}
