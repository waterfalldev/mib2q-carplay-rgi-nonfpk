import com.luka.carplay.framework.Log;
import com.luka.carplay.rgd.BAPBridge;
import com.luka.carplay.rgd.RendererServer;
import java.lang.reflect.Field;
import static testsupport.TestSupport.*;

/** Route stop preserves pixels; session shutdown still releases the renderer. */
public final class ClusterKdkRendererLifecycleTest {
    public static final class Renderer extends RendererServer {
        int clears, disposals;
        public boolean sendClear() { clears++; return true; }
        public void dispose() { disposals++; }
    }
    static Field field(String name) throws Exception {
        Field f=BAPBridge.class.getDeclaredField(name);f.setAccessible(true);return f;
    }
    public static void main(String[] args) throws Exception {
        Log.setLevel(-1);
        BAPBridge bridge=new BAPBridge(); Renderer renderer=new Renderer();
        field("initialized").setBoolean(bridge,true);
        field("rendererClient").set(bridge,renderer);
        field("customRendererStarted").setBoolean(bridge,true);
        // Absent BAP endpoint also exercises cleanup after a failed BAP teardown.
        bridge.onRouteEnd();
        check(renderer.clears==0 && renderer.disposals==0,"route end destroys fading surface");
        check(field("rendererClient").get(bridge)==renderer,"route end loses reusable connection");
        check(!field("customRendererStarted").getBoolean(bridge),"route end keeps feeding renderer");
        bridge.onShutdown();
        check(renderer.clears==1 && renderer.disposals==1,"disconnect fails to release preserved renderer");
        check(field("rendererClient").get(bridge)==null,"disconnect retains renderer reference");
        bridge.onShutdown();
        check(renderer.disposals==1,"repeated disconnect disposes twice");
        System.out.println("ClusterKdkRendererLifecycleTest: route end preserves surface, disconnect releases, BAP failure cleanup PASS");
    }
}
