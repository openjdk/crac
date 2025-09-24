package jdk.test.lib.crac;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

public class AsyncStreamReader {
    private final LinkedBlockingQueue<String> lines = new LinkedBlockingQueue<>();
    private volatile boolean isRunning = true;

    public AsyncStreamReader(InputStream stream) {
        Thread t = new Thread(() -> {
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(stream))) {
                for (String line; (line = reader.readLine()) != null;) {
                    lines.put(line);
                }
            } catch (IOException | InterruptedException e) {
                e.printStackTrace();
            } finally {
                isRunning = false;
            }
        });
        t.setDaemon(true);
        t.start();
    }

    public String readLine(long timeoutMillis) throws TimeoutException, InterruptedException {
        String line = lines.poll(timeoutMillis, TimeUnit.MILLISECONDS);
        if (line == null) {
            throw new TimeoutException("Timeout " + timeoutMillis + " msecs while waiting for new line");
        }
        return line;
    }

    public boolean isRunning() {
        return isRunning;
    }
}
