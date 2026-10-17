/*
 * Whole-record files shared by the MOST handshakes. QNX's shared-memory /tmp
 * cannot rename, so a record is rewritten in place and a reader accepts only a
 * complete, bounded record (callers validate the content).
 */
package com.luka.carplay.cluster;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;

final class RecordFiles {
    private RecordFiles() { }

    /** Truncate the file and write one ASCII record; any failure is thrown. */
    static void write(String path, String record) throws IOException {
        FileOutputStream out = new FileOutputStream(path);
        try { out.write(record.getBytes("US-ASCII")); }
        finally { out.close(); }
    }

    /** Remove the file; false when it is still there. */
    static boolean remove(String path) {
        java.io.File file = new java.io.File(path);
        return !file.exists() || file.delete();
    }

    /** The file's ASCII content, or null when it is missing, unreadable or longer than limit. */
    static String read(String path, int limit) {
        FileInputStream in = null;
        try {
            in = new FileInputStream(path);
            byte[] bytes = new byte[limit + 1];
            int count = 0, n;
            while (count < bytes.length && (n = in.read(bytes, count, bytes.length - count)) > 0) count += n;
            return count > limit ? null : new String(bytes, 0, count, "US-ASCII");
        } catch (Throwable t) {
            return null;
        } finally {
            if (in != null) { try { in.close(); } catch (Throwable t) { } }
        }
    }
}
