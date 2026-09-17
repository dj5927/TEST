package com.reblue.android;

import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileWriter;

public class ReBlueActivity extends SDLActivity {
    private Vibrator phoneVibrator;
    private Handler vibrationHandler;
    private int phoneVibrationAmplitude;

    private File traceFile() {
        String path = getSharedPreferences("launcher", MODE_PRIVATE)
                .getString("game_path", "");
        if (path == null || path.isEmpty()) {
            return null;
        }
        File chosen = new File(path);
        File gameDir = new File(chosen, "default.xex").isFile()
                ? chosen : new File(chosen, "game");
        return new File(gameDir, "reblue_android_trace.txt");
    }

    private synchronized void trace(String message) {
        File file = traceFile();
        if (file == null) {
            return;
        }
        try (FileWriter writer = new FileWriter(file, true)) {
            writer.write("JAVA time_ms=" + System.currentTimeMillis()
                    + " thread=" + Thread.currentThread().getName()
                    + " " + message + "\n");
            writer.flush();
        } catch (Exception ignored) {
        }
    }

    private final Runnable vibrationPulse = new Runnable() {
        @Override
        public void run() {
            if (phoneVibrationAmplitude <= 0 || phoneVibrator == null) {
                return;
            }
            int amplitude = phoneVibrator.hasAmplitudeControl()
                    ? phoneVibrationAmplitude : VibrationEffect.DEFAULT_AMPLITUDE;
            phoneVibrator.vibrate(VibrationEffect.createOneShot(300, amplitude));
            vibrationHandler.postDelayed(this, 220);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        trace("ReBlueActivity.onCreate BEFORE super");
        super.onCreate(savedInstanceState);
        trace("ReBlueActivity.onCreate AFTER super");
        phoneVibrator = (Vibrator) getSystemService(VIBRATOR_SERVICE);
        vibrationHandler = new Handler(Looper.getMainLooper());
    }

    @Override
    protected void main() {
        trace("SDL native main ENTER");
        try {
            super.main();
            trace("SDL native main RETURNED normally");
        } catch (RuntimeException | Error t) {
            trace("SDL native main THREW " + t.getClass().getName()
                    + ": " + String.valueOf(t.getMessage()));
            throw t;
        }
    }

    public void setPhoneVibration(final int requestedAmplitude) {
        final int amplitude = Math.max(0, Math.min(255, requestedAmplitude));
        runOnUiThread(() -> {
            if (amplitude == phoneVibrationAmplitude) {
                return;
            }
            phoneVibrationAmplitude = amplitude;
            if (vibrationHandler == null || phoneVibrator == null) {
                return;
            }
            vibrationHandler.removeCallbacks(vibrationPulse);
            if (amplitude == 0) {
                phoneVibrator.cancel();
            } else {
                vibrationPulse.run();
            }
        });
    }

    @Override
    protected void onPause() {
        trace("ReBlueActivity.onPause ENTER finishing=" + isFinishing());
        if (vibrationHandler != null) {
            vibrationHandler.removeCallbacks(vibrationPulse);
        }
        if (phoneVibrator != null) {
            phoneVibrator.cancel();
        }
        super.onPause();
        trace("ReBlueActivity.onPause EXIT finishing=" + isFinishing());
    }

    @Override
    protected void onResume() {
        trace("ReBlueActivity.onResume ENTER finishing=" + isFinishing());
        super.onResume();
        trace("ReBlueActivity.onResume AFTER super finishing=" + isFinishing());
        if (phoneVibrationAmplitude > 0 && vibrationHandler != null) {
            vibrationHandler.removeCallbacks(vibrationPulse);
            vibrationHandler.post(vibrationPulse);
        }
    }

    @Override
    protected void onStop() {
        trace("ReBlueActivity.onStop ENTER finishing=" + isFinishing());
        super.onStop();
        trace("ReBlueActivity.onStop EXIT finishing=" + isFinishing());
    }

    @Override
    protected void onDestroy() {
        trace("ReBlueActivity.onDestroy ENTER finishing=" + isFinishing());
        if (vibrationHandler != null) {
            vibrationHandler.removeCallbacks(vibrationPulse);
        }
        if (phoneVibrator != null) {
            phoneVibrator.cancel();
        }
        super.onDestroy();
        trace("ReBlueActivity.onDestroy EXIT finishing=" + isFinishing());
    }

    @Override
    protected String[] getLibraries() {
        // librexruntime contains the exact SDL3 build whose Java glue is
        // packaged with this APK. libmain is the re:Blue Android host.
        return new String[] { "rexruntime", "main" };
    }
}
