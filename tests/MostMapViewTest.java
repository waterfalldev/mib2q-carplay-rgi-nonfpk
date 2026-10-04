import com.luka.carplay.bus.CarplayBus;
import com.luka.carplay.cluster.ClusterVideo;
import com.luka.carplay.cluster.MostPresentation;
import de.audi.tghu.fwhmi.DisplayManagerMIB2High;
import java.io.DataInputStream;
import java.io.File;
import java.lang.reflect.Method;
import java.net.Socket;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** The MOST MAP view through MostPresentation's shared engine, against the shipping display
 *  manager and stock core: MAP 72 -> 82 = {99} only for a measured, renderer-confirmed
 *  window, released back to stock, independent of the arrows view; requested by CarPlay
 *  guidance only while the phone's cluster stream is live; the wheel zooms it only then. */
public final class MostMapViewTest {
    private static int checks;

    /** The engine's MAP request itself, below the guidance-and-stream rule. */
    static void requestMap(boolean on) throws Exception {
        Method m = MostPresentation.class.getDeclaredMethod("setMapRequested", boolean.class);
        m.setAccessible(true);
        m.invoke(null, Boolean.valueOf(on));
    }
    private static void check(boolean yes, String why) {
        checks++;
        if (!yes) throw new AssertionError(why);
    }

    static final class Rig implements AutoCloseable {
        final MostArrowsTest.Hmi hmi = new MostArrowsTest.Hmi();
        final MostArrowsTest.Dsi dsi = new MostArrowsTest.Dsi();
        final Map<Integer, int[]> extents = new HashMap<Integer, int[]>();
        final DisplayManagerMIB2High dm = MostArrowsTest.displayManager(dsi, hmi, extents);
        final MostArrowsTest.Listener listener = new MostArrowsTest.Listener();
        final File request, ready;

        Rig() throws Exception {
            listener.hmi = hmi;
            MostArrowsTest.mostSession(true);
            Method defineContexts = DisplayManagerMIB2High.class.getDeclaredMethod("defineContexts");
            defineContexts.setAccessible(true);
            defineContexts.invoke(dm);
            MostArrowsTest.freshOutputRequest();
            request = new File((String) MostArrowsTest.viewField("MAP", "outputRequestPath"));
            ready = new File((String) MostArrowsTest.viewField("MAP", "outputReadyPath"));
        }

        void choose(int ctx) throws Exception { MostArrowsTest.stockSwitch(hmi, dm, ctx, 1, listener); }

        /** The display service's late answer, filled in as stock's DSI listener does. */
        void measured(final int w, final int h) throws Exception {
            hmi.run(new Runnable() { public void run() { extents.put(Integer.valueOf(33), new int[]{w, h}); } });
        }

        String requestText() throws Exception { return MostArrowsTest.read(request); }

        void report(String line) throws Exception { MostArrowsTest.write(ready, line); }

        boolean waitForRequest(String line, long ms) throws Exception {
            long end = System.currentTimeMillis() + ms;
            while (System.currentTimeMillis() < end) {
                if (line.equals(requestText())) return true;
                Thread.sleep(20);
            }
            return false;
        }

        public void close() throws Exception {
            MostPresentation.setMapLive(false);
            MostPresentation.setActive(false);
            hmi.idle();
            Thread.sleep(50);
            hmi.idle();
            check(dsi.offHmi.isEmpty(), "Every display-service call runs on the HMI thread: " + dsi.offHmi);
            MostArrowsTest.endSession();
            hmi.thread.interrupt();
        }
    }

    /* map02: stock reports -1x-1 until its map is ready.  An unknown source keeps being asked
     * for, never guessed: no request, no composition until a real size; then 82 only once the
     * renderer confirms exactly that window, however long that takes (the arrows compose once
     * for diagnosis). */
    private static void measuredConfirmedWindowOnly() throws Exception {
        try (Rig r = new Rig()) {
            r.extents.put(Integer.valueOf(33), new int[]{-1, -1});
            r.choose(72);
            r.dsi.drain();
            requestMap(true);
            Thread.sleep(8600);                          /* 3 s of cache checks, then every 5 s */
            r.hmi.idle();
            List<String> calls = r.dsi.drain();
            check(MostArrowsTest.count(calls, "getExtents 33") >= 2, "An unknown MAP source is re-queried: " + calls);
            check(r.requestText() == null && !MostArrowsTest.hasPrefix(calls, "switch ")
                && !MostArrowsTest.hasPrefix(calls, "crop "), "Unknown extents: no window request, no composition");
            r.measured(800, 298);
            check(r.waitForRequest("0800 0298\n", 2000L), "The measured size is requested: " + r.requestText());
            Thread.sleep(MostPresentation.READY_WAIT_MS + 1000L);
            r.hmi.idle();
            check(!MostArrowsTest.hasPrefix(r.dsi.drain(), "switch 82") && !MostPresentation.isMapActive(),
                "An unconfirmed window is never composed");
            r.listener.seen.clear();
            r.report("0800 0298 0000001234.0000000001\n");
            check(MostArrowsTest.waitFor(r.dsi, "switch 82 term 4", 2000L), "The confirmed window is composed as 82");
            r.hmi.idle();
            check(MostPresentation.isMapActive() && MostPresentation.substitute(1, 72) == 82,
                "Stock's MAP 72 now composes 82");
            Thread.sleep(50);
            r.hmi.idle();
            check(r.listener.seen.contains("72 term 1") && !r.listener.seen.contains("82 term 1")
                && !r.listener.seen.contains("3 term 1"), "Stock's listener sees its logical MAP 72: " + r.listener.seen);

            /* Release: stock 72 back before 99 is hidden, and the window request withdrawn. */
            r.dsi.drain();
            requestMap(false);
            r.hmi.idle();
            List<String> off = r.dsi.drain();
            check(off.contains("switch 72 term 4") && off.contains("opacity 99=0")
                && off.indexOf("switch 72 term 4") < off.lastIndexOf("opacity 99=0"),
                "Release restores stock MAP before hiding 99: " + off);
            check(!MostPresentation.isMapActive() && r.requestText() == null,
                "Release withdraws the renderer's map window request");
        }
    }

    /* The first composition writes the placement; a new window token re-composes. */
    private static void placementAndWindowToken() throws Exception {
        try (Rig r = new Rig()) {
            r.extents.put(Integer.valueOf(33), new int[]{800, 298});
            r.choose(72);
            r.report("0800 0298 0000001234.0000000001\n");
            r.dsi.drain();
            requestMap(true);
            check(MostArrowsTest.waitFor(r.dsi, "switch 82 term 4", 3000L), "Composed over window token 1");
            r.hmi.idle();
            List<String> calls = r.dsi.drain();
            check(calls.contains("crop 99 src(0,0 800x298) dst(0,0 800x298)") && calls.contains("opacity 99=100"),
                "99 is placed at the measured size and made visible: " + calls);
            check(MostPresentation.describe().indexOf("mapPlacement={window 800x298 from stock MAP}") >= 0,
                "The placement is described: " + MostPresentation.describe());
            r.report("0800 0298 0000001234.0000000002\n");
            check(MostArrowsTest.waitFor(r.dsi, "switch 82 term 4", 3000L), "A new window is composed again");
            r.hmi.idle();
            check(MostPresentation.isMapActive(), "Still composed after the new window");
        }
    }

    /* Arrows and MAP are two views of one engine: each substitutes only its own context.
     * Guidance requests the arrows, and the MAP view only while the cluster stream is live. */
    private static void viewsAreIndependent() throws Exception {
        try (Rig r = new Rig()) {
            r.extents.put(Integer.valueOf(33), new int[]{800, 298});
            r.report("0800 0298 0000001234.0000000001\n");
            MostPresentation.setActive(true);            /* arrows: the scratch renderer is instant */
            long end = System.currentTimeMillis() + 3000L;
            while (!MostPresentation.isActive() && System.currentTimeMillis() < end) Thread.sleep(20);
            Thread.sleep(300);
            r.hmi.idle();
            check(MostPresentation.isActive() && !MostPresentation.isMapActive(),
                "Guidance without a live cluster stream takes the arrows view only");
            MostPresentation.setMapLive(true);
            end = System.currentTimeMillis() + 3000L;
            while ((!MostPresentation.isActive() || !MostPresentation.isMapActive()) && System.currentTimeMillis() < end)
                Thread.sleep(20);
            check(MostPresentation.isActive() && MostPresentation.isMapActive(), "Both views composed");
            r.dsi.drain();
            r.choose(73);
            r.choose(72);
            r.choose(76);
            List<String> calls = r.dsi.drain();
            check(calls.indexOf("switch 81 term 4") >= 0 && calls.indexOf("switch 82 term 4") > calls.indexOf("switch 81 term 4")
                && calls.contains("switch 76 term 4"), "73 -> 81, 72 -> 82, alternate map 76 stays stock: " + calls);
            check(MostPresentation.logicalContext(81) == 73 && MostPresentation.logicalContext(82) == 72
                && MostPresentation.logicalContext(76) == 76, "Composed contexts map back to their stock views");
            r.choose(82);
            check(((Integer) MostArrowsTest.field(DisplayManagerMIB2High.class, "lastClusterRequest").get(r.dm)).intValue() == 72,
                "A re-issued 82 is remembered as stock MAP 72");
            MostPresentation.setMapLive(false);
            r.hmi.idle();
            check(MostPresentation.isActive() && !MostPresentation.isMapActive(),
                "The stream ending releases the MAP view and keeps the arrows");
            MostPresentation.setMapLive(true);
            end = System.currentTimeMillis() + 3000L;
            while (!MostPresentation.isMapActive() && System.currentTimeMillis() < end) Thread.sleep(20);
            check(MostPresentation.isMapActive(), "A live stream again during guidance: the MAP view again");
            MostPresentation.setActive(false);
            r.hmi.idle();
            check(!MostPresentation.isActive() && !MostPresentation.isMapActive(), "Guidance ending releases both views");
        }
    }

    /** The next CMD_ALT_ZOOM the hook reads (past others, such as the sync request): its one
     *  signed step byte, or -128 when malformed. */
    private static int zoomFrame(DataInputStream in) throws Exception {
        while (true) {
            if (in.readInt() != CarplayBus.MAGIC) return -128;
            in.readInt();
            int type = in.readUnsignedShort(), flags = in.readUnsignedByte();
            in.readByte();
            int len = in.readInt();
            byte[] body = new byte[len];
            in.readFully(body);
            if (type != CarplayBus.CMD_ALT_ZOOM) continue;
            return flags == CarplayBus.FLAG_BINARY && len == 1 ? body[0] : -128;
        }
    }

    /* The steering-wheel roller zooms the phone's map only while the MAP view shows it (its
     * stream live and composed): one bus frame per report, capped; otherwise stock's map. */
    private static void wheelZoom() throws Exception {
        ClusterVideo video = ClusterVideo.getInstance();
        Socket hook = null;
        try (Rig r = new Rig()) {
            r.extents.put(Integer.valueOf(33), new int[]{800, 298});
            r.report("0800 0298 0000001234.0000000001\n");
            check(!video.zoom(1), "No cluster stream: the wheel is stock's");
            video.start();
            long end = System.currentTimeMillis() + 3000L;
            while (hook == null && System.currentTimeMillis() < end) {
                try { hook = new Socket("127.0.0.1", CarplayBus.PORT); } catch (java.io.IOException e) { Thread.sleep(20); }
            }
            while (!CarplayBus.getInstance().isConnected() && System.currentTimeMillis() < end) Thread.sleep(20);
            check(hook != null && CarplayBus.getInstance().isConnected(), "The fake hook is connected");
            hook.setSoTimeout(3000);
            DataInputStream in = new DataInputStream(hook.getInputStream());
            byte[] live = "live:b:true\nstream:n:1\n".getBytes("UTF-8");
            video.onFrame(CarplayBus.EVT_CLUSTER_VIDEO, 0, live, live.length);
            check(!MostPresentation.isMapActive() && !video.zoom(1), "A live stream outside guidance: still stock's");
            MostPresentation.setActive(true);
            end = System.currentTimeMillis() + 3000L;
            while (!MostPresentation.isMapActive() && System.currentTimeMillis() < end) Thread.sleep(20);
            check(MostPresentation.isMapActive(), "Guidance with the live stream composes the MAP view");
            check(!video.zoom(0), "No step: nothing to send");
            check(video.zoom(-2) && zoomFrame(in) == -2, "Two steps in reach the hook");
            check(video.zoom(100) && zoomFrame(in) == 8, "A burst is capped at 8 steps out");
            byte[] ended = "live:b:false\nstream:n:1\n".getBytes("UTF-8");
            video.onFrame(CarplayBus.EVT_CLUSTER_VIDEO, 0, ended, ended.length);
            check(!video.zoom(1), "The stream ended: stock's again");
        } finally {
            video.stop();
            CarplayBus.getInstance().stop();
            if (hook != null) hook.close();
        }
    }

    public static void main(String[] args) throws Exception {
        measuredConfirmedWindowOnly();
        placementAndWindowToken();
        viewsAreIndependent();
        wheelZoom();
        System.out.println("MostMapViewTest: measured, confirmed MAP window only, stock restoration, independent views, the guidance-and-stream rule and wheel zoom PASS (" + checks + " checks)");
    }
}
