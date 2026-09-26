package com.luka.carplay.core;

import com.luka.carplay.rgd.RouteGuidance;
import java.lang.reflect.Field;
import sun.misc.Unsafe;

/** Actual module teardown, including a failing route-guidance service. */
public final class RgdTeardownTest {
    static final class FailingGuidance extends RouteGuidance {
        int released;
        public void stop() { throw new IllegalStateException("injected stop failure"); }
        public void disengageTakeover() { released++; }
    }
    public static void main(String[] args) throws Exception {
        Field unsafeField=Unsafe.class.getDeclaredField("theUnsafe"); unsafeField.setAccessible(true);
        Unsafe unsafe=(Unsafe)unsafeField.get(null);
        FailingGuidance guidance=(FailingGuidance)unsafe.allocateInstance(FailingGuidance.class);
        RgdModule module=new RgdModule();
        Field rg=RgdModule.class.getDeclaredField("rg"); rg.setAccessible(true); rg.set(module,guidance);
        module.stop();
        if(guidance.released!=1 || rg.get(module)!=null) throw new AssertionError("failed stop leaked takeover");
        module.stop();
        if(guidance.released!=1) throw new AssertionError("duplicate release");
        System.out.println("RgdTeardownTest: exception-safe release and idempotent stop PASS");
    }
}
