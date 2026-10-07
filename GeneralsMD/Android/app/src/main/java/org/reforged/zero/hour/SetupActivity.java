package org.reforged.zero.hour;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

public final class SetupActivity extends Activity {
    private static final int REQUEST_FOLDER = 4101;
    private boolean pickerOpen = false;
    private boolean permissionScreenOpen = false;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        new android.os.Handler(getMainLooper()).postDelayed(this::prepare, 150);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (permissionScreenOpen && hasDirectFileAccess()) {
            permissionScreenOpen = false;
            openPicker();
        }
    }

    private File stateDir() {
        File dir = getExternalFilesDir(null);
        if (dir != null && !dir.exists()) dir.mkdirs();
        return dir;
    }

    private File rootMarker() {
        File dir = stateDir();
        return dir == null ? null : new File(dir, ".zh-game-root");
    }

    // Base Generals is a separate asset set from Zero Hour. Keep its resolved path
    // beside the Zero Hour marker so native code can mount the correct unsuffixed
    // archives (INI.big / Terrain.big / Textures.big / W3D.big) explicitly.
    private File baseRootMarker() {
        File dir = stateDir();
        return dir == null ? null : new File(dir, ".zh-base-generals-root");
    }

    private boolean hasDirectFileAccess() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    private String readMarker(File marker) {
        if (marker == null || !marker.isFile()) return null;
        try {
            String value = new String(java.nio.file.Files.readAllBytes(marker.toPath()), StandardCharsets.UTF_8).trim();
            return value.isEmpty() ? null : value;
        } catch (Exception e) {
            return null;
        }
    }

    private String savedRoot() {
        return readMarker(rootMarker());
    }

    private String savedBaseRoot() {
        return readMarker(baseRootMarker());
    }

    private void prepare() {
        String root = savedRoot();
        if (root != null && inspect(new File(root)).complete()) {
            launchGame();
            return;
        }
        if (!hasDirectFileAccess()) showPermissionDialog();
        else openPicker();
    }

    private void showPermissionDialog() {
        permissionScreenOpen = true;
        new AlertDialog.Builder(this)
                .setTitle("الوصول إلى ملفات اللعبة")
                .setMessage("امنح التطبيق إذن «إدارة جميع الملفات» حتى يقرأ تثبيت Generals Zero Hour الأصلي مباشرة، بدون نسخ ملفات اللعبة إلى Android/data.")
                .setCancelable(false)
                .setPositiveButton("السماح", (d, w) -> requestAccess())
                .setNegativeButton("إغلاق", (d, w) -> finish())
                .show();
    }

    private void requestAccess() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            openPicker();
            return;
        }
        permissionScreenOpen = true;
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void openPicker() {
        if (pickerOpen || isFinishing()) return;
        pickerOpen = true;

        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION |
                Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_FOLDER);
        } catch (ActivityNotFoundException e) {
            pickerOpen = false;
            showError("لا يوجد مدير ملفات يدعم اختيار مجلدات على هذا الجهاز.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_FOLDER) return;
        pickerOpen = false;

        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            Toast.makeText(this, "لم يتم اختيار مجلد اللعبة.", Toast.LENGTH_LONG).show();
            return;
        }

        Uri uri = data.getData();
        try {
            int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            getContentResolver().takePersistableUriPermission(uri, flags);
        } catch (SecurityException ignored) {}

        String path = resolveTree(uri);
        if (path == null) {
            showError("اختر مجلدًا من الذاكرة المشتركة المحلية. هذا الإصدار يحتاج مسار ملفات حقيقي حتى يقرأ ملفات BIG من دون نسخها.");
            return;
        }

        File folder = new File(path);
        Status status = inspect(folder);
        showStatus(folder, status);
    }

    private String resolveTree(Uri uri) {
        if (!DocumentsContract.isTreeUri(uri) ||
                !"com.android.externalstorage.documents".equals(uri.getAuthority())) return null;

        final String id;
        try {
            id = DocumentsContract.getTreeDocumentId(uri);
        } catch (IllegalArgumentException e) {
            return null;
        }

        int sep = id.indexOf(':');
        if (sep <= 0) return null;

        String volume = id.substring(0, sep);
        String relative = id.substring(sep + 1);
        File base = "primary".equalsIgnoreCase(volume)
                ? Environment.getExternalStorageDirectory()
                : new File("/storage", volume);

        File result = relative.isEmpty() ? base : new File(base, relative);
        try {
            return result.getCanonicalPath();
        } catch (IOException e) {
            return result.getAbsolutePath();
        }
    }

    private Status inspect(File folder) {
        Status s = new Status();
        if (folder == null || !folder.isDirectory() || !folder.canRead()) {
            s.reason = "المجلد غير قابل للقراءة.";
            return s;
        }
        s.readable = true;

        // Zero Hour's own expansion archive. This is the primary install root.
        s.inizh = findFile(folder, "INIZH.big");
        s.texturesZH = findFile(folder, "TexturesZH.big");

        // Original Generals is a separate base-game asset set. Do not move the
        // user to a folder containing Textures.big just because it lacks "ZH":
        // Textures.big is SUPPOSED to be the original Generals archive.
        File savedBase = null;
        String savedBasePath = savedBaseRoot();
        if (savedBasePath != null) {
            File candidate = new File(savedBasePath);
            if (isBaseGeneralsFolder(candidate)) savedBase = candidate;
        }

        s.baseFolder = savedBase != null ? savedBase : resolveBaseGeneralsFolder(folder);
        if (s.baseFolder != null) {
            s.baseIni = findFile(s.baseFolder, "INI.big");
            s.baseTerrain = findFile(s.baseFolder, "Terrain.big");
            s.textures = findFile(s.baseFolder, "Textures.big");
            s.baseW3d = findFile(s.baseFolder, "W3D.big");
        }
        return s;
    }

    private boolean isBaseGeneralsFolder(File dir) {
        return dir != null && dir.isDirectory() && findFile(dir, "Textures.big") != null;
    }

    // Match the layouts accepted by the real engine and by MYSOREZ:
    //   1) base archives directly beside INIZH.big,
    //   2) the common Steam/Deluxe ZH_Generals subfolder,
    //   3) a sibling Command & Conquer Generals folder,
    //   4) a generic sibling Generals folder.
    private File resolveBaseGeneralsFolder(File zeroHourFolder) {
        if (zeroHourFolder == null || !zeroHourFolder.isDirectory()) return null;

        File direct = zeroHourFolder;
        if (isBaseGeneralsFolder(direct)) return direct;

        String[] insideNames = {
                "ZH_Generals",
                "z_generals",
                "Generals",
                "Command & Conquer Generals",
                "Command & Conquer(tm) Generals"
        };
        for (String name : insideNames) {
            File candidate = findDir(zeroHourFolder, name);
            if (isBaseGeneralsFolder(candidate)) return candidate;
        }

        File parent = zeroHourFolder.getParentFile();
        if (parent != null) {
            for (String name : insideNames) {
                File candidate = findDir(parent, name);
                if (isBaseGeneralsFolder(candidate)) return candidate;
            }
        }
        return null;
    }

    private File findFile(File dir, String wanted) {
        File[] files = dir == null ? null : dir.listFiles();
        if (files == null) return null;
        for (File f : files)
            if (f.isFile() && f.getName().equalsIgnoreCase(wanted)) return f;
        return null;
    }

    private File findDir(File dir, String wanted) {
        File[] files = dir == null ? null : dir.listFiles();
        if (files == null) return null;
        for (File f : files)
            if (f.isDirectory() && f.getName().equalsIgnoreCase(wanted)) return f;
        return null;
    }

    private void showStatus(File folder, Status s) {
        StringBuilder msg = new StringBuilder();
        msg.append("المجلد:\n").append(folder.getAbsolutePath()).append("\n\n");
        msg.append(s.inizh != null ? "✅ INIZH.big — Zero Hour موجود\n" : "❌ INIZH.big — Zero Hour مفقود\n");
        msg.append(s.texturesZH != null ? "✅ TexturesZH.big — موجود (اختياري)\n" : "ℹ️ TexturesZH.big — غير موجود، ليس مطلوبًا هنا\n");
        msg.append(s.baseFolder != null ? "✅ مجلد Generals الأصلي — تم العثور عليه\n" : "❌ مجلد Generals الأصلي — مفقود\n");
        msg.append(s.textures != null ? "✅ Textures.big — Generals الأصلي موجود\n" : "❌ Textures.big — Generals الأصلي مفقود\n");
        msg.append(s.baseIni != null ? "✅ INI.big — موجود\n" : "⚠️ INI.big — غير موجود\n");
        msg.append(s.baseTerrain != null ? "✅ Terrain.big — موجود\n" : "⚠️ Terrain.big — غير موجود\n");
        msg.append(s.baseW3d != null ? "✅ W3D.big — موجود\n" : "⚠️ W3D.big — غير موجود\n");
        if (s.baseFolder != null) msg.append("مجلد Generals: ").append(s.baseFolder.getAbsolutePath()).append("\n");

        if (s.complete()) {
            msg.append("\n✅ ملفات اللعبة الأساسية مكتملة.\nلن يتم نسخ أي ملفات BIG إلى Android/data.");
        } else {
            msg.append("\n⚠️ المجلد غير مكتمل. لن تبدأ اللعبة حتى تكتمل الملفات المطلوبة.");
        }

        AlertDialog.Builder b = new AlertDialog.Builder(this)
                .setTitle(s.complete() ? "✅ جاهز للتشغيل" : "⚠️ ملفات ناقصة")
                .setMessage(msg.toString()).setCancelable(false);

        if (s.complete()) {
            b.setPositiveButton("بدء اللعبة", (d, w) -> { saveRoot(folder, s.baseFolder); launchGame(); });
            b.setNegativeButton("اختيار آخر", (d, w) -> openPicker());
        } else {
            b.setPositiveButton("اختيار آخر", (d, w) -> openPicker());
            b.setNegativeButton("إغلاق", (d, w) -> finish());
        }
        b.show();
    }

    private void writeMarker(File marker, String path, String tempName) throws IOException {
        if (marker == null || path == null || path.isEmpty()) throw new IOException("marker unavailable");
        File temp = new File(marker.getParentFile(), tempName);
        try (FileOutputStream out = new FileOutputStream(temp, false)) {
            out.write((new File(path).getCanonicalPath() + "\n").getBytes(StandardCharsets.UTF_8));
            out.flush();
            out.getFD().sync();
            if (marker.exists() && !marker.delete()) throw new IOException("old marker delete failed");
            if (!temp.renameTo(marker)) throw new IOException("rename failed");
        } catch (Exception e) {
            temp.delete();
            throw e;
        }
    }

    private void saveRoot(File folder, File baseFolder) {
        File marker = rootMarker();
        File baseMarker = baseRootMarker();
        if (marker == null || baseFolder == null || baseMarker == null) {
            showError("تعذر حفظ مسارات ملفات اللعبة.");
            return;
        }
        try {
            writeMarker(marker, folder.getCanonicalPath(), ".zh-game-root.new");
            writeMarker(baseMarker, baseFolder.getCanonicalPath(), ".zh-base-generals-root.new");
        } catch (Exception e) {
            showError("تعذر حفظ مسارات ملفات اللعبة.");
        }
    }

    private void launchGame() {
        startActivity(new Intent(this, GeneralsActivity.class));
        finish();
    }

    private void showError(String message) {
        new AlertDialog.Builder(this).setTitle("إعداد اللعبة")
                .setMessage(message)
                .setPositiveButton("اختيار مجلد", (d, w) -> openPicker())
                .setNegativeButton("إغلاق", (d, w) -> finish())
                .show();
    }

    private static final class Status {
        boolean readable;
        File inizh;
        File texturesZH;
        File baseFolder;
        File baseIni;
        File baseTerrain;
        File textures;
        File baseW3d;
        String reason = "";
        boolean complete() { return readable && inizh != null && baseFolder != null && textures != null; }
    }
}
