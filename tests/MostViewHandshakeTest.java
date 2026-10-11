import com.luka.carplay.cluster.ClusterPlatform;
import com.luka.carplay.core.CarPlayApp;
import com.luka.carplay.core.FrameworkRef;
import com.luka.carplay.framework.Log;
import com.luka.carplay.rgd.BAPBridge;
import com.luka.carplay.rgd.GatedCombiService;
import de.audi.atip.base.IFrameworkAccess;
import de.audi.atip.hmi.modelaccess.ChoiceModelApp;
import de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi;
import de.audi.atip.log.LogChannel;
import de.audi.tghu.navi.app.NavigationEnv;
import de.audi.tghu.navi.app.cluster.BAPDistanceFormatter;
import de.audi.tghu.navi.app.cluster.CarPlayKOMOService;
import de.audi.tghu.navi.app.cluster.ClusterKDKHandler;
import de.audi.tghu.navi.app.cluster.ClusterService;
import de.audi.tghu.navi.app.cluster.ClusterViewMode;
import de.audi.tghu.navi.app.cluster.KOMOCaller;
import de.audi.tghu.navi.app.cluster.ScreenCombiBAPListener;
import de.audi.tghu.navi.app.command.DSIResponseContainer;
import de.audi.tghu.navi.app.map.MapInterface;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import sun.misc.Unsafe;

/**
 * the MOST cluster's view handshake when CarPlay route guidance starts and ends.
 *
 * Stock's real ClusterViewMode (view decision), upstream's ScreenCombiBAPListener over stock
 * CombiBAPListener (view -> BAP ActiveRGType), the real GatedCombiService, the patched ClusterService
 * and CarPlayKOMOService, driven by the real BAPBridge claim/release.  Only the edges are recorded:
 * the BAP transport, the map interface (kombi context switches) and the KOMO transport.
 *
 * The 25 September 2026 trial: cluster favoured the arrows view, reported its KOMO view not visible,
 * stock stayed in COMPASS on kombi context 8/ctx 72 and never answered the cluster's view request.
 */
public final class MostViewHandshakeTest {
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

    static Field field(Class<?> type, String name) throws Exception {
        for (Class<?> c = type; c != null; c = c.getSuperclass()) {
            try { Field f = c.getDeclaredField(name); f.setAccessible(true); return f; }
            catch (NoSuchFieldException e) { }
        }
        throw new NoSuchFieldException(name);
    }

    static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }

    static LogChannel silentLog() throws Exception {
        Constructor<?> ctor = Class.forName("com.luka.carplay.rgd.BAPBridge$SilentLogChannel").getDeclaredConstructor();
        ctor.setAccessible(true);
        return (LogChannel) ctor.newInstance();
    }

    static IFrameworkAccess framework(final int kombiMapMode) {
        return (IFrameworkAccess) Proxy.newProxyInstance(MostViewHandshakeTest.class.getClassLoader(),
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

    /* ---------------- edges ---------------- */

    /** What reaches the cluster over BAP navigation (FctIDs 17, 39, info states). */
    static final class Bap implements InvocationHandler {
        final List<String> sent = new ArrayList<String>();
        public synchronized Object invoke(Object p, Method m, Object[] a) {
            String n = m.getName();
            if (n.equals("updateRGStatus")) sent.add("RGStatus " + a[0]);
            else if (n.equals("updateActiveRGType")) sent.add("ActiveRGType " + a[0]);
            else if (n.equals("updateInfoStates")) sent.add("InfoStates " + a[0]);
            return null;
        }
        synchronized List<String> drain() { List<String> out = new ArrayList<String>(sent); sent.clear(); return out; }
    }

    /** Stock's kombi display-context requests (map app -> HMI -> terminal-1 context). */
    public static final class Map extends MapInterface {
        List<String> calls;
        Map() { super(null, null, null, null); }
        public void switchDisplayContextKombi(int ctx) { calls.add("kombi " + ctx); }
        public void showKombiMap(boolean show) { calls.add("showMap " + show); }
    }

    /** The KOMO transport: view selection, fades and text. */
    public static final class Komo extends KOMOCaller {
        List<String> calls;
        Komo() { super(null, null); }
        public synchronized void setRgSelect(int n) { calls.add("rgSelect " + n); }
        public synchronized void fadeIn(int a, int b, int c) { calls.add("fadeIn"); }
        public synchronized void fadeOut(int a) { calls.add("fadeOut"); }
        public synchronized void setDistanceToNextManeuver(long v, int u, boolean ok) { }
        public synchronized void setTurnToStreet(String a, String b) { }
        public synchronized void setCurrentStreet(String s) { }
        public synchronized void setETA(int f, short d, short h, short m, boolean ok, boolean fl) { }
        public synchronized void setRTT(short h, short m, boolean ok) { }
        public synchronized void setDistanceToDestination(long v, int u, boolean ok) { }
    }

    /** NavigationEnv whose model 67 is ClusterViewMode's view-mode model. */
    public static final class Env extends NavigationEnv {
        ChoiceModelApp viewModel;
        Env() { super((IFrameworkAccess) null, null, null); }
        public ChoiceModelApp getChoiceModel(int id) { return viewModel; }
    }

    static ChoiceModelApp intModel() {
        final int[] value = {0};
        return (ChoiceModelApp) Proxy.newProxyInstance(MostViewHandshakeTest.class.getClassLoader(),
            new Class<?>[]{ChoiceModelApp.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) {
                    if (m.getName().equals("setValue")) { value[0] = ((Integer) a[0]).intValue(); return null; }
                    if (m.getName().equals("getValue")) return Integer.valueOf(value[0]);
                    if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
                    if (m.getReturnType() == Integer.TYPE) return Integer.valueOf(0);
                    return null;
                }
            });
    }

    /* ---------------- rig ---------------- */

    static final class Rig {
        final IFrameworkAccess fw;
        final DSIResponseContainer container;
        final ChoiceModelApp viewModel = intModel();
        final ClusterViewMode view;
        final ClusterService service;
        final ScreenCombiBAPListener listener;
        final GatedCombiService gate;
        final CarPlayKOMOService komo;
        final Bap bap = new Bap();
        final Map map;
        final Komo komoCaller;
        final BAPBridge bridge;
        final Method force;
        final Method claim;
        final Method activeRGType;

        Rig(int kombiMapMode, int favoredView) throws Exception {
            fw = framework(kombiMapMode);
            ClusterPlatform.bind(fw);
            ClusterPlatform.resolve(fw);
            FrameworkRef ref = new FrameworkRef(null);
            set(ref, "fw", fw);
            field(CarPlayApp.class, "fwRef").set(null, ref);
            LogChannel log = silentLog();

            container = (DSIResponseContainer) U.allocateInstance(DSIResponseContainer.class);
            Env env = (Env) U.allocateInstance(Env.class);
            env.viewModel = viewModel;
            set(env, "framework", fw);
            set(env, "container", container);

            view = (ClusterViewMode) U.allocateInstance(ClusterViewMode.class);
            service = (ClusterService) U.allocateInstance(ClusterService.class);
            listener = (ScreenCombiBAPListener) U.allocateInstance(ScreenCombiBAPListener.class);
            komo = (CarPlayKOMOService) U.allocateInstance(CarPlayKOMOService.class);
            map = (Map) U.allocateInstance(Map.class);
            map.calls = new ArrayList<String>();
            komoCaller = (Komo) U.allocateInstance(Komo.class);
            komoCaller.calls = new ArrayList<String>();
            gate = new GatedCombiService((CombiBAPServiceNavi) Proxy.newProxyInstance(
                MostViewHandshakeTest.class.getClassLoader(), new Class<?>[]{CombiBAPServiceNavi.class}, bap));
            gate.setRouteGuidanceBlocked(true);                 /* REPLACE: shut for the whole session */

            /* Stock view state as the trial logged it: compass, cluster wants 'favoredView',
             * graphics and map ready, KOMO view enabled but reported not visible. */
            set(view, "env", env);
            set(view, "logChannel", log);
            set(view, "clusterService", service);
            set(view, "viewMode", viewModel);
            set(view, "favoredViewMode", Integer.valueOf(favoredView));
            set(view, "favoredViewModeReceived", Boolean.TRUE);
            set(view, "gfxAvailable", Boolean.TRUE);
            set(view, "mapReady", Boolean.TRUE);
            set(view, "komoViewEnabled", Boolean.TRUE);
            set(view, "komoViewVisible", Boolean.FALSE);
            set(view, "dataRate", Integer.valueOf(1));

            set(listener, "env", env);
            set(listener, "logChannel", log);
            set(listener, "combiservice", gate);
            set(listener, "clusterservice", service);
            set(listener, "rgType", Integer.valueOf(2));        /* stock last answered COMPASS */
            set(listener, "validRGTypeReceived", Boolean.TRUE);
            set(listener, "bapDistanceFormatter", new BAPDistanceFormatter(log));

            set(komo, "fieldLock", new Object());
            set(komo, "visibilityLock", new Object());
            set(komo, "logChannel", log);
            set(komo, "komoCaller", komoCaller);
            set(komo, "service", service);
            set(komo, "clusterKDKHandler", Proxy.newProxyInstance(MostViewHandshakeTest.class.getClassLoader(),
                new Class<?>[]{ClusterKDKHandler.class}, new InvocationHandler() {
                    public Object invoke(Object p, Method m, Object[] a) { return null; }
                }));

            set(service, "env", env);
            set(service, "logChannel", log);
            set(service, "clusterViewMode", view);
            set(service, "komoService", komo);
            set(service, "combiBAPListener", listener);
            set(service, "mapInterface", map);
            set(service, "bapDistanceFormatter", new BAPDistanceFormatter(log));
            set(service, "galLock", new Object());
            set(service, "lastKombiContext", Integer.valueOf(-1));

            bridge = new BAPBridge();
            set(bridge, "csRef", service);
            force = BAPBridge.class.getDeclaredMethod("forceClusterRouteInfoState", Boolean.TYPE);
            force.setAccessible(true);
            claim = BAPBridge.class.getDeclaredMethod("claimKomoGuidance", Boolean.TYPE);
            claim.setAccessible(true);
            activeRGType = BAPBridge.class.getDeclaredMethod("activeRGType");
            activeRGType.setAccessible(true);
        }

        /** BAPBridge.onStart's cluster-state part, in its order. */
        void carPlayStarts() throws Exception {
            force.invoke(bridge, Boolean.TRUE);
            claim.invoke(bridge, Boolean.TRUE);
        }

        /** BAPBridge.onStop/onShutdown's cluster-state part, in its order. */
        void carPlayStops() throws Exception {
            force.invoke(bridge, Boolean.FALSE);
            claim.invoke(bridge, Boolean.FALSE);
        }

        int viewMode() { return viewModel.getValue(); }
        boolean stockVisible() throws Exception { return ((Boolean) get(view, "komoViewVisible")).booleanValue(); }
        int carPlayRgType() throws Exception { return ((Integer) activeRGType.invoke(bridge)).intValue(); }
        boolean smartphoneNavigation() throws Exception { return ((Boolean) get(listener, "naviIsRunningOnSmartphone")).booleanValue(); }
        List<String> kombi() { List<String> out = new ArrayList<String>(map.calls); map.calls.clear(); return out; }
        List<String> komoCalls() { List<String> out = new ArrayList<String>(komoCaller.calls); komoCaller.calls.clear(); return out; }
    }

    static String last(List<String> sent, String prefix) {
        String found = null;
        for (int i = 0; i < sent.size(); i++) if (sent.get(i).startsWith(prefix)) found = sent.get(i);
        return found;
    }

    /* ---------------- scenarios ---------------- */

    /** The trial's case: arrows view wanted, KOMO view reported not visible. */
    static void arrowsViewSelectedAndAnswered() throws Exception {
        Rig r = new Rig(1, ClusterViewMode.VIEWMODE_KDK);
        r.force.invoke(r.bridge, Boolean.TRUE);                 /* rgActive + RGI valid, as before */
        check(r.viewMode() == ClusterViewMode.VIEWMODE_COMPASS && !r.kombi().contains("kombi 9"),
            "without the view hold stock stays in COMPASS (the 25 September symptom)");
        r.bap.drain();
        r.komoCalls();

        r.claim.invoke(r.bridge, Boolean.TRUE);
        List<String> sent = r.bap.drain();
        check(r.viewMode() == ClusterViewMode.VIEWMODE_KDK, "stock selects the arrows view once CarPlay guides: " + r.viewMode());
        check(sent.contains("ActiveRGType 1"), "stock's answer (arrows = 1) reaches the cluster through the gate: " + sent);
        check(last(sent, "RGStatus") == null, "stock's RGStatus stays gated (CarPlay owns FctID 17): " + sent);
        check(r.kombi().contains("kombi 9"), "stock switches the kombi context to 9 (display ctx 73 -> CarPlay 81)");
        List<String> k = r.komoCalls();
        check(k.contains("rgSelect 1") && k.contains("fadeIn"), "stock selects the KDK view over KOMO and fades it in: " + k);
        check(r.carPlayRgType() == 1, "CarPlay's own FctID 39 carries the same arrows type: " + r.carPlayRgType());

        r.komo.updateVisibility(false, 1);                      /* cluster's own reports meanwhile */
        r.komo.updateVisibility(true, 1);
        r.komo.updateVisibility(false, 1);
        check(r.viewMode() == ClusterViewMode.VIEWMODE_KDK && r.stockVisible(),
            "cluster visibility reports do not drop the arrows view while CarPlay guides");
        check(last(r.bap.drain(), "ActiveRGType") == null, "no view flapping on the cluster meanwhile");

        r.carPlayStops();
        sent = r.bap.drain();
        check(r.viewMode() == ClusterViewMode.VIEWMODE_COMPASS, "route end: stock back to COMPASS (no route): " + r.viewMode());
        check("ActiveRGType 2".equals(last(sent, "ActiveRGType")), "route end: stock answers compass: " + sent);
        check(!r.stockVisible(), "route end: stock sees the cluster's latest own report (false), not CarPlay's");
        check(r.carPlayRgType() == 2, "after release CarPlay's type follows stock's compass");
    }

    /** The cluster's latest report is what stock keeps, whichever it is. */
    static void visibilityReturnedIsTheClustersLatest() throws Exception {
        Rig r = new Rig(1, ClusterViewMode.VIEWMODE_KDK);
        r.carPlayStarts();
        r.komo.updateVisibility(true, 1);
        r.carPlayStops();
        check(r.stockVisible(), "cluster last reported visible: stock keeps visible after release");
        r.carPlayStarts();
        check(r.viewMode() == ClusterViewMode.VIEWMODE_KDK, "second route: arrows view again");
        r.carPlayStops();
        r.komo.updateVisibility(false, 1);
        check(!r.stockVisible(), "outside CarPlay the cluster's reports pass straight through");
    }

    /** Map view wanted: answered too (map = 3, kombi context 8). */
    static void mapViewAnswered() throws Exception {
        Rig r = new Rig(1, ClusterViewMode.VIEWMODE_MAP);
        r.carPlayStarts();
        List<String> sent = r.bap.drain();
        check(r.viewMode() == ClusterViewMode.VIEWMODE_MAP, "map view selected: " + r.viewMode());
        check(sent.contains("ActiveRGType 3") && r.kombi().contains("kombi 8"), "map answered (3) on kombi context 8: " + sent);
        check(r.carPlayRgType() == 3, "CarPlay's FctID 39 carries map");
        r.carPlayStops();
    }

    /** "Navigation on the smartphone" puts the cluster on compass in stock; held while CarPlay guides. */
    static void smartphoneNavigationHeldWhileCarPlayGuides() throws Exception {
        Rig r = new Rig(1, ClusterViewMode.VIEWMODE_KDK);
        r.service.updateGALState(true);                         /* Apple Maps reported before RGI start */
        List<String> sent = r.bap.drain();
        check(r.smartphoneNavigation() && sent.contains("InfoStates 6") && sent.contains("ActiveRGType 2"),
            "stock: phone navigation -> info state 6, compass: " + sent);

        r.carPlayStarts();
        sent = r.bap.drain();
        check(!r.smartphoneNavigation(), "held: stock no longer treats navigation as phone-only");
        check(!sent.contains("InfoStates 6") && sent.contains("ActiveRGType 1"),
            "cluster gets the arrows view, not the phone-navigation compass: " + sent);
        r.service.updateGALState(true);                         /* re-reported during the route */
        r.service.updateGALState(false);
        r.service.updateGALState(true);
        check(!r.smartphoneNavigation() && !r.bap.drain().contains("InfoStates 6"), "reports during the route are recorded, not applied");

        r.carPlayStops();
        sent = r.bap.drain();
        check(r.smartphoneNavigation() && "InfoStates 6".equals(last(sent, "InfoStates")),
            "release hands stock its latest phone-navigation state: " + sent);
        check("ActiveRGType 2".equals(last(sent, "ActiveRGType")), "...and stock's compass answer: " + sent);

        Rig q = new Rig(1, ClusterViewMode.VIEWMODE_KDK);         /* phone navigation never reported */
        q.carPlayStarts();
        q.carPlayStops();
        check(!q.smartphoneNavigation() && q.bap.drain().indexOf("InfoStates 6") < 0, "nothing invented when stock never saw phone navigation");
    }

    /** Virtual Cockpit and RGI-only codings are untouched: no claim, gate as before. */
    static void otherCodingsUntouched() throws Exception {
        int[] modes = {2, 0};
        for (int i = 0; i < modes.length; i++) {
            Rig r = new Rig(modes[i], ClusterViewMode.VIEWMODE_KDK);
            r.carPlayStarts();
            check(!r.komo.isCarPlayOwned() && !r.stockVisible(), "541=" + modes[i] + ": no KOMO claim, no visibility hold");
            r.gate.updateActiveRGType(1);
            check(last(r.bap.drain(), "ActiveRGType") == null, "541=" + modes[i] + ": stock FctID 39 stays gated");
            check(r.carPlayRgType() == 0, "541=" + modes[i] + ": CarPlay keeps upstream's type");
            r.carPlayStops();
        }
    }

    public static void main(String[] args) throws Exception {
        Log.setLevel(-1);
        arrowsViewSelectedAndAnswered();
        visibilityReturnedIsTheClustersLatest();
        mapViewAnswered();
        smartphoneNavigationHeldWhileCarPlayGuides();
        otherCodingsUntouched();
        System.out.println("MostViewHandshakeTest: stock selects and answers the arrows/map view while CarPlay guides a MOST "
            + "cluster, phone-navigation compass held, cluster's own reports restored PASS (" + checks + " checks)");
    }
}
