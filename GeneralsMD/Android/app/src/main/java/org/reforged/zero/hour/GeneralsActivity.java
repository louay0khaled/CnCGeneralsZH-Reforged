package org.reforged.zero.hour;

import android.os.Bundle;
import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import org.libsdl.app.SDLActivity;

/**
 * Actual game activity. It runs in an isolated Android process so native renderer
 * crashes cannot tear down the launcher Activity/process.
 */
public final class GeneralsActivity extends SDLActivity {
    private Thread.UncaughtExceptionHandler previousHandler;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        previousHandler = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            writeJavaCrashReport(thread, throwable);
            if (previousHandler != null) previousHandler.uncaughtException(thread, throwable);
        });
    }

    private void writeJavaCrashReport(Thread thread, Throwable throwable) {
        File logs = new File(new File(getFilesDir(), "ZeroHourData"), "Logs");
        if (!logs.isDirectory()) logs.mkdirs();

        StringWriter stack = new StringWriter();
        throwable.printStackTrace(new PrintWriter(stack));
        String body =
                "Java crash in isolated Generals game process\\n" +
                "Thread: " + thread.getName() + " (id " + thread.getId() + ")\\n" +
                "Time: " + System.currentTimeMillis() + "\\n\\n" +
                stack + "\\n";

        File report = new File(logs, "JavaCrashInfo.txt");
        try (FileOutputStream out = new FileOutputStream(report, false)) {
            out.write(body.getBytes(StandardCharsets.UTF_8));
            out.flush();
            out.getFD().sync();
        } catch (Exception ignored) {}
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
