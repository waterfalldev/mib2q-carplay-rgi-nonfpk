import com.luka.carplay.cluster.ClusterLayerController;
import com.luka.carplay.cluster.ClusterPlatform;
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
 * cluster-state ownership.  Uses the supplied stock ClusterService,
 * ClusterViewMode, KOMOService and DSIResponseContainer objects (allocated without their HU
 * constructors) with the shipping BAPBridge and ClusterLayerController.
 */
public final class ClusterOwnershipTest {
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
        return (IFrameworkAccess) Proxy.newProxyInstance(ClusterOwnershipTest.class.getClassLoader(),
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

    /** KOMO gfx/dataRate are the cluster's own reports.  Forging them on a MOST cluster left the
     * cluster "without graphics" after route end/disconnect: MAP -> COMPASS, no zoom, no KDK. */
    static void komoStateIsNeverForged(int kombiMapMode) throws Exception {
        IFrameworkAccess fw = framework(kombiMapMode);
        installCarPlayFramework(fw);
        NavigationEnv env = (NavigationEnv) U.allocateInstance(NavigationEnv.class);
        set(env, "framework", fw);
        ClusterViewMode view = (ClusterViewMode) U.allocateInstance(ClusterViewMode.class);
        Service service = (Service) U.allocateInstance(Service.class);
        service.rates = new ArrayList<Integer>();
        set(service, "clusterViewMode", view);
        set(view, "env", env);
        set(view, "clusterService", service);
        /* The cluster really reported graphics available at full rate. */
        set(view, "gfxAvailable", Boolean.TRUE);
        set(view, "dataRate", Integer.valueOf(2));
        KOMOService komo = (KOMOService) U.allocateInstance(KOMOService.class);
        set(komo, "service", service);
        set(komo, "logChannel", silentLogChannel());
        BAPBridge bridge = new BAPBridge();
        set(bridge, "csRef", service);
        Method force = BAPBridge.class.getDeclaredMethod("forceGfxAvailable", Boolean.TYPE);
        force.setAccessible(true);
        for (int fallback = 0; fallback < 2; fallback++) {
            set(bridge, "komoService", fallback == 0 ? komo : null);
            boolean[] edges = {true, false, true, true, false, false};
            for (int i = 0; i < edges.length; i++) {
                force.invoke(bridge, Boolean.valueOf(edges[i]));
                check(((Integer) get(view, "dataRate")).intValue() == 2,
                    "541=" + kombiMapMode + ": KOMO dataRate forged by edge " + i);
                check(((Boolean) get(view, "gfxAvailable")).booleanValue(),
                    "541=" + kombiMapMode + ": KOMO gfxAvailable forged by edge " + i);
                check(service.rates.isEmpty(), "541=" + kombiMapMode + ": KOMO pacing written");
            }
        }
    }

    static final class DmRecorder implements InvocationHandler {
        final List<String> calls = new ArrayList<String>();
        public Object invoke(Object p, Method m, Object[] a) {
            calls.add(m.getName() + (a != null && a.length > 0 ? "(" + a[0] + ")" : ""));
            if (m.getReturnType() == Integer.TYPE) return Integer.valueOf(0);
            if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
            return null;
        }
        IDisplayManagerKombiControl proxy() {
            return (IDisplayManagerKombiControl) Proxy.newProxyInstance(ClusterOwnershipTest.class.getClassLoader(),
                new Class<?>[]{IDisplayManagerKombiControl.class}, this);
        }
    }

    /** 101/102 do not exist on a MOST/RGI-only cluster; stock never writes 98/101/102 there. */
    static void layerControllerSilentOnNonFpk() throws Exception {
        setStatic(ScreenModule.class, "platformSupported", Boolean.TRUE);
        setStatic(ScreenModule.class, "connected", Boolean.TRUE);
        setStatic(ScreenModule.class, "navActive", Boolean.TRUE);
        setStatic(ScreenModule.class, "clusterContextsOwned", Boolean.FALSE);
        DmRecorder dm = new DmRecorder();
        for (int mode = 0; mode < 2; mode++) {
            ClusterPlatform.bind(framework(mode));
            ClusterLayerController.bind(dm.proxy(), 1);
            ClusterLayerController.apply(dm.proxy(), 1, null, true, 100, false);
            ClusterLayerController.onVcPresentation(true);
            ClusterLayerController.onVcVisibility(true);
            ClusterLayerController.reapply();
            ClusterLayerController.onVcVisibility(false);
            check(dm.calls.isEmpty(), "541=" + mode + ": layer controller wrote the DM: " + dm.calls);
        }
        /* Sanity: the same sequence on the Virtual Cockpit does drive the planes. */
        ClusterPlatform.bind(framework(2));
        setStatic(ScreenModule.class, "clusterContextsOwned", Boolean.TRUE);
        ClusterLayerController.onVcVisibility(true);
        check(!dm.calls.isEmpty(), "FPK still drives 98/101/102");
        check(ScreenModule.ownsClusterContext(), "FPK connected session owns the context");
        setStatic(ScreenModule.class, "clusterContextsOwned", Boolean.FALSE);
        setStatic(ScreenModule.class, "connected", Boolean.FALSE);
        setStatic(ScreenModule.class, "navActive", Boolean.FALSE);
    }

    public static void main(String[] args) throws Exception {
        Log.setLevel(-1);
        for (int mode = 0; mode <= 2; mode++) komoStateIsNeverForged(mode);
        layerControllerSilentOnNonFpk();
        System.out.println("ClusterOwnershipTest: KOMO never forged, "
            + "no plane writes off FPK PASS (" + checks + " checks)");
    }
}
