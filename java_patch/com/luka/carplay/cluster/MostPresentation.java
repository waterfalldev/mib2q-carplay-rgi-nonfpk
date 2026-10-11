/*
 * MostPresentation - CarPlay pictures in the stock views of a MOST cluster.
 *
 * On a cluster coded for map/KDK over MOST (sysConst 541 == 1) the stock head unit offers two
 * views (ClusterInputListener.validateViewMode): MAP and KDK.  KDK is the "arrows" screen: the
 * navigation's own maneuver image, displayable 20, streamed to the cluster as KOMO video in
 * terminal-1 display context 73 (MapUtils: kombi context 9 -> 73).  MAP is the Audi map,
 * displayable 33 in context 72 (kombi context 8).  The cluster draws the KOMO text fields
 * (distance, street, ETA) itself; those are fed by CarPlayKOMOService.
 *
 * Each view is one View below: while it is requested, the DisplayManager composes CarPlay's
 * renderer window wherever stock asks for that view's context - arrows 73 -> 81 = {98}
 * (maneuver_render's scene) and MAP 72 -> 82 = {99} (the phone's cluster map, decoded by the
 * renderer).  CarPlay route guidance requests the arrows (setActive), and the MAP view too
 * while the phone's cluster stream is live (setMapLive, from ClusterVideo).  The MOST
 * encoder streams that leading displayable as-is at the stock source's size (KDK 20: 800x252,
 * MAP 33: 800x298 on this car), so the renderer is asked to make its window exactly that size
 * (25 Sept 2026: a 328x181 window streamed blank).  Everything else stays stock:
 * ClusterViewMode still chooses the view, KOMO still selects it on the cluster (setRgSelect),
 * stock still requests the frame rate.  Deactivation re-issues the stock request, so the stock
 * context and picture return.
 *
 * Renderer handshake (protocol.h CR_MOST_OUTPUT_PATH / CR_MOST_MAP_OUTPUT_PATH and their
 * _READY files): Java writes the size it needs; the renderer (re)creates the window at that
 * size and reports the size it presents plus a token new for every window.  A view is
 * composed, and its window cropped, only once that report matches the request actually
 * written, so a window is never recreated or cropped past its buffer while the MOST stream
 * shows it.  A changed size or a new window token while composed hands the view back to stock
 * and re-composes after the report: a real stock -> CarPlay switch, which is what re-points
 * the MOST encoder at the new window.
 *
 * The views differ only where the evidence does: the arrows fall back to KVS_Most 800x252
 * while stock KDK's extents are unknown, keep their window between routes and compose an
 * unconfirmed size once after READY_WAIT_MS, for diagnosis; the MAP view uses measured sizes
 * only (stock reports -1x-1 until its map is ready, map02: 14 s after connect), has a window
 * only while requested, and never composes an unconfirmed one.
 *
 * Threading: every DisplayManager call made here runs on the HMI event thread, posted as a stock
 * RunnableEvent (ContextOwner.runOnHmiThread) - the thread stock's own terminal-1 switches and
 * the DSI extents answers use.  CarPlay threads only record the request; the HMI-thread pass
 * converges to "requested AND the session is still connected", so a late start after a
 * disconnect cannot compose.
 */
package com.luka.carplay.cluster;

import com.luka.carplay.framework.Log;
import de.audi.tghu.fwhmi.IDisplayManagerKombiControl;

public final class MostPresentation {
    public static final int TERMINAL_CLUSTER = com.luka.carplay.core.ScreenModule.TERMINAL_CLUSTER;
    public static final int CTX_STOCK_KDK = 73;      /* stock arrows view: {20, 102, 101}; 101/102 absent on MOST */
    public static final int CTX_CARPLAY_KDK = 81;    /* DisplayManagerMIB2High dc[81] = {98} */
    public static final int CTX_STOCK_MAP = 72;      /* stock MAP view: {33} */
    public static final int CTX_CARPLAY_MAP = 82;    /* DisplayManagerMIB2High dc[82] = {99} */
    public static final int MAP_DISPLAYABLE = 99;    /* maneuver_render's map window */
    /* maneuver_render content: protocol.h CR_DEFAULT_WIDTH x (CR_DEFAULT_HEIGHT - 1 ECC row).
     * On MOST the renderer fits its whole 328x181 frame (ECC row included) into window 98. */
    static final int SRC_W = 328;
    static final int SRC_H = 180;
    private static final int EXTENTS_RETRIES = 30;
    private static final long EXTENTS_POLL_MS = 100L;
    /* A measured-only view keeps asking while its source is unknown, this often. */
    private static final long EXTENTS_REFRESH_MS = 5000L;
    /* The renderer checks the request every 1 s; a backoff-suppressed recreate completes on
     * its 5 s health tick when idle.  Wait past that worst case before composing anyway. */
    public static final long READY_WAIT_MS = 7000L;
    private static final long READY_POLL_MS = 100L;
    private static final long WINDOW_WATCH_MS = 500L;
    /* protocol.h CR_OUTPUT_MIN / CR_OUTPUT_MAX: the renderer refuses anything else. */
    private static final int OUTPUT_MIN = 64;
    private static final int OUTPUT_MAX = 2048;

    /* The MOST encoder streams the context's leading displayable as-is at the KOMO view size
     * (KVS_Most 800x252 here; stock KDK 20 measures exactly that), so window 98 must BE that
     * size.  Default until stock KDK's extents are known. */
    public static final int KVS_MOST_W = 800;
    public static final int KVS_MOST_H = 252;

    /** Implemented by DisplayManagerMIB2High.  All but runOnHmiThread are HMI-thread only. */
    public interface ContextOwner {
        /** Queue work on the HMI event thread (stock RunnableEvent). */
        void runOnHmiThread(Runnable work);
        /** Extents the display service already reported for a displayable, or null. */
        int[] cachedExtents(int displayable);
        /** Ask the display service for a displayable's extents afresh; the answer fills the
         *  cache, even over an earlier unknown (-1x-1 / 0x0) answer. */
        void requestExtents(int displayable);
        /** Re-issue stock's last terminal-1 context request. */
        void reapplyClusterContext();
        /** Re-send stock's last terminal-1 update rate through substituteRate, if that changes it. */
        void reapplyClusterRate();
    }

    /** One CarPlay picture replacing one stock view's picture.  The configuration is fixed;
     *  the state below it is guarded by LOCK unless marked HMI thread / volatile. */
    static final class View {
        final String tag;             /* log tag */
        final String label;           /* "arrows view" */
        final String content;         /* what CarPlay shows there: "maneuver" */
        final String sourceName;      /* stock's source in "... extents unknown": "KDK" */
        final String stockName;       /* stock's picture: "stock KDK" */
        final String trace;           /* ClusterStateTrace prefix */
        final int stockContext, composedContext, displayable, extentsSource;
        /* The size while stock's extents are unknown, or null: measured sizes only.  A view
         * with one keeps its window between routes and composes an unconfirmed one once. */
        final int[] fallback;
        String outputRequestPath, outputReadyPath;

        boolean requested;
        int generation;
        volatile boolean applied;         /* HMI thread: what DisplayManager composes */
        String lastPlacement = "none";
        String placedOutputRequest;       /* the request the window is cropped for */
        String waitingOutputRequest;      /* the request a ready waiter watches */
        int readyWaitSerial;              /* retires superseded waiters */
        boolean readyWaitExpired;         /* waitingOutputRequest timed out */
        String composedToken;             /* the renderer window the view was composed over */
        int composeSerial;                /* retires superseded window watchers */
        int extentsRequestedGen = -1;     /* HMI thread: one fresh DSI request per generation */
        int extentsPollGen = -1;          /* one extents poller per generation */
        String lastOutputRequest;         /* written and not since failed */
        String lastOutputFailure;         /* logged once per distinct failure */
        final Object outputWriteLock = new Object();   /* serializes the file write only */

        View(String tag, String label, String content, String sourceName, String trace,
             int stockContext, int composedContext, int displayable, int extentsSource,
             int[] fallback, String outputRequestPath, String outputReadyPath) {
            this.tag = tag; this.label = label; this.content = content;
            this.sourceName = sourceName; this.stockName = "stock " + sourceName; this.trace = trace;
            this.stockContext = stockContext; this.composedContext = composedContext;
            this.displayable = displayable; this.extentsSource = extentsSource;
            this.fallback = fallback;
            this.outputRequestPath = outputRequestPath; this.outputReadyPath = outputReadyPath;
        }
    }

    static final View ARROWS = new View("MostArrows", "arrows view", "maneuver", "KDK", "most-arrows",
        CTX_STOCK_KDK, CTX_CARPLAY_KDK, 98, 20, new int[]{ KVS_MOST_W, KVS_MOST_H },
        "/tmp/carplay_most_output", "/tmp/carplay_most_output_ready");
    static final View MAP = new View("MostMap", "MAP view", "map", "MAP", "most-map",
        CTX_STOCK_MAP, CTX_CARPLAY_MAP, MAP_DISPLAYABLE, 33, null,
        "/tmp/carplay_most_map_output", "/tmp/carplay_most_map_output_ready");
    private static final View[] VIEWS = { ARROWS, MAP };

    private static final Object LOCK = new Object();
    private static IDisplayManagerKombiControl dm;
    private static ContextOwner owner;

    private MostPresentation() { }

    public static void bind(IDisplayManagerKombiControl displayManager, ContextOwner contextOwner) {
        synchronized (LOCK) {
            dm = displayManager;
            owner = contextOwner;
        }
    }

    /** True while the DisplayManager composes CarPlay's maneuver for the arrows view. */
    public static boolean isActive() { return ARROWS.applied; }

    /** True while the DisplayManager composes CarPlay's picture for the MAP view. */
    public static boolean isMapActive() { return MAP.applied; }

    /** The context the DisplayManager composes for a stock terminal-1 request. */
    public static int substitute(int terminal, int ctx) {
        if (terminal != TERMINAL_CLUSTER) return ctx;
        for (int i = 0; i < VIEWS.length; i++) {
            if (ctx == VIEWS[i].stockContext && VIEWS[i].applied) return VIEWS[i].composedContext;
        }
        return ctx;
    }

    /** Stock code that re-issues getCurrentContextID(1) reads a composed context; that request
     *  means the view's stock context (81 -> 73, 82 -> 72).  Non-G24 cluster contexts only. */
    public static int logicalContext(int ctx) {
        for (int i = 0; i < VIEWS.length; i++) {
            if (ctx == VIEWS[i].composedContext) return VIEWS[i].stockContext;
        }
        return ctx;
    }

    /* Stock streams terminal 1 at 10 fps when the cluster reports KOMO data rate 2 and at
     * 1 fps at data rate 1 (measured in v20).  While CarPlay's arrows are composed on a MOST
     * cluster, stock's full rate becomes CARPLAY_RATE (the Virtual Cockpit's rate) in every view
     * it streams; while the phone's map is composed, CARPLAY_MAP_RATE: at 30 the cluster kept
     * dropping its KOMO data rate to 1 on the map view (map24, map25: 38 s at 1 fps), and 60
     * showed about a picture a second (map24).  15 takes every second picture of the phone's 30.
     * 1 and 0 pass through: the rate only rises where stock already streams at full rate.
     * DisplayManagerMIB2High re-sends stock's last request on every composition edge, so a stock
     * session without CarPlay keeps stock's rate. */
    static final int STOCK_FULL_RATE = 10;
    static final int CARPLAY_RATE = 30;
    static final int CARPLAY_MAP_RATE = 15;

    /** The update rate the DisplayManager sends for a stock terminal request. */
    public static int substituteRate(int terminal, int rate) {
        if (terminal != TERMINAL_CLUSTER || rate != STOCK_FULL_RATE) return rate;
        if (MAP.applied) return CARPLAY_MAP_RATE;
        return ARROWS.applied ? CARPLAY_RATE : rate;
    }

    /* What the requests follow: CarPlay route guidance and a live cluster stream.  Changed only
     * under REQUESTS, so two callers can never apply them out of order. */
    private static final Object REQUESTS = new Object();
    private static boolean guiding, mapLive;

    /** CarPlay route guidance started/stopped on a MOST cluster (ScreenModule): the arrows view,
     *  and the MAP view while the phone's cluster stream is live. */
    public static void setActive(boolean on) {
        synchronized (REQUESTS) {
            guiding = on;
            if (setRequested(ARROWS, on)) {
                Log.i(ARROWS.tag, on ? "CarPlay route guidance: arrows view requested" : "CarPlay route guidance ended: arrows view released");
            }
            setMapRequested(guiding && mapLive);
        }
    }

    /** The phone's cluster stream became live or ended (ClusterVideo); outside guidance the
     *  Audi map stays - the phone streams its cluster display for the whole session (map14). */
    public static void setMapLive(boolean live) {
        synchronized (REQUESTS) {
            mapLive = live;
            setMapRequested(guiding && mapLive);
        }
    }

    private static void setMapRequested(boolean on) {
        if (setRequested(MAP, on)) {
            Log.w(MAP.tag, on ? "MAP view: CarPlay picture requested" : "MAP view: CarPlay picture released");
        }
    }

    /** Records the request and converges on the HMI thread; true when it changed. */
    private static boolean setRequested(final View v, boolean on) {
        ContextOwner o;
        boolean changed;
        synchronized (LOCK) {
            changed = v.requested != on;
            v.requested = on;
            if (changed) v.generation++;
            if (!on) retireReadyWaitLocked(v);
            o = owner;
        }
        /* Also when unchanged but not yet converged (e.g. a start that raced a disconnect). */
        if (changed || v.applied != on) postConverge(v, o);
        return changed;
    }

    /** CarPlay connected on a MOST cluster: warm the extents cache and size the renderer's
     *  arrows window for the KOMO stream before any route starts (it checks every second).
     *  The MAP view is only converged: its window exists only while that view is requested,
     *  so a request left by an earlier HMI is withdrawn. */
    public static void prefetchExtents() {
        final ContextOwner o;
        synchronized (LOCK) { o = owner; }
        post(o, new Runnable() {
            public void run() {
                int[] size = ARROWS.fallback;
                try {
                    int[] extents = o.cachedExtents(ARROWS.extentsSource);
                    if (extents == null) o.requestExtents(ARROWS.extentsSource);
                    size = size(ARROWS, extents);
                } catch (Throwable t) { }
                requestRendererOutput(ARROWS, size[0], size[1]);
                converge(MAP);
            }
        });
    }

    /** DisplayManagerMIB2High, on the switching thread, right after it composed `ctx`. */
    public static void onContextApplied(int ctx) {
        IDisplayManagerKombiControl d;
        ContextOwner o;
        synchronized (LOCK) { d = dm; o = owner; }
        for (int i = 0; i < VIEWS.length; i++) {
            View v = VIEWS[i];
            if (ctx == v.composedContext && d != null && o != null && v.applied) refreshPlacement(v, d, o);
        }
    }

    private static void post(ContextOwner o, Runnable work) {
        if (o == null) return;
        try { o.runOnHmiThread(work); }
        catch (Throwable t) { Log.w(ARROWS.tag, "HMI post failed: " + t); }
    }

    /** HMI thread, after a composition edge: stock's last context and rate requests, substituted
     *  anew. */
    private static void reapply(View v, ContextOwner o) {
        try { o.reapplyClusterContext(); }
        catch (Throwable t) { Log.w(v.tag, "context reapply failed: " + t); }
        try { o.reapplyClusterRate(); }
        catch (Throwable t) { Log.w(v.tag, "stream rate reapply failed: " + t); }
    }

    /** HMI thread: stock's picture back in a composed view. */
    private static void restoreStock(View v, ContextOwner o, String why, String traceSuffix) {
        v.applied = false;
        synchronized (LOCK) {
            v.lastPlacement = "none";                 /* the next start's placement is logged afresh */
            v.placedOutputRequest = null;
            v.composeSerial++;                        /* retires the window watcher */
        }
        Log.w(v.tag, v.label + ": " + v.stockName + " restored " + why);
        reapply(v, o);
        ClusterStateTrace.dump(v.trace + traceSuffix);
    }

    private static void postConverge(final View v, ContextOwner o) {
        post(o, new Runnable() { public void run() { converge(v); } });
    }

    /** HMI thread: converge a view to "requested and still connected", with its window at the
     *  size the renderer reports presenting. */
    static void converge(View v) {
        IDisplayManagerKombiControl d;
        ContextOwner o;
        boolean want;
        int gen;
        synchronized (LOCK) {
            d = dm;
            o = owner;
            want = v.requested;
            gen = v.generation;
        }
        want = want && com.luka.carplay.core.ScreenModule.isConnected();
        if (d == null || o == null) return;

        if (!want) {
            cancelReadyWait(v);
            if (v.applied) {
                restoreStock(v, o, "(ctx " + v.stockContext + ")", "-off");
                /* Hidden only after stock's context is back, so the view never shows an empty
                 * CarPlay context. */
                try { d.setOpacity(v.displayable, TERMINAL_CLUSTER, 0); } catch (Throwable t) { }
            }
            if (v.fallback == null) withdrawRendererOutput(v);
            return;
        }
        if (v.applied) {
            refreshPlacement(v, d, o);
            return;
        }

        int[] extents = null;
        try {
            extents = o.cachedExtents(v.extentsSource);
            /* Once per request: re-runs while waiting for the renderer only read the cache. */
            if (!known(extents) && v.extentsRequestedGen != gen) {
                v.extentsRequestedGen = gen;
                o.requestExtents(v.extentsSource);
            }
        } catch (Throwable t) { }
        int[] size = size(v, extents);
        if (size == null) {
            startExtentsPoll(v, o, gen);              /* measured-only: nothing to ask for yet */
            return;
        }
        String request = outputLine(size[0], size[1]);
        requestRendererOutput(v, size[0], size[1]);
        String token = readyToken(v, request);
        if (token == null && !expiredFor(v, request)) {
            startReadyWait(v, o, gen, request, size[0], size[1]);
            return;
        }
        if (token != null) cancelReadyWait(v);

        v.applied = true;
        startWindowWatch(v, o, gen, token);
        /* The stock DisplayManager caches every displayable's opacity as 100 from boot and
         * drops a setOpacity equal to the cache, so a plain 100 would never reach the display
         * service for our window on this cluster.  0 first (still in the stock context, so not
         * visible) makes the following 100 a real write. */
        try { d.setOpacity(v.displayable, TERMINAL_CLUSTER, 0); } catch (Throwable t) { }
        place(v, d, o, true);
        if (!known(extents)) startExtentsPoll(v, o, gen);   /* composed at the fallback */
        /* WARN: one line per composition edge, visible without the verbose marker. */
        Log.w(v.tag, v.label + ": CarPlay " + v.content + " composed (ctx " + v.stockContext
            + " -> " + v.composedContext + ")");
        reapply(v, o);
        /* After the switch: the terminal-1 context and stock's view state (gfxAvailable,
         * komoView*, favored/current view), which decide whether the view is reachable. */
        ClusterStateTrace.dump(v.trace + "-on");
    }

    /** Extents the renderer can present (protocol.h range); anything else is "unknown". */
    private static boolean known(int[] extents) {
        return extents != null && extents.length >= 2
            && extents[0] >= OUTPUT_MIN && extents[1] >= OUTPUT_MIN
            && extents[0] <= OUTPUT_MAX && extents[1] <= OUTPUT_MAX;
    }

    /** The display service answered with a real size (it reports 0x0 / -1x-1 until it knows). */
    private static boolean reported(int[] extents) {
        return extents != null && extents.length >= 2 && extents[0] > 0 && extents[1] > 0;
    }

    /** The window a view needs: the stock source's extents, else the view's fallback, else
     *  null (a measured-only view with an unknown source). */
    private static int[] size(View v, int[] extents) {
        return known(extents) ? new int[]{ extents[0], extents[1] } : v.fallback;
    }

    /** Why known() refused stock's extents: never reported, or outside the renderer's range. */
    private static String describeExtents(View v, int[] extents) {
        if (!reported(extents)) return v.sourceName + " extents unknown";
        return v.stockName + " " + extents[0] + "x" + extents[1] + " outside the renderer's "
            + OUTPUT_MIN + "-" + OUTPUT_MAX + " range";
    }

    private static String fourDigits(int v) {
        String s = Integer.toString(v);
        StringBuffer b = new StringBuffer();
        for (int i = s.length(); i < 4; i++) b.append('0');
        return b.append(s).toString();
    }

    /** "%04d %04d\n": fixed width, so a rewrite is whole even if truncation misbehaves. */
    static String outputLine(int w, int h) {
        return fourDigits(w) + " " + fourDigits(h) + "\n";
    }

    /** Ask the renderer for a w x h window, written in place in one write.  This unit's /tmp
     *  is procnto shared memory, which cannot rename (car log, 25 Sept 2026: "rename
     *  failed"); the renderer acts only on a complete line, so a read racing this write is
     *  ignored, never misread.  A failed write is retried on the next call and logged once
     *  per distinct failure. */
    static void requestRendererOutput(View v, int w, int h) {
        String line = outputLine(w, h);
        String logLine = null;
        /* The file write is serialized on its own lock, never under LOCK, so a slow /tmp or
         * logger never blocks setActive() or a ClusterStateTrace dump. */
        synchronized (v.outputWriteLock) {
            String path;
            synchronized (LOCK) {
                if (line.equals(v.lastOutputRequest)) return;
                v.lastOutputRequest = null;           /* in flight: nothing counts as written */
                path = v.outputRequestPath;
            }
            Throwable failure = null;
            try { RecordFiles.write(path, line); }
            catch (Throwable t) { failure = t; }
            synchronized (LOCK) {
                if (failure == null) {
                    v.lastOutputRequest = line;
                    v.lastOutputFailure = null;
                    logLine = "renderer output " + w + "x" + h + " requested (MOST KOMO stream)";
                } else {
                    String why = String.valueOf(failure);
                    if (!why.equals(v.lastOutputFailure)) {
                        v.lastOutputFailure = why;
                        logLine = "renderer output request failed (retried quietly): " + why;
                    }
                }
            }
        }
        if (logLine != null) Log.w(v.tag, logLine);
    }

    /** A view without a kept window removes its request, so the renderer releases the window
     *  (and withdraws its report).  Retried on the next converge if /tmp refuses. */
    private static void withdrawRendererOutput(View v) {
        String logLine = null;
        synchronized (v.outputWriteLock) {
            String path;
            synchronized (LOCK) {
                path = v.outputRequestPath;
                v.lastOutputRequest = null;
            }
            if (!RecordFiles.remove(path)) {
                String why = "delete refused";
                synchronized (LOCK) {
                    if (!why.equals(v.lastOutputFailure)) {
                        v.lastOutputFailure = why;
                        logLine = "renderer output withdrawal failed (retried quietly): " + why;
                    }
                }
            }
        }
        if (logLine != null) Log.w(v.tag, logLine);
    }

    /** The token of the renderer window presenting exactly the request that was written, or
     *  null: a stale report cannot confirm a request that never reached the renderer.  The
     *  report is the request's size plus " <pid>.<serial>" (protocol.h), new for every window;
     *  a bare size (earlier renderers) confirms with an empty token. */
    private static String readyToken(View v, String request) {
        String path;
        synchronized (LOCK) {
            if (!request.equals(v.lastOutputRequest)) return null;
            path = v.outputReadyPath;
        }
        String report = RecordFiles.read(path, 63);
        if (report == null) return null;
        if (report.equals(request)) return "";
        String size = request.substring(0, request.length() - 1) + " ";
        if (!report.startsWith(size) || !report.endsWith("\n")) return null;
        String token = report.substring(size.length(), report.length() - 1);
        if (token.length() == 0) return null;
        for (int i = 0; i < token.length(); i++) {
            char c = token.charAt(i);
            if ((c < '0' || c > '9') && c != '.') return null;
        }
        return token;
    }

    private static boolean outputReady(View v, String request) {
        return readyToken(v, request) != null;
    }

    private static boolean expiredFor(View v, String request) {
        synchronized (LOCK) { return v.readyWaitExpired && request.equals(v.waitingOutputRequest); }
    }

    private static void retireReadyWaitLocked(View v) {
        v.waitingOutputRequest = null;
        v.readyWaitExpired = false;
        v.readyWaitSerial++;
    }

    private static void cancelReadyWait(View v) {
        synchronized (LOCK) { retireReadyWaitLocked(v); }
    }

    private static void cancelWaitForOtherOutput(View v, String request) {
        synchronized (LOCK) {
            if (v.waitingOutputRequest != null && !request.equals(v.waitingOutputRequest)) retireReadyWaitLocked(v);
        }
    }

    /** Watch for the renderer's report of `request`; converge on the HMI thread once it appears
     *  (converge re-reads it there, so a torn report is not taken).  After READY_WAIT_MS the
     *  arrows compose once anyway, for diagnosis; the MAP view keeps waiting.  One waiter per
     *  request; a newer request, the view's release or the phone disconnecting retires it. */
    private static void startReadyWait(final View v, final ContextOwner o, final int gen,
                                       final String request, final int w, final int h) {
        final int serial;
        synchronized (LOCK) {
            if (request.equals(v.waitingOutputRequest)) return;
            v.waitingOutputRequest = request;
            v.readyWaitExpired = false;
            serial = ++v.readyWaitSerial;
        }
        Log.w(v.tag, v.label + ": waiting for the renderer's " + w + "x" + h + " window");
        Thread waiter = new Thread(new Runnable() {
            public void run() {
                long start = System.currentTimeMillis();
                boolean reportedWait = false;
                while (v.fallback == null || System.currentTimeMillis() - start < READY_WAIT_MS) {
                    try { Thread.sleep(reportedWait ? WINDOW_WATCH_MS : READY_POLL_MS); }
                    catch (InterruptedException e) { return; }
                    synchronized (LOCK) {
                        if (v.generation != gen || !v.requested || v.readyWaitSerial != serial) return;
                    }
                    if (!com.luka.carplay.core.ScreenModule.isConnected()) {
                        synchronized (LOCK) {
                            if (v.generation == gen && v.readyWaitSerial == serial) retireReadyWaitLocked(v);
                        }
                        return;
                    }
                    /* Retries a failed request write here, off the HMI thread; the HMI thread is
                     * only involved once the report is there (or the wait expires). */
                    requestRendererOutput(v, w, h);
                    if (outputReady(v, request)) {
                        synchronized (LOCK) {
                            if (v.generation != gen || v.readyWaitSerial != serial) return;
                            retireReadyWaitLocked(v);
                        }
                        postConverge(v, o);
                        return;
                    }
                    if (!reportedWait && System.currentTimeMillis() - start >= READY_WAIT_MS) {
                        reportedWait = true;
                        Log.w(v.tag, "renderer output not confirmed within " + READY_WAIT_MS
                            + " ms; stock picture kept, still waiting");
                    }
                }
                if (!com.luka.carplay.core.ScreenModule.isConnected()) return;
                synchronized (LOCK) {
                    if (v.generation != gen || !v.requested || v.readyWaitSerial != serial) return;
                    v.readyWaitExpired = true;
                }
                Log.w(v.tag, "renderer output not confirmed within " + READY_WAIT_MS
                    + " ms; composing once for diagnosis");
                postConverge(v, o);
            }
        }, "carplay-most-ready");
        waiter.setDaemon(true);
        waiter.start();
    }

    /** HMI thread (or the DisplayManager's switching thread, via onContextApplied): keep the
     *  window in step with the size the view needs.  A different size than the window was
     *  cropped for means it is recreated: the view is handed back to stock and re-composed
     *  once the renderer reports the new size - even if it already has, as only a real
     *  stock -> CarPlay switch re-points the MOST encoder - and the window is never cropped
     *  past its buffer meanwhile.  An unknown measured-only size keeps the placed window. */
    private static void refreshPlacement(View v, IDisplayManagerKombiControl d, ContextOwner o) {
        try {
            int[] size = size(v, o.cachedExtents(v.extentsSource));
            if (size == null) return;
            String request = outputLine(size[0], size[1]);
            requestRendererOutput(v, size[0], size[1]);
            String placed;
            synchronized (LOCK) { placed = v.placedOutputRequest; }
            cancelWaitForOtherOutput(v, request);
            if (!request.equals(placed)) {
                postHandBack(v, o, "renderer output changes to " + size[0] + "x" + size[1]);
                return;
            }
            if (outputReady(v, request)) cancelReadyWait(v);
            place(v, d, o, false);
        } catch (Throwable t) {
            Log.w(v.tag, "placement refresh failed: " + t);
        }
    }

    /** Hand the view back to stock and re-compose (a real context switch, which re-points the
     *  MOST encoder at a recreated window).  Posted: callers include the DisplayManager's own
     *  switchContext tail and the window watcher, and a nested switch must not run inside
     *  another. */
    private static void postHandBack(final View v, final ContextOwner o, final String reason) {
        post(o, new Runnable() {
            public void run() {
                if (!v.applied) return;
                restoreStock(v, o, "while " + reason, "-wait");
                converge(v);                          /* re-composes now if already reported */
            }
        });
    }

    /** While composed, watch the renderer's report for the placed size: a new token means the
     *  renderer replaced the window on its own (swap failure, loss recovery, a restart, or a
     *  resize that completed after the wait expired), which the MOST encoder only follows
     *  through a real stock -> CarPlay switch.  A withdrawn report (a recreation in progress)
     *  is waited out. */
    private static void startWindowWatch(final View v, final ContextOwner o, final int gen, final String token) {
        final int serial;
        synchronized (LOCK) {
            v.composedToken = token;
            serial = ++v.composeSerial;
        }
        Thread watcher = new Thread(new Runnable() {
            public void run() {
                while (true) {
                    try { Thread.sleep(WINDOW_WATCH_MS); } catch (InterruptedException e) { return; }
                    String placed;
                    String composed;
                    synchronized (LOCK) {
                        if (v.generation != gen || !v.requested || v.composeSerial != serial) return;
                        placed = v.placedOutputRequest;
                        composed = v.composedToken;
                    }
                    if (!v.applied || placed == null) continue;
                    String now = readyToken(v, placed);
                    if (now != null && !now.equals(composed)) {
                        postHandBack(v, o, "the renderer replaced its window");
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
     *  re-sent to the display service.  The window is the whole stream: identity cropping at
     *  its size. */
    private static void place(View v, IDisplayManagerKombiControl d, ContextOwner o, boolean force) {
        try {
            int[] extents = o.cachedExtents(v.extentsSource);
            int[] size = size(v, extents);
            if (size == null) return;
            boolean fromExtents = known(extents);
            int w = size[0];
            int h = size[1];
            requestRendererOutput(v, w, h);
            String placement = "window " + w + "x" + h + " from "
                + (fromExtents ? v.stockName : "KVS_Most default (" + describeExtents(v, extents) + ")");
            boolean changed;
            synchronized (LOCK) {
                changed = !placement.equals(v.lastPlacement);
                if (!changed && !force) return;
                v.placedOutputRequest = outputLine(w, h);
            }
            d.setCropping(v.displayable, TERMINAL_CLUSTER, 0, 0, w, h, 0, 0, w, h);
            d.setOpacity(v.displayable, TERMINAL_CLUSTER, 100);
            synchronized (LOCK) { v.lastPlacement = placement; }
            if (changed) Log.w(v.tag, v.content + " " + placement);
        } catch (Throwable t) {
            Log.w(v.tag, "placement failed: " + t);
        }
    }

    /** Stock's extents arrive asynchronously on the HMI thread.  While they are unknown, re-check
     *  the cache there and converge once they are known: that re-places a view composed at its
     *  fallback, or composes a measured-only one.  A view with a fallback keeps it after
     *  EXTENTS_RETRIES checks; a measured-only one goes on every WINDOW_WATCH_MS, asking afresh
     *  every EXTENTS_REFRESH_MS (the stock map only reports once it is ready).  One per request;
     *  this thread only sleeps and queues HMI-thread checks - it never touches the DM. */
    private static void startExtentsPoll(final View v, final ContextOwner o, final int gen) {
        synchronized (LOCK) {
            if (v.extentsPollGen == gen) return;
            v.extentsPollGen = gen;
        }
        if (v.fallback == null) {
            Log.w(v.tag, v.label + ": waiting for the " + v.stockName + " size (" + v.sourceName
                + " extents unknown)");
        }
        Thread poller = new Thread(new Runnable() {
            public void run() {
                try { poll(); }
                finally { synchronized (LOCK) { if (v.extentsPollGen == gen) v.extentsPollGen = -1; } }
            }

            void poll() {
                long sinceAsked = 0L;                 /* converge asked once already */
                for (int i = 0; v.fallback == null || i < EXTENTS_RETRIES; i++) {
                    long pause = i < EXTENTS_RETRIES ? EXTENTS_POLL_MS : WINDOW_WATCH_MS;
                    try { Thread.sleep(pause); } catch (InterruptedException e) { return; }
                    sinceAsked += pause;
                    final boolean ask = sinceAsked >= EXTENTS_REFRESH_MS;
                    if (ask) sinceAsked = 0L;
                    if (!polling()) return;
                    post(o, new Runnable() {
                        public void run() {
                            if (!polling()) return;
                            try {
                                if (known(o.cachedExtents(v.extentsSource))) {
                                    synchronized (LOCK) { v.extentsPollGen = -1; }
                                    converge(v);
                                } else if (ask) {
                                    o.requestExtents(v.extentsSource);
                                }
                            } catch (Throwable t) { }
                        }
                    });
                }
                int[] extents = null;
                try { extents = o.cachedExtents(v.extentsSource); } catch (Throwable t) { }
                if (!known(extents)) {
                    Log.w(v.tag, (!reported(extents) ? v.stockName + " extents not reported within "
                            + (EXTENTS_RETRIES * EXTENTS_POLL_MS) + " ms"
                            : describeExtents(v, extents)) + "; keeping fallback placement");
                }
            }

            boolean polling() {
                synchronized (LOCK) {
                    if (v.generation != gen || !v.requested || v.extentsPollGen != gen) return false;
                }
                return com.luka.carplay.core.ScreenModule.isConnected();
            }
        }, "carplay-most-extents");
        poller.setDaemon(true);
        poller.start();
    }

    public static String describe() {
        synchronized (LOCK) {
            return "mostArrows=" + (ARROWS.applied ? "carplay" : "stock") + (ARROWS.requested ? "/requested" : "")
                + " placement={" + ARROWS.lastPlacement + "}"
                + " mostMap=" + (MAP.applied ? "carplay" : "stock") + (MAP.requested ? "/requested" : "")
                + " mapPlacement={" + MAP.lastPlacement + "}";
        }
    }
}
