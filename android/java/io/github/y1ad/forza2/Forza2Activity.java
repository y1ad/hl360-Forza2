package io.github.y1ad.forza2;

import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.List;

import org.libsdl.app.SDLActivity;

/**
 * SDL's activity with the game's folders and arguments. Adapted from
 * PinyonShiftActivity of arcanite24/pinyon-shift (BSD 3-Clause, Copyright (c)
 * 2026 Pinyon Shift contributors).
 *
 * The native side reads one folder from the environment, FORZA2_ANDROID_ROOT:
 * the settings copied out of the APK, the player's forza2.user.toml, game/
 * (the files extracted from the player's own disc), state/ (saves, caches,
 * custom GPU drivers) and logs/. It is /sdcard/Forza2 once the player allowed
 * all-files access (the app asks once), the app's own external files folder
 * until then.
 *
 * Tooling can override any variable with a string extra named env.NAME and
 * pass game arguments as a string-array extra named args:
 *
 *   adb shell am start -n io.github.y1ad.forza2/.Forza2Activity \
 *       --esa args --gpu_backend=null
 */
public class Forza2Activity extends SDLActivity {
    private static final String TAG = "Forza2";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        File root = storageRoot();
        //noinspection ResultOfMethodCallIgnored
        root.mkdirs();
        //noinspection ResultOfMethodCallIgnored
        new File(root, "game").mkdirs();
        File drivers = new File(root, "state/drivers");
        //noinspection ResultOfMethodCallIgnored
        drivers.mkdirs();
        // The shipped settings are refreshed at every start; the player's
        // choices live in forza2.user.toml, which is never touched.
        copyAsset("forza2.toml", new File(root, "forza2.toml"));
        copyAsset("forza2.android.toml", new File(root, "forza2.android.toml"));
        setDefaultEnvironment("HOME", getFilesDir().getAbsolutePath());
        setEnvironment("FORZA2_ANDROID_ROOT", root.getAbsolutePath());
        // Custom GPU drivers (Mesa Turnip) the player copied in, chosen with
        // android_gpu_driver; the runtime copies the chosen one inside.
        setDefaultEnvironment("REX_ANDROID_DRIVERS_DIR", drivers.getAbsolutePath());
        Intent intent = getIntent();
        Bundle extras = intent != null ? intent.getExtras() : null;
        if (extras != null) {
            for (String key : extras.keySet()) {
                if (key.startsWith("env.")) {
                    Object value = extras.get(key);
                    if (value != null) {
                        setEnvironment(key.substring(4), value.toString());
                    }
                }
            }
        }
        super.onCreate(savedInstanceState);
        // A race played on a controller touches nothing, and a screen that
        // times out sends the game to the background, where it pauses.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    private File storageRoot() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (Environment.isExternalStorageManager()) {
                return new File(Environment.getExternalStorageDirectory(), "Forza2");
            }
            SharedPreferences prefs = getSharedPreferences("forza2", MODE_PRIVATE);
            if (!prefs.getBoolean("asked_all_files", false)) {
                prefs.edit().putBoolean("asked_all_files", true).apply();
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                            Uri.parse("package:" + getPackageName())));
                } catch (Exception error) {
                    Log.w(TAG, "all-files access screen not available", error);
                }
            }
        }
        File external = getExternalFilesDir(null);
        return external != null ? external : getFilesDir();
    }

    private void copyAsset(String name, File target) {
        try (InputStream in = getAssets().open(name);
             OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[8192];
            for (int read; (read = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, read);
            }
        } catch (IOException error) {
            Log.w(TAG, name + " not copied out of the package", error);
        }
    }

    // The C++ runtime first, then the ReXGlue runtime, which holds SDL and
    // registers its Java natives when loaded, then the game (libmain.so),
    // whose SDL_main SDL calls. The GPU plugin is a dependency of libmain.
    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    // The native libraries are built for ARMv8.2-A: an older core would crash
    // on the first LSE atomic, possibly in a static initializer. SDL shows a
    // link error from here as a dialog instead.
    @Override
    public void loadLibraries() {
        if (!cpuHasFeatures("atomics", "asimdrdm")) {
            throw new UnsatisfiedLinkError("This build needs an ARMv8.2 CPU "
                    + "(Cortex-A55, Cortex-A75 or newer); this device's CPU is older.");
        }
        super.loadLibraries();
    }

    // True when /proc/cpuinfo cannot be read, so an unusual kernel does not
    // lock out a CPU that would run the game.
    private static boolean cpuHasFeatures(String... features) {
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/cpuinfo"))) {
            for (String line; (line = reader.readLine()) != null; ) {
                if (!line.startsWith("Features")) continue;
                List<String> listed = Arrays.asList(line.substring(line.indexOf(':') + 1).trim().split("\\s+"));
                for (String feature : features) {
                    if (!listed.contains(feature)) return false;
                }
                return true;
            }
        } catch (IOException ignored) {
            // Unreadable: let the libraries load.
        }
        return true;
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        String[] given = intent != null ? intent.getStringArrayExtra("args") : null;
        return given != null ? given : new String[0];
    }

    private static void setDefaultEnvironment(String name, String value) {
        if (Os.getenv(name) == null) {
            setEnvironment(name, value);
        }
    }

    private static void setEnvironment(String name, String value) {
        try {
            Os.setenv(name, value, true);
        } catch (ErrnoException error) {
            Log.e(TAG, "setenv " + name + " failed", error);
        }
    }
}
