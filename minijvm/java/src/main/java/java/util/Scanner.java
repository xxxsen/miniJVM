/*
 * Minimal line-oriented Scanner implementation for compact profiles.
 */
package java.util;

import java.io.BufferedReader;
import java.io.Closeable;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;

/**
 * Implements the line-reading subset used by MIDP applications and emulator
 * helpers without pulling the regular-expression engine into miniJVM.
 */
public final class Scanner implements Closeable {
    private final BufferedReader reader;
    private String pendingLine;
    private boolean finished;

    public Scanner(InputStream source) {
        if (source == null) throw new NullPointerException("source");
        reader = new BufferedReader(new InputStreamReader(source));
    }

    public boolean hasNextLine() {
        if (pendingLine != null) return true;
        if (finished) return false;
        try {
            pendingLine = reader.readLine();
            if (pendingLine == null) finished = true;
            return pendingLine != null;
        } catch (IOException error) {
            finished = true;
            return false;
        }
    }

    public String nextLine() {
        if (!hasNextLine()) throw new NoSuchElementException();
        String line = pendingLine;
        pendingLine = null;
        return line;
    }

    public void close() {
        finished = true;
        pendingLine = null;
        try {
            reader.close();
        } catch (IOException ignored) {
        }
    }
}
