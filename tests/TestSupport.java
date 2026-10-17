package testsupport;

import java.lang.reflect.Field;
import sun.misc.Unsafe;

/** Checks and reflection shared by the Java host suites (each suite runs in its own JVM). */
public final class TestSupport {
    /** Checks run so far, for a suite's PASS line. */
    public static int checks;
    private static final Unsafe U = unsafe();

    private TestSupport() { }

    public static void check(boolean ok, String why) {
        checks++;
        if (!ok) throw new AssertionError(why);
    }

    private static Unsafe unsafe() {
        try {
            Field f = Unsafe.class.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            return (Unsafe) f.get(null);
        } catch (Exception e) { throw new AssertionError(e); }
    }

    /** An instance of type without running a constructor. */
    public static Object alloc(Class<?> type) {
        try { return U.allocateInstance(type); }
        catch (InstantiationException e) { throw new AssertionError(e); }
    }

    /** The field declared by type or its nearest superclass, made accessible. */
    public static Field field(Class<?> type, String name) throws Exception {
        for (Class<?> c = type; c != null; c = c.getSuperclass()) {
            try { Field f = c.getDeclaredField(name); f.setAccessible(true); return f; }
            catch (NoSuchFieldException e) { }
        }
        throw new NoSuchFieldException(name);
    }

    public static Object get(Object target, String name) throws Exception {
        return field(target.getClass(), name).get(target);
    }

    public static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }

    public static Object getStatic(Class<?> type, String name) throws Exception {
        return field(type, name).get(null);
    }

    public static void setStatic(Class<?> type, String name, Object value) throws Exception {
        field(type, name).set(null, value);
    }
}
