package com.luka.carplay.core;

import com.luka.carplay.framework.Log;
import com.luka.carplay.rgd.GatedCombiService;
import com.luka.carplay.rgd.RouteGuidance;
import de.audi.app.terminalmode.IContext;
import de.audi.atip.base.IFrameworkAccess;
import de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi;
import de.audi.tghu.navi.app.Navigation;
import de.audi.tghu.navi.app.cluster.ClusterService;
import de.audi.tghu.navi.app.cluster.CombiBAPListener;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import sun.misc.Unsafe;

/**
 * core seams.  Host-only; runs the shipping carplay_hook.jar classes
 * against the supplied stock JAR.  Every scenario names the on-car failure it guards against.
 */
public final class ClusterCoreTest {
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

    static Object alloc(Class<?> type) {
        try { return U.allocateInstance(type); }
        catch (InstantiationException e) { throw new AssertionError(e); }
    }

    static Field field(Class<?> type, String name) throws Exception {
        for (Class<?> c = type; c != null; c = c.getSuperclass()) {
            try { Field f = c.getDeclaredField(name); f.setAccessible(true); return f; }
            catch (NoSuchFieldException e) { }
        }
        throw new NoSuchFieldException(name);
    }

    static void setStatic(Class<?> type, String name, Object value) throws Exception {
        field(type, name).set(null, value);
    }

    static Object getStatic(Class<?> type, String name) throws Exception {
        return field(type, name).get(null);
    }

    static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }

    /** Framework coded for the given KOMBI_MAP_MODE; counts HMI-service lookups. */
    static final class Fw implements InvocationHandler {
        final int kombiMapMode;
        int hmiServiceLookups;
        Fw(int kombiMapMode) { this.kombiMapMode = kombiMapMode; }
        public Object invoke(Object proxy, Method m, Object[] a) {
            if (m.getName().equals("getSysConst")) {
                return Integer.valueOf(((Integer) a[0]).intValue() == 541 ? kombiMapMode : 0);
            }
            if (m.getName().equals("getKombiType")) return Integer.valueOf(0);
            if (m.getName().equals("getHMIService")) { hmiServiceLookups++; return null; }
            if (m.getReturnType() == Integer.TYPE) return Integer.valueOf(0);
            if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
            return null;
        }
        IFrameworkAccess proxy() {
            return (IFrameworkAccess) Proxy.newProxyInstance(ClusterCoreTest.class.getClassLoader(),
                new Class<?>[]{IFrameworkAccess.class}, this);
        }
    }

    static FrameworkRef ref(final IFrameworkAccess fw) {
        IContext ctx = (IContext) Proxy.newProxyInstance(ClusterCoreTest.class.getClassLoader(),
            new Class<?>[]{IContext.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) {
                    if (m.getName().equals("getFramework")) return fw;
                    if (m.getReturnType() == Integer.TYPE) return Integer.valueOf(0);
                    if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
                    return null;
                }
            });
        return new FrameworkRef(ctx);
    }

    static boolean switchWorkerAlive() {
        for (Thread t : Thread.getAllStackTraces().keySet()) {
            if ("carplay-cluster-switch".equals(t.getName()) && t.isAlive()) return true;
        }
        return false;
    }

    /** On a MOST (541=1) or RGI-only (541=0) cluster the session must not take terminal 1. */
    static void screenModuleLeavesNonFpkClusterToStock(int kombiMapMode) throws Exception {
        Fw handler = new Fw(kombiMapMode);
        ScreenModule screen = new ScreenModule();
        check(screen.start(ref(handler.proxy())), "non-FPK start must complete, not retry");
        check(handler.hmiServiceLookups == 0, "non-FPK start must not acquire the DisplayManager");
        check(!switchWorkerAlive(), "non-FPK start must not create the context switch worker");
        check(ScreenModule.isConnected(), "session semantics (isConnected) stay for other modules");
        check(!ScreenModule.ownsClusterContext(), "541=" + kombiMapMode + " must not own terminal-1 contexts");
        screen.stop();
        check(!ScreenModule.isConnected(), "stop clears the session");
        check(!ScreenModule.ownsClusterContext(), "stop leaves no ownership");
    }

    /** Virtual Cockpit keeps the upstream path: it goes on to acquire the DisplayManager. */
    static void screenModuleKeepsFpkPath() throws Exception {
        Fw handler = new Fw(2);
        ScreenModule screen = new ScreenModule();
        check(!screen.start(ref(handler.proxy())), "FPK start without a DisplayManager retries (upstream)");
        check(handler.hmiServiceLookups == 1, "FPK start proceeds to the DisplayManager");
        check(((Boolean) getStatic(ScreenModule.class, "clusterContextsOwned")).booleanValue(),
            "FPK marks the terminal-1 composition as ours");
        setStatic(ScreenModule.class, "clusterContextsOwned", Boolean.FALSE);
    }

    public static void main(String[] args) throws Exception {
        Log.setLevel(-1);
        screenModuleLeavesNonFpkClusterToStock(1);
        screenModuleLeavesNonFpkClusterToStock(0);
        screenModuleKeepsFpkPath();
        System.out.println("ClusterCoreTest: platform context ownership PASS ("+checks+" checks)");
    }
}
