package com.reblue.android;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.system.Os;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class LauncherActivity extends Activity {
    private static final int REQUEST_TREE = 1001;
    private static final int REQUEST_ALL_FILES = 1002;
    private static final int REQUEST_READ_STORAGE = 1003;
    private static final String PREFS = "launcher";
    private static final String KEY_GAME_PATH = "game_path";

    private TextView status;
    private boolean pickAfterPermission;
    private String lastDataError;

    private static final class DataProfile {
        String language = "us";
        int userLanguage = 1;
        int voiceType = 1;
        boolean korean;
        String description = "NTSC-U / English";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        super.onCreate(savedInstanceState);
        if (tryLaunchGame()) {
            return;
        }
        buildUi();
        updateStatus(lastDataError);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (status == null) {
            return;
        }
        if (pickAfterPermission && hasBroadStorageAccess()) {
            pickAfterPermission = false;
            openFolderPicker();
            return;
        }
        if (tryLaunchGame()) {
            return;
        }
        updateStatus(lastDataError);
    }

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private File selectedGameDir() {
        String path = prefs().getString(KEY_GAME_PATH, "");
        if (path == null || path.isEmpty()) {
            return null;
        }
        return normalizeGameDir(new File(path));
    }

    private File normalizeGameDir(File chosen) {
        if (chosen == null) {
            return null;
        }
        if (new File(chosen, "default.xex").isFile()) {
            return chosen;
        }
        File nested = new File(chosen, "game");
        if (new File(nested, "default.xex").isFile()) {
            return nested;
        }
        return chosen;
    }

    private File findLegacyGameDir() {
        File[] mediaDirs = getExternalMediaDirs();
        if (mediaDirs != null) {
            for (File media : mediaDirs) {
                if (media == null) {
                    continue;
                }
                File candidate = new File(media, "reblue/game");
                if (new File(candidate, "default.xex").isFile()) {
                    return candidate;
                }
                File legacyKr = new File(media, "BlueDragonKR/game");
                if (new File(legacyKr, "default.xex").isFile()) {
                    return legacyKr;
                }
            }
        }
        File externalFiles = getExternalFilesDir(null);
        if (externalFiles != null) {
            File candidate = new File(externalFiles, "reblue/game");
            if (new File(candidate, "default.xex").isFile()) {
                return candidate;
            }
        }
        return null;
    }

    private File findUsableGameDir() {
        File selected = selectedGameDir();
        if (selected != null && new File(selected, "default.xex").isFile()) {
            return selected;
        }
        return findLegacyGameDir();
    }

    private boolean tryLaunchGame() {
        File gameDir = findUsableGameDir();
        if (gameDir == null) {
            lastDataError = "Could not find a game folder containing default.xex.";
            return false;
        }
        File diagFile = prepareDiagFile(gameDir);
        if (diagFile == null) {
            if (lastDataError == null || lastDataError.isEmpty()) {
                lastDataError = "Could not create the Android trace file.\n\n"
                        + new File(gameDir, "reblue_android_trace.txt").getAbsolutePath();
            }
            return false;
        }
        String invalid = validateGameData(gameDir);
        if (invalid != null) {
            lastDataError = invalid;
            return false;
        }
        File configFile = prepareExternalConfig(gameDir);
        if (configFile == null) {
            lastDataError = "Could not create the external settings file.\n\n"
                    + "Make sure the parent folder of game is writable.";
            return false;
        }
        try {
            Os.setenv("REBLUE_GAME_DATA_ROOT", gameDir.getAbsolutePath(), true);
            Os.setenv("REBLUE_CONFIG_PATH", configFile.getAbsolutePath(), true);
            Os.setenv("REBLUE_DIAG_FILE", diagFile.getAbsolutePath(), true);
        } catch (Exception e) {
            lastDataError = "Failed to configure the game-data path:\n" + e.getMessage();
            updateStatus(lastDataError);
            return false;
        }
        lastDataError = null;
        startActivity(new Intent(this, ReBlueActivity.class));
        finish();
        return true;
    }

    private File prepareExternalConfig(File gameDir) {
        File installRoot = gameDir.getParentFile();
        if (installRoot == null) {
            return null;
        }
        File config = new File(installRoot, "reblue.toml");
        if (config.isFile()) {
            return config;
        }
        DataProfile profile = detectDataProfile(gameDir);
        try (FileWriter writer = new FileWriter(config, false)) {
            writer.write("# re:Blue Android - user editable settings\n");
            writer.write("# This file is intentionally outside Android/data.\n\n");
            writer.write("config_version = 2\n");
            writer.write("user_language = " + profile.userLanguage + "\n");
            writer.write("bd_language = \"" + profile.language + "\"\n");
            writer.write("bd_opt_voice_type = " + profile.voiceType + "\n");
            writer.write("bd_opt_msg_speed = 3\n");
            writer.write("bd_opt_msg_size = 1\n");
            writer.write("bd_opt_audio_hints = 0\n");
            writer.write("resolution = \"1280x720\"\n");
            writer.write("fullscreen = true\n");
            writer.write("bd_aspect_ratio = 0\n");
            writer.write("bd_fov_offset = 0\n");
            writer.write("bd_supersampling = 1\n");
            writer.write("bd_msaa = 0\n");
            writer.write("bd_render_scale = 50\n");
            writer.write("bd_anisotropy = 0\n");
            writer.write("bd_shadow_dimension = 512\n");
            writer.write("bd_shadow_distance = 1\n");
            writer.write("bd_post_quality = 0\n");
            writer.write("bd_reflection_quality = 0\n");
            writer.write("bd_dof_strength = 0\n");
            writer.write("bd_ntsc_filter = false\n");
            writer.write("bd_vsync = false\n");
            writer.write("bd_scene_color_r11g11b10 = true\n");
            writer.write("bd_perf_csv = false\n");
            writer.write("bd_profiler = false\n");
            writer.write("bd_perf_overlay = 0\n");
            writer.write("bd_mod_log = false\n");
            writer.write("bd_dbgprint = false\n");
            writer.write("bd_devmode = false\n");
            writer.write("bd_update_check = false\n");
            writer.write("bd_fps_limit = 60\n");
            writer.flush();
            return config;
        } catch (Exception ignored) {
            return null;
        }
    }

    private File prepareDiagFile(File gameDir) {
        File installRoot = gameDir.getParentFile();
        File diag = new File(gameDir, "reblue_android_trace.txt");
        try (FileWriter writer = new FileWriter(diag, false)) {
            DataProfile profile = detectDataProfile(gameDir);
            writer.write("re:Blue Android preview trace\n");
            writer.write("time_ms=" + System.currentTimeMillis() + "\n");
            writer.write("version_code=1\n");
            writer.write("detected_profile=" + profile.description + "\n");
            writer.write("detected_language=" + profile.language + "\n");
            writer.write("detected_voice_type=" + profile.voiceType + "\n");
            writer.write("game_dir=" + gameDir.getAbsolutePath() + "\n");
            writer.write("diag_path=" + diag.getAbsolutePath() + "\n");
            writer.write("install_root=" + (installRoot == null ? "<none>" : installRoot.getAbsolutePath()) + "\n");
            if (installRoot != null) {
                writer.write("config_path=" + new File(installRoot, "reblue.toml").getAbsolutePath() + "\n");
                writer.write("config_exists=" + new File(installRoot, "reblue.toml").isFile() + "\n");
            }
            writer.write("android_sdk=" + Build.VERSION.SDK_INT + "\n");
            writer.write("device_manufacturer=" + Build.MANUFACTURER + "\n");
            writer.write("device_model=" + Build.MODEL + "\n");
            writer.write("device_name=" + Build.DEVICE + "\n");
            writer.write("device_hardware=" + Build.HARDWARE + "\n");
            if (Build.VERSION.SDK_INT >= 31) {
                writer.write("soc_model=" + Build.SOC_MODEL + "\n");
                writer.write("soc_manufacturer=" + Build.SOC_MANUFACTURER + "\n");
            }
            writer.write("all_files_access=" + hasBroadStorageAccess() + "\n");
            writer.write("game_can_read=" + gameDir.canRead() + "\n");
            writer.write("game_can_write=" + gameDir.canWrite() + "\n");
            writer.write("default_xex=" + new File(gameDir, "default.xex").isFile() + "\n");
            writer.write("launcher_probe=PUBLIC_PREVIEW_OK\n");
            writer.write("descriptor_strategy=adaptive_bindless_or_mobile_core\n");
            writer.write("TRACE launcher about_to_start_ReBlueActivity\n");
            writer.flush();
        } catch (Exception e) {
            lastDataError = "Android trace-file creation failed\n\n"
                    + "Path:\n" + diag.getAbsolutePath() + "\n\n"
                    + "Error: " + e.getClass().getSimpleName() + "\n"
                    + String.valueOf(e.getMessage());
            return null;
        }
        if (!diag.isFile() || diag.length() <= 0) {
            lastDataError = "Android trace-file validation failed\n\n"
                    + "Path:\n" + diag.getAbsolutePath() + "\n\n"
                    + "exists=" + diag.isFile() + " length=" + diag.length()
                    + " game_can_write=" + gameDir.canWrite();
            return null;
        }
        return diag;
    }

    private String validateGameData(File gameDir) {
        File xex = new File(gameDir, "default.xex");
        if (!xex.isFile()) {
            return "Invalid game data: default.xex is missing.\n\n" + xex.getAbsolutePath();
        }
        File boot = new File(gameDir, "bd_boot.ini");
        if (!boot.isFile()) {
            return "Invalid game data: bd_boot.ini is missing.\n\n" + boot.getAbsolutePath();
        }

        String hash = sha256(xex);
        if (hash != null && isKnownKoreanRetailXex(hash)) {
            return "Korean retail default.xex cannot be used directly with re:Blue.\n\n"
                    + "re:Blue is statically recompiled from the NTSC-U executable. "
                    + "Keep the NTSC-U base default.xex and import the Korean retail data instead.\n\n"
                    + "Detected XEX SHA-256:\n" + hash;
        }
        return null;
    }

    private boolean isKnownKoreanRetailXex(String sha) {
        String upper = sha.toUpperCase(Locale.ROOT);
        return upper.equals("1C3C47B56DE6F876E98A8C5D0CDABBDEA20A09045FEC049FA9DAEE921BF17F6C")
                || upper.equals("941F771E08CF086F31EE3E155CFC2866543BB4E28C2B2531A7D5967814AC7FCE")
                || upper.equals("18CD5F0AFDF1B6D7B1B627F5AEF452BA7BB7F9925846F105106A4BCE9ACAA474");
    }

    private String sha256(File file) {
        try (FileInputStream in = new FileInputStream(file)) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[1024 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                digest.update(buffer, 0, read);
            }
            StringBuilder out = new StringBuilder(64);
            for (byte b : digest.digest()) {
                out.append(String.format(Locale.ROOT, "%02X", b & 0xFF));
            }
            return out.toString();
        } catch (Exception ignored) {
            return null;
        }
    }

    private DataProfile detectDataProfile(File gameDir) {
        DataProfile profile = new DataProfile();
        File boot = new File(gameDir, "bd_boot.ini");
        List<String> languages = new ArrayList<>();
        List<String> voices = new ArrayList<>();
        try (BufferedReader reader = new BufferedReader(new FileReader(boot))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String trimmed = line.trim();
                if (trimmed.startsWith("[Language]")) {
                    addCodes(trimmed.substring("[Language]".length()), languages);
                } else if (trimmed.startsWith("[Voice]")) {
                    addCodes(trimmed.substring("[Voice]".length()), voices);
                }
            }
        } catch (Exception ignored) {
        }

        File packmemKr = new File(gameDir, "pack/packmem_kr.ipk");
        File installRoot = gameDir.getParentFile();
        File krOverlay = installRoot == null ? null : new File(installRoot, "mods/bd_asia_text");
        int krVoiceIndex = indexOfCode(voices, "KR");
        boolean hasKoreanText = containsCode(languages, "KR") && packmemKr.isFile()
                && krOverlay != null && krOverlay.isDirectory();
        if (hasKoreanText && krVoiceIndex >= 0) {
            profile.language = "kr";
            profile.userLanguage = 7;
            profile.voiceType = krVoiceIndex + 1;
            profile.korean = true;
            profile.description = "NTSC-U base + Korean retail data";
        }
        return profile;
    }

    private void addCodes(String text, List<String> out) {
        for (String token : text.trim().split("\\s+")) {
            if (!token.isEmpty()) {
                out.add(token.toUpperCase(Locale.ROOT));
            }
        }
    }

    private boolean containsCode(List<String> codes, String wanted) {
        return indexOfCode(codes, wanted) >= 0;
    }

    private int indexOfCode(List<String> codes, String wanted) {
        for (int i = 0; i < codes.size(); i++) {
            if (wanted.equalsIgnoreCase(codes.get(i))) {
                return i;
            }
        }
        return -1;
    }

    private boolean hasBroadStorageAccess() {
        if (Build.VERSION.SDK_INT >= 30) {
            return Environment.isExternalStorageManager();
        }
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    private void requestStorageAccessThenPick() {
        if (hasBroadStorageAccess()) {
            openFolderPicker();
            return;
        }
        pickAfterPermission = true;
        if (Build.VERSION.SDK_INT >= 30) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            try {
                startActivityForResult(intent, REQUEST_ALL_FILES);
            } catch (Exception ignored) {
                startActivityForResult(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
                        REQUEST_ALL_FILES);
            }
        } else {
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE },
                    REQUEST_READ_STORAGE);
        }
    }

    private void openFolderPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_TREE);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_TREE || resultCode != RESULT_OK || data == null
                || data.getData() == null) {
            return;
        }

        Uri treeUri = data.getData();
        int flags = data.getFlags()
                & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            getContentResolver().takePersistableUriPermission(treeUri, flags);
        } catch (Exception ignored) {
        }

        File rawDir = rawPathFromExternalStorageTree(treeUri);
        if (rawDir == null) {
            updateStatus("This location cannot be converted to a normal file-system path.\n\n"
                    + "Choose a folder on internal storage or a physical SD card.\n"
                    + "Cloud-drive providers are not supported by this preview.");
            return;
        }

        File gameDir = normalizeGameDir(rawDir);
        if (!new File(gameDir, "default.xex").isFile()) {
            updateStatus("The selected folder does not contain default.xex.\n\n"
                    + "Choose either the game folder itself or its parent folder containing game/.\n\n"
                    + "Selected path:\n" + rawDir.getAbsolutePath());
            return;
        }

        prefs().edit().putString(KEY_GAME_PATH, gameDir.getAbsolutePath()).apply();
        if (!tryLaunchGame()) {
            updateStatus(lastDataError);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_READ_STORAGE && hasBroadStorageAccess()) {
            pickAfterPermission = false;
            openFolderPicker();
        } else if (requestCode == REQUEST_READ_STORAGE) {
            pickAfterPermission = false;
            updateStatus("Storage read access is required.");
        }
    }

    private File rawPathFromExternalStorageTree(Uri treeUri) {
        if (!"com.android.externalstorage.documents".equals(treeUri.getAuthority())) {
            return null;
        }
        String treeId;
        try {
            treeId = DocumentsContract.getTreeDocumentId(treeUri);
        } catch (Exception e) {
            return null;
        }
        int colon = treeId.indexOf(':');
        String volumeId = colon >= 0 ? treeId.substring(0, colon) : treeId;
        String relative = colon >= 0 ? treeId.substring(colon + 1) : "";

        File volumeRoot = null;
        StorageManager manager = (StorageManager) getSystemService(STORAGE_SERVICE);
        if (manager != null) {
            List<StorageVolume> volumes = manager.getStorageVolumes();
            for (StorageVolume volume : volumes) {
                boolean match = "primary".equalsIgnoreCase(volumeId)
                        ? volume.isPrimary()
                        : volumeId.equalsIgnoreCase(volume.getUuid());
                if (!match) {
                    continue;
                }
                if (Build.VERSION.SDK_INT >= 30) {
                    volumeRoot = volume.getDirectory();
                } else if (volume.isPrimary()) {
                    volumeRoot = Environment.getExternalStorageDirectory();
                } else if (volume.getUuid() != null) {
                    volumeRoot = new File("/storage", volume.getUuid());
                }
                break;
            }
        }
        if (volumeRoot == null) {
            if ("primary".equalsIgnoreCase(volumeId)) {
                volumeRoot = Environment.getExternalStorageDirectory();
            } else if (!volumeId.isEmpty()) {
                volumeRoot = new File("/storage", volumeId);
            }
        }
        return volumeRoot == null ? null
                : (relative.isEmpty() ? volumeRoot : new File(volumeRoot, relative));
    }

    private String selectedPathText() {
        String path = prefs().getString(KEY_GAME_PATH, "");
        return path == null || path.isEmpty() ? "No game-data folder selected" : path;
    }

    private void updateStatus(String extra) {
        if (status == null) {
            return;
        }
        String permission = hasBroadStorageAccess()
                ? "Storage access: granted"
                : "Storage access: permission will be requested when choosing a folder";
        String text = "Select a re:Blue game-data folder.\n\n"
                + "NTSC-U / English data is the default supported base.\n"
                + "A prepared NTSC-U + Korean retail-data install is detected automatically.\n"
                + "Select either the folder containing game/ or the game folder itself.\n\n"
                + permission + "\n\nCurrent selection:\n" + selectedPathText();
        if (extra != null && !extra.isEmpty()) {
            text += "\n\n" + extra;
        }
        status.setText(text);
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        status = new TextView(this);
        status.setTextSize(18.0f);
        status.setGravity(Gravity.CENTER);
        root.addView(status, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        Button choose = new Button(this);
        choose.setText("Choose / change game-data folder");
        choose.setOnClickListener(v -> requestStorageAccessThenPick());
        root.addView(choose);

        Button retry = new Button(this);
        retry.setText("Validate and launch");
        retry.setOnClickListener(v -> {
            if (!tryLaunchGame()) {
                updateStatus(lastDataError);
            }
        });
        root.addView(retry);

        Button clear = new Button(this);
        clear.setText("Clear selected path");
        clear.setOnClickListener(v -> {
            prefs().edit().remove(KEY_GAME_PATH).apply();
            updateStatus("Saved game-data path cleared.");
        });
        root.addView(clear);

        setContentView(root);
    }
}
