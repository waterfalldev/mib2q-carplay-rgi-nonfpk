import com.luka.carplay.core.CarPlayApp;
import com.luka.carplay.core.FrameworkRef;
import com.luka.carplay.core.ScreenModule;
import com.luka.carplay.framework.Log;
import com.luka.carplay.rgd.BAPBridge;
import de.audi.atip.base.IFrameworkAccess;
import de.audi.tghu.fwhmi.IDisplayManagerKombiControl;
import de.audi.tghu.navi.app.NavigationEnv;
import de.audi.tghu.navi.app.cluster.ClusterService;
import de.audi.tghu.navi.app.cluster.ClusterViewMode;
import de.audi.tghu.navi.app.cluster.KOMOService;
import de.audi.tghu.navi.app.command.DSIResponseContainer;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import sun.misc.Unsafe;

/**
 * Guidance restoration. Uses the supplied stock ClusterService,
 * ClusterViewMode, KOMOService and DSIResponseContainer objects (allocated without their HU
 * constructors) with the shipping BAPBridge and ClusterLayerController.
 */
public final class NativeGuidanceStateTest {
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

    static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }

    static Object get(Object target, String name) throws Exception {
        return field(target.getClass(), name).get(target);
    }

    static void setStatic(Class<?> type, String name, Object value) throws Exception {
        field(type, name).set(null, value);
    }

    static IFrameworkAccess framework(final int kombiMapMode) {
        return (IFrameworkAccess) Proxy.newProxyInstance(NativeGuidanceStateTest.class.getClassLoader(),
            new Class<?>[]{IFrameworkAccess.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) {
                    if (m.getName().equals("getSysConst")) {
                        int id = ((Integer) a[0]).intValue();
                        if (id == 541) return Integer.valueOf(kombiMapMode);
                        if (id == 4383 || id == 4388) return Integer.valueOf(kombiMapMode > 0 ? 1 : 0);
                        return Integer.valueOf(0);
                    }
                    if (m.getName().equals("getScreenRes")) return Integer.valueOf(0);   /* not ClusterMMI */
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

    /** ClusterService whose KOMO pacing writes are recorded instead of reaching a model. */
    public static final class Service extends ClusterService {
        List<Integer> rates;
        Service() { super(null, null, null, null, null, null); }
        public void setKOMODataRate(int rate) { rates.add(Integer.valueOf(rate)); }
    }

    /** Stock view-mode machine with the HMI-model side cut off; records what ClusterService feeds it. */
    public static final class View extends ClusterViewMode {
        Boolean lastRgiValid;
        View() { super(null, null); }
        public void setRGIValid(boolean valid) { lastRgiValid = Boolean.valueOf(valid); }
        public void refreshRGState() { }
    }

    static Object silentLogChannel() throws Exception {
        Constructor<?> ctor = Class.forName("com.luka.carplay.rgd.BAPBridge$SilentLogChannel").getDeclaredConstructor();
        ctor.setAccessible(true);
        return ctor.newInstance();
    }

    static final class Rig {
        final DSIResponseContainer container;
        final View view;
        final ClusterService service;
        final BAPBridge bridge;
        final Method force;
        Rig(int kombiMapMode) throws Exception {
            IFrameworkAccess fw = framework(kombiMapMode);
            installCarPlayFramework(fw);
            container = (DSIResponseContainer) U.allocateInstance(DSIResponseContainer.class);
            NavigationEnv env = (NavigationEnv) U.allocateInstance(NavigationEnv.class);
            set(env, "framework", fw);
            set(env, "container", container);
            view = (View) U.allocateInstance(View.class);
            service = (ClusterService) U.allocateInstance(ClusterService.class);
            set(service, "env", env);
            set(service, "clusterViewMode", view);
            set(service, "logChannel", silentLogChannel());
            bridge = new BAPBridge();
            set(bridge, "csRef", service);
            force = BAPBridge.class.getDeclaredMethod("forceClusterRouteInfoState", Boolean.TYPE);
            force.setAccessible(true);
        }
        /** Exactly what AbstractDSINavigationHandler.updateRgActive does.  The stock tail of
         * ClusterService.updateRgActive (KDK handler, BAP listener, HMI models) needs HU objects
         * this host rig does not build; the patched seam and refreshRGIValid() run before it. */
        void dsiReportsRgActive(boolean active) throws Exception {
            container.setRgActive(active);
            try { service.updateRgActive(active); }
            catch (NullPointerException hostOnlyStockTail) { }
        }
        /** Stock DefaultDSINavigationMainHandler.updateRgiString result, minus the HMI model. */
        void stockRgiData(boolean valid) throws Exception { set(service, "rgiDataValid", Boolean.valueOf(valid)); }
        void carPlay(boolean active) throws Exception { force.invoke(bridge, Boolean.valueOf(active)); }
        boolean rgiValid() { return view.lastRgiValid != null && view.lastRgiValid.booleanValue(); }
        boolean stockRgiDataValid() throws Exception { return ((Boolean) get(service, "rgiDataValid")).booleanValue(); }
    }

    /** Release used to call updateRGIString(null): a live native route's RGI turned invalid and
     * a MOST/RGI-only ClusterViewMode fell to COMPASS until new RGI data happened to arrive. */
    static void rgiAndRgActiveReturnToStockTruth(int kombiMapMode) throws Exception {
        /* 1. native Audi route guiding throughout the CarPlay session */
        Rig r = new Rig(kombiMapMode);
        r.dsiReportsRgActive(true);
        r.stockRgiData(true);
        r.carPlay(true);
        check(r.rgiValid() && r.container.isRgActive(), "CarPlay RGI claims validity");
        r.carPlay(false);
        check(r.container.isRgActive(), "live native route keeps rgActive after release");
        check(r.stockRgiDataValid(), "stock RGI data is never overwritten");
        check(r.rgiValid(), "live native route stays RGI-valid after release");

        /* 2. no native route */
        r = new Rig(kombiMapMode);
        r.dsiReportsRgActive(false);
        r.carPlay(true);
        check(r.rgiValid() && r.container.isRgActive(), "CarPlay RGI claims validity without a native route");
        r.carPlay(false);
        check(!r.container.isRgActive() && !r.rgiValid(), "idle navigator gets its honest false back");

        /* 3. an MMI route is started while CarPlay holds the overlay */
        r = new Rig(kombiMapMode);
        r.dsiReportsRgActive(false);
        r.carPlay(true);
        r.dsiReportsRgActive(true);
        r.stockRgiData(true);
        r.carPlay(false);
        check(r.container.isRgActive(), "DSI's newer rgActive=true survives the release (no stale restore)");
        check(r.rgiValid(), "route set mid-session remains RGI-valid");

        /* 4. the navigator finishes starting while CarPlay guides (map23): its first report,
         *    no route, must not end CarPlay's RGI; the release then hands that report back */
        r = new Rig(kombiMapMode);
        r.carPlay(true);
        r.dsiReportsRgActive(false);
        check(r.container.isRgActive() && r.rgiValid(), "a late navigator 'no route' leaves CarPlay's RGI valid");
        r.carPlay(false);
        check(!r.container.isRgActive() && !r.rgiValid(), "the release hands back the navigator's no route");

        /* 5. release without a prior claim is a no-op on stock state */
        r = new Rig(kombiMapMode);
        r.dsiReportsRgActive(true);
        r.stockRgiData(true);
        r.carPlay(false);
        check(r.container.isRgActive() && r.rgiValid() && r.stockRgiDataValid(), "stray release leaves stock untouched");
    }

    public static void main(String[] args) throws Exception {
        Log.setLevel(-1);
        for (int mode = 0; mode <= 2; mode++) rgiAndRgActiveReturnToStockTruth(mode);
        System.out.println("NativeGuidanceStateTest: restores latest stock guidance PASS ("+checks+" checks)");
    }
}
