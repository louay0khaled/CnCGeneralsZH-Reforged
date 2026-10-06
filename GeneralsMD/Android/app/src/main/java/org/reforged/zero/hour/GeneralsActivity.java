package org.reforged.zero.hour;

import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * Android shell for Zero Hour Reforged.
 *
 * The native engine needs a real filesystem path for the original game install. Android's
 * Storage Access Framework returns a tree URI rather than a POSIX path, so this activity uses
 * the SAF folder picker to identify the install and requires All Files Access before passing
 * the resolved shared-storage path to native code. No game files are packaged or copied by
 * the app merely to make the user responsible for managing them.
 */
public class GeneralsActivity extends SDLActivity {
    private static final int REQUEST_GAME_FOLDER = 4101;
    private static final int REQUEST_ALL_FILES_ACCESS = 4102;
    private static final long STARTUP_CHECK_DELAY_MS = 350L;

    private boolean folderPickerInFlight = false;
    private boolean waitingForAllFilesAccess = false;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        new Handler(Looper.getMainLooper()).postDelayed(this::prepareGameDataAccess, STARTUP_CHECK_DELAY_MS);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (waitingForAllFilesAccess) {
            if (hasAllFilesAccess()) {
                waitingForAllFilesAccess = false;
                launchFolderPicker();
            }
        }
    }

    private File stateDirectory() {
        File dir = getExternalFilesDir(null);
        if (dir != null && !dir.exists()) {
            // Best effort; Android creates this directory for normal app startup.
            dir.mkdirs();
        }
        return dir;
    }

    private File readyMarker() {
        File dir = stateDirectory();
        return dir == null ? null : new File(dir, ".zh-game-root");
    }

    private File cancelledMarker() {
        File dir = stateDirectory();
        return dir == null ? null : new File(dir, ".zh-game-root.cancelled");
    }

    private void clearStateMarkers() {
        File ready = readyMarker();
        File cancelled = cancelledMarker();
        if (ready != null) ready.delete();
        if (cancelled != null) cancelled.delete();
    }

    private boolean hasAllFilesAccess() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    private void prepareGameDataAccess() {
        String readyRoot = readReadyRoot();
        if (readyRoot != null && inspectGameFolder(new File(readyRoot)).isComplete()) {
            return;
        }

        clearStateMarkers();

        if (!hasAllFilesAccess()) {
            waitingForAllFilesAccess = true;
            showAllFilesAccessDialog();
            return;
        }

        launchFolderPicker();
    }

    private void showAllFilesAccessDialog() {
        new AlertDialog.Builder(this)
                .setTitle("الوصول إلى ملفات اللعبة")
                .setMessage(
                        "حتى تعمل اللعبة من مجلد تثبيتك الأصلي بدون نسخ الملفات إلى Android/data، "
                                + "نحتاج إذن \"إدارة جميع الملفات\".\n\n"
                                + "بعد السماح، ستختار مجلد Zero Hour من مدير الملفات، ثم سيفحص التطبيق "
                                + "INIZH.big و Textures.big ويخبرك مباشرة بما هو ناقص.")
                .setCancelable(false)
                .setPositiveButton("السماح", (dialog, which) -> requestAllFilesAccess())
                .setNegativeButton("إغلاق", (dialog, which) -> markCancelled())
                .show();
    }

    private void requestAllFilesAccess() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            launchFolderPicker();
            return;
        }

        waitingForAllFilesAccess = true;
        try {
            Intent intent = new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(intent);
        } catch (ActivityNotFoundException e) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (ActivityNotFoundException ignored) {
                waitingForAllFilesAccess = false;
                showFatalSetupMessage(
                        "لم يستطع النظام فتح صفحة إذن إدارة الملفات.\n\n"
                                + "افتح إعدادات التطبيق يدويًا وامنحه إذن الوصول إلى جميع الملفات.");
            }
        }
    }

    private void launchFolderPicker() {
        if (folderPickerInFlight) {
            return;
        }
        folderPickerInFlight = true;
        clearStateMarkers();

        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_GAME_FOLDER);
        } catch (ActivityNotFoundException e) {
            folderPickerInFlight = false;
            showFatalSetupMessage(
                    "لا يوجد مدير ملفات في النظام يستطيع اختيار مجلد اللعبة.\n\n"
                            + "ثبّت مدير ملفات يدعم اختيار مجلدات ثم جرّب مرة ثانية.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == REQUEST_GAME_FOLDER) {
            folderPickerInFlight = false;

            if (resultCode != RESULT_OK || data == null || data.getData() == null) {
                markCancelled();
                Toast.makeText(this, "لم يتم اختيار مجلد اللعبة.", Toast.LENGTH_LONG).show();
                return;
            }

            Uri treeUri = data.getData();
            try {
                final int takeFlags = data.getFlags()
                        & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                getContentResolver().takePersistableUriPermission(treeUri, takeFlags);
            } catch (SecurityException ignored) {
                // All Files Access is the real filesystem permission used by the native engine.
            }

            String path = resolveTreeUriToPath(treeUri);
            if (path == null) {
                showUnsupportedLocationMessage();
                return;
            }

            File folder = new File(path);
            FolderStatus status = inspectGameFolder(folder);
            showFolderStatus(folder, status);
        } else if (requestCode == REQUEST_ALL_FILES_ACCESS) {
            if (hasAllFilesAccess()) {
                waitingForAllFilesAccess = false;
                launchFolderPicker();
            } else {
                waitingForAllFilesAccess = true;
                showAllFilesAccessDialog();
            }
        }
    }

    private String resolveTreeUriToPath(Uri treeUri) {
        if (!DocumentsContract.isTreeUri(treeUri)) {
            return null;
        }

        final String authority = treeUri.getAuthority();
        if (!"com.android.externalstorage.documents".equals(authority)) {
            return null;
        }

        String documentId;
        try {
            documentId = DocumentsContract.getTreeDocumentId(treeUri);
        } catch (IllegalArgumentException e) {
            return null;
        }

        int separator = documentId.indexOf(':');
        if (separator <= 0) {
            return null;
        }

        String volume = documentId.substring(0, separator);
        String relative = documentId.substring(separator + 1);

        File root;
        if ("primary".equalsIgnoreCase(volume)) {
            root = Environment.getExternalStorageDirectory();
        } else {
            root = new File("/storage", volume);
        }

        File resolved = relative.isEmpty() ? root : new File(root, relative);
        try {
            return resolved.getCanonicalPath();
        } catch (Exception e) {
            return resolved.getAbsolutePath();
        }
    }

    private FolderStatus inspectGameFolder(File folder) {
        FolderStatus status = new FolderStatus();

        if (folder == null || !folder.isDirectory() || !folder.canRead()) {
            status.folderReadable = false;
            status.details = "المجلد غير قابل للقراءة.";
            return status;
        }

        status.folderReadable = true;
        status.inizh = findDirectFileIgnoreCase(folder, "INIZH.big");

        File texturesHere = findDirectFileIgnoreCase(folder, "Textures.big");
        if (texturesHere != null) {
            status.textures = texturesHere;
        } else {
            File zhGenerals = findDirectDirectoryIgnoreCase(folder, "ZH_Generals");
            if (zhGenerals != null) {
                status.textures = findDirectFileIgnoreCase(zhGenerals, "Textures.big");
            }
        }

        if (status.textures == null) {
            File parent = folder.getParentFile();
            if (parent != null) {
                File siblingGenerals = findDirectDirectoryIgnoreCase(parent, "Command & Conquer Generals");
                if (siblingGenerals == null) {
                    siblingGenerals = findDirectDirectoryIgnoreCase(parent, "Command & Conquer(tm) Generals");
                }
                if (siblingGenerals != null) {
                    status.textures = findDirectFileIgnoreCase(siblingGenerals, "Textures.big");
                }
            }
        }

        return status;
    }

    private File findDirectFileIgnoreCase(File directory, String name) {
        if (directory == null || !directory.isDirectory()) {
            return null;
        }

        File[] files = directory.listFiles();
        if (files == null) {
            return null;
        }

        for (File file : files) {
            if (file.isFile() && file.getName().equalsIgnoreCase(name)) {
                return file;
            }
        }
        return null;
    }

    private File findDirectDirectoryIgnoreCase(File directory, String name) {
        if (directory == null || !directory.isDirectory()) {
            return null;
        }

        File[] files = directory.listFiles();
        if (files == null) {
            return null;
        }

        for (File file : files) {
            if (file.isDirectory() && file.getName().equalsIgnoreCase(name)) {
                return file;
            }
        }
        return null;
    }

    private void showFolderStatus(File folder, FolderStatus status) {
        StringBuilder message = new StringBuilder();
        message.append("المجلد المختار:\n").append(folder.getAbsolutePath()).append("\n\n");

        if (status.folderReadable) {
            if (status.inizh != null) {
                message.append("✅ INIZH.big — موجود\n");
            } else {
                message.append("❌ INIZH.big — مفقود\n");
            }

            if (status.textures != null) {
                message.append("✅ Textures.big — موجود\n");
                message.append("   ").append(status.textures.getAbsolutePath()).append("\n");
            } else {
                message.append("❌ Textures.big — مفقود\n");
                message.append("   يجب أن يكون داخل المجلد أو ZH_Generals أو مجلد Generals المجاور.\n");
            }
        } else {
            message.append("❌ لا يمكن قراءة المجلد.\n");
            message.append(status.details).append("\n");
        }

        if (status.isComplete()) {
            message.append("\n✅ ملفات اللعبة الأساسية كاملة.\n");
            message.append("يمكن تشغيل Zero Hour الآن بدون نسخ الملفات إلى Android/data.");
        } else {
            message.append("\n⚠️ المجلد غير مكتمل. أصلح الملفات الناقصة ثم اختر المجلد مرة ثانية.");
        }

        AlertDialog.Builder builder = new AlertDialog.Builder(this)
                .setTitle(status.isComplete() ? "تم العثور على ملفات اللعبة" : "ملفات اللعبة ناقصة")
                .setMessage(message.toString())
                .setCancelable(false);

        if (status.isComplete()) {
            builder.setPositiveButton("بدء اللعبة", (dialog, which) -> writeReadyRoot(folder));
            builder.setNegativeButton("اختيار مجلد آخر", (dialog, which) -> launchFolderPicker());
        } else {
            builder.setPositiveButton("اختيار مجلد آخر", (dialog, which) -> launchFolderPicker());
            builder.setNegativeButton("إغلاق", (dialog, which) -> markCancelled());
        }

        builder.show();
    }

    private String readReadyRoot() {
        File marker = readyMarker();
        if (marker == null || !marker.isFile()) {
            return null;
        }

        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(new FileInputStream(marker), StandardCharsets.UTF_8))) {
            String root = reader.readLine();
            if (root == null) {
                return null;
            }
            root = root.trim();
            if (root.isEmpty()) {
                return null;
            }
            return new File(root).getCanonicalPath();
        } catch (Exception e) {
            return null;
        }
    }

    private void writeReadyRoot(File folder) {
        File marker = readyMarker();
        File cancelled = cancelledMarker();
        if (marker == null || folder == null) {
            markCancelled();
            return;
        }

        clearStateMarkers();

        File temp = new File(marker.getParentFile(), ".zh-game-root.new");
        try (FileOutputStream output = new FileOutputStream(temp, false)) {
            byte[] data = (folder.getCanonicalPath() + "\n").getBytes(StandardCharsets.UTF_8);
            output.write(data);
            output.flush();
            output.getFD().sync();

            if (!temp.renameTo(marker)) {
                throw new IllegalStateException("Unable to commit game-root marker");
            }
        } catch (Exception e) {
            temp.delete();
            Toast.makeText(this, "تعذر حفظ مجلد اللعبة. أعد الاختيار مرة أخرى.", Toast.LENGTH_LONG).show();
            markCancelled();
            return;
        }

        Toast.makeText(this, "✅ تم حفظ مجلد اللعبة بنجاح.", Toast.LENGTH_SHORT).show();
    }

    private void markCancelled() {
        File marker = readyMarker();
        File cancelled = cancelledMarker();
        if (marker != null) marker.delete();
        if (cancelled == null) {
            return;
        }

        try (FileOutputStream output = new FileOutputStream(cancelled, false)) {
            output.write("cancelled\n".getBytes(StandardCharsets.UTF_8));
            output.flush();
        } catch (Exception ignored) {
        }
    }

    private void showUnsupportedLocationMessage() {
        new AlertDialog.Builder(this)
                .setTitle("المجلد غير مدعوم")
                .setMessage(
                        "اختر مجلد اللعبة من الذاكرة الداخلية المشتركة أو من وحدة تخزين ظاهرة للنظام.\n\n"
                                + "هذا الإصدار يحتاج مسارًا حقيقيًا على نظام الملفات حتى يقرأ محرك اللعبة ملفات BIG مباشرة.")
                .setPositiveButton("اختيار مجلد آخر", (dialog, which) -> launchFolderPicker())
                .setNegativeButton("إغلاق", (dialog, which) -> markCancelled())
                .show();
    }

    private void showFatalSetupMessage(String message) {
        new AlertDialog.Builder(this)
                .setTitle("إعداد ملفات اللعبة")
                .setMessage(message)
                .setPositiveButton("اختيار المجلد", (dialog, which) -> {
                    if (!hasAllFilesAccess()) {
                        waitingForAllFilesAccess = true;
                        showAllFilesAccessDialog();
                    } else {
                        launchFolderPicker();
                    }
                })
                .setNegativeButton("إغلاق", (dialog, which) -> markCancelled())
                .show();
    }

    private static final class FolderStatus {
        boolean folderReadable;
        File inizh;
        File textures;
        String details = "";

        boolean isComplete() {
            return folderReadable && inizh != null && textures != null;
        }
    }
}
