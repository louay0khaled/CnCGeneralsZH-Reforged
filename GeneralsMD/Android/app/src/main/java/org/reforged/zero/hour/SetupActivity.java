package org.reforged.zero.hour;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.text.TextUtils;
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
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

public final class SetupActivity extends Activity {
    private static final int REQUEST_FOLDER = 4101;
    // Bump this whenever the Reforged-owned Data tree changes. The data is kept in
    // private app storage and refreshed atomically before native startup.
    private static final String REFORGED_DATA_VERSION = "2026-10-07-2";
    private static final String NATIVE_DIAGNOSTICS_SEEN = ".native-diagnostics-seen";
    private static final String GAME_RUN_PENDING = ".game-run-pending";
    private boolean pickerOpen = false;
    private boolean permissionScreenOpen = false;

    // Reforged launcher shell: the native game is started only after the install
    // has been validated; this screen itself never touches the renderer.
    private FrameLayout launcherContent;
    private LinearLayout bottomNav;
    private TextView zeroHourPathView;
    private TextView fileStatusView;
    private TextView classicPathView;
    private TextView primaryLaunchButton;
    private int selectedTab = 0;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.rgb(15, 18, 20));
        getWindow().setNavigationBarColor(Color.rgb(15, 18, 20));
        buildLauncherUi();
        new android.os.Handler(getMainLooper()).postDelayed(this::prepare, 150);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (permissionScreenOpen && hasDirectFileAccess()) {
            permissionScreenOpen = false;
            openPicker();
            return;
        }

        // The game runs in :game. When that process dies, this launcher process resumes.
        // Check the shared app-specific diagnostics directory automatically.
        new android.os.Handler(getMainLooper()).postDelayed(
                () -> showNativeDiagnosticsIfPresent(true), 120);
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

    private File reforgedDataDir() {
        return new File(getFilesDir(), "ReforgedData");
    }

    private File reforgedDataVersionFile() {
        return new File(getFilesDir(), ".reforged-data-version");
    }

    private boolean reforgedAssetExists(String assetPath) {
        try (InputStream in = getAssets().open(assetPath)) {
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    private boolean ensureReforgedData() {
        // These two files prove that the build actually embedded the Reforged asset tree.
        // A missing asset must never degrade into an empty ReforgedData directory and a
        // later opaque INI crash.
        if (!reforgedAssetExists("Data/INI/ScienceReforged.ini") ||
                !reforgedAssetExists("Data/Patch.str")) {
            return false;
        }

        File destination = reforgedDataDir();
        String installed = readMarker(reforgedDataVersionFile());
        if (REFORGED_DATA_VERSION.equals(installed) && destination.isDirectory()) {
            return true;
        }

        File temporary = new File(getFilesDir(), "ReforgedData.new");
        deleteTree(temporary);
        try {
            if (!temporary.mkdirs() && !temporary.isDirectory()) {
                throw new IOException("could not create temporary Reforged data directory");
            }
            copyAssetTree("", temporary);

            File temporaryVersion = new File(getFilesDir(), ".reforged-data-version.new");
            try (FileOutputStream out = new FileOutputStream(temporaryVersion, false)) {
                out.write((REFORGED_DATA_VERSION + "\\n").getBytes(StandardCharsets.UTF_8));
                out.flush();
                out.getFD().sync();
            }

            deleteTree(destination);
            if (!temporary.renameTo(destination)) {
                throw new IOException("could not activate Reforged data directory");
            }

            File version = reforgedDataVersionFile();
            if (version.exists() && !version.delete()) {
                throw new IOException("could not replace Reforged data version");
            }
            if (!temporaryVersion.renameTo(version)) {
                throw new IOException("could not activate Reforged data version");
            }
            return true;
        } catch (Exception e) {
            deleteTree(temporary);
            return false;
        }
    }

    private void copyAssetTree(String assetPath, File destination) throws IOException {
        String[] children = getAssets().list(assetPath);
        if (children != null && children.length > 0) {
            if (!destination.exists() && !destination.mkdirs()) {
                throw new IOException("could not create " + destination);
            }
            for (String child : children) {
                String childAsset = assetPath.isEmpty() ? child : assetPath + "/" + child;
                copyAssetTree(childAsset, new File(destination, child));
            }
            return;
        }

        File parent = destination.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("could not create " + parent);
        }
        try (InputStream in = getAssets().open(assetPath);
             FileOutputStream out = new FileOutputStream(destination, false)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = in.read(buffer)) != -1) {
                out.write(buffer, 0, count);
            }
            out.flush();
            out.getFD().sync();
        }
    }

    private void deleteTree(File file) {
        if (file == null || !file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteTree(child);
        }
        file.delete();
    }


    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private GradientDrawable rounded(int color, float radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radiusDp));
        return d;
    }

    private TextView makeText(String value, float sizeSp, int color, boolean bold) {
        TextView v = new TextView(this);
        v.setText(value);
        v.setTextSize(sizeSp);
        v.setTextColor(color);
        v.setGravity(Gravity.CENTER_VERTICAL);
        v.setIncludeFontPadding(true);
        if (bold) v.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return v;
    }

    private TextView actionButton(String label, int fill, int textColor) {
        TextView b = makeText(label, 15f, textColor, true);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(16), dp(12), dp(16), dp(12));
        b.setBackground(rounded(fill, 28f));
        b.setClickable(true);
        b.setFocusable(true);
        return b;
    }

    private LinearLayout card(LinearLayout page) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(16), dp(16), dp(16), dp(16));
        c.setBackground(rounded(Color.rgb(27, 32, 34), 18f));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(16);
        page.addView(c, lp);
        return c;
    }

    private TextView addCardText(LinearLayout parent, String value, float size,
                                 int color, boolean bold, int topDp) {
        TextView v = makeText(value, size, color, bold);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(topDp);
        parent.addView(v, lp);
        return v;
    }

    private LinearLayout page() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setClipToPadding(false);
        scroll.setPadding(dp(16), dp(10), dp(16), dp(18));
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(page, new ScrollView.LayoutParams(-1, -2));
        launcherContent.addView(scroll, new FrameLayout.LayoutParams(-1, -1));
        return page;
    }

    private void buildLauncherUi() {
        final int bg = Color.rgb(15, 18, 20);
        final int primary = Color.rgb(160, 197, 92);
        final int primaryText = Color.rgb(20, 25, 20);
        final int textPrimary = Color.rgb(239, 242, 237);
        final int textSecondary = Color.rgb(171, 181, 176);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setLayoutDirection(View.LAYOUT_DIRECTION_RTL);
        root.setBackgroundColor(bg);

        LinearLayout top = new LinearLayout(this);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(dp(20), dp(14), dp(20), dp(12));
        top.setBackgroundColor(Color.rgb(20, 24, 26));

        LinearLayout titleBox = new LinearLayout(this);
        titleBox.setOrientation(LinearLayout.VERTICAL);
        top.addView(titleBox, new LinearLayout.LayoutParams(0, -2, 1f));

        TextView over = makeText("ZERO HOUR", 11f, primary, true);
        over.setLetterSpacing(0.12f);
        titleBox.addView(over);

        TextView title = makeText("Generals Zero Hour Reforged", 21f, textPrimary, true);
        titleBox.addView(title);

        TextView version = makeText("Android  •  Native GLES", 11f, textSecondary, false);
        top.addView(version, new LinearLayout.LayoutParams(-2, -2));
        root.addView(top, new LinearLayout.LayoutParams(-1, -2));

        launcherContent = new FrameLayout(this);
        root.addView(launcherContent, new LinearLayout.LayoutParams(-1, 0, 1f));

        bottomNav = new LinearLayout(this);
        bottomNav.setOrientation(LinearLayout.HORIZONTAL);
        bottomNav.setGravity(Gravity.CENTER);
        bottomNav.setPadding(dp(8), dp(6), dp(8), dp(6));
        bottomNav.setBackgroundColor(Color.rgb(22, 27, 29));

        String[] icons = {"⌂", "▣", "☰", "⚙", "?"};
        String[] labels = {"الرئيسية", "الرسوميات", "الواجهة", "الأدوات", "المساعدة"};
        for (int i = 0; i < labels.length; i++) {
            final int tab = i;
            LinearLayout item = new LinearLayout(this);
            item.setOrientation(LinearLayout.VERTICAL);
            item.setGravity(Gravity.CENTER);
            item.setPadding(dp(4), dp(4), dp(4), dp(4));
            item.setTag(tab);
            item.setClickable(true);
            item.setFocusable(true);

            TextView icon = makeText(icons[i], 21f, textSecondary, true);
            icon.setGravity(Gravity.CENTER);
            item.addView(icon, new LinearLayout.LayoutParams(-1, dp(27)));

            TextView label = makeText(labels[i], 10f, textSecondary, true);
            label.setGravity(Gravity.CENTER);
            label.setMaxLines(1);
            label.setEllipsize(TextUtils.TruncateAt.END);
            item.addView(label, new LinearLayout.LayoutParams(-1, dp(20)));

            item.setOnClickListener(v -> {
                selectedTab = tab;
                renderTab();
            });
            bottomNav.addView(item, new LinearLayout.LayoutParams(0, dp(70), 1f));
        }
        root.addView(bottomNav, new LinearLayout.LayoutParams(-1, dp(78)));

        setContentView(root);
        renderTab();
    }

    private void renderTab() {
        if (launcherContent == null) return;
        launcherContent.removeAllViews();

        if (bottomNav != null) {
            final int primary = Color.rgb(160, 197, 92);
            final int selectedBg = Color.rgb(48, 60, 45);
            final int secondary = Color.rgb(171, 181, 176);
            for (int i = 0; i < bottomNav.getChildCount(); i++) {
                LinearLayout item = (LinearLayout) bottomNav.getChildAt(i);
                boolean selected = i == selectedTab;
                item.setBackground(selected ? rounded(selectedBg, 18f) : null);
                if (item.getChildCount() >= 2) {
                    ((TextView) item.getChildAt(0)).setTextColor(selected ? primary : secondary);
                    ((TextView) item.getChildAt(1)).setTextColor(selected ? primary : secondary);
                }
            }
        }

        switch (selectedTab) {
            case 1: buildGraphicsTab(); break;
            case 2: buildInterfaceTab(); break;
            case 3: buildToolsTab(); break;
            case 4: buildHelpTab(); break;
            default: buildHomeTab(); break;
        }
    }

    private void sectionTitle(LinearLayout page, String title, String subtitle) {
        TextView t = makeText(title, 21f, Color.rgb(239, 242, 237), true);
        page.addView(t, new LinearLayout.LayoutParams(-1, -2));
        if (subtitle != null)
            addCardText(page, subtitle, 13f, Color.rgb(171, 181, 176), false, 2);
    }

    private void buildHomeTab() {
        LinearLayout p = page();
        sectionTitle(p, "جاهز للقتال؟", "شغّل Reforged بعد التأكد من ملفات Zero Hour الأصلية.");

        primaryLaunchButton = actionButton("▶   تشغيل اللعبة",
                Color.rgb(160, 197, 92), Color.rgb(20, 25, 20));
        primaryLaunchButton.setTextSize(19f);
        primaryLaunchButton.setOnClickListener(v -> {
            String root = savedRoot();
            Status status = root == null ? new Status() : inspect(new File(root));
            if (!status.complete()) {
                Toast.makeText(this, "اختر مجلد Zero Hour مكتملًا قبل التشغيل.",
                        Toast.LENGTH_LONG).show();
                return;
            }
            launchGame();
        });
        LinearLayout.LayoutParams launchLp = new LinearLayout.LayoutParams(-1, dp(64));
        launchLp.topMargin = dp(16);
        p.addView(primaryLaunchButton, launchLp);

        LinearLayout files = card(p);
        addCardText(files, "FILES & INSTALLATION", 11f,
                Color.rgb(160, 197, 92), true, 0);
        zeroHourPathView = addCardText(files, "مسار Zero Hour: —", 12f,
                Color.rgb(171, 181, 176), false, 8);
        zeroHourPathView.setMaxLines(2);
        zeroHourPathView.setEllipsize(TextUtils.TruncateAt.END);
        fileStatusView = addCardText(files, "الحالة: جارٍ التحقق…", 14f,
                Color.rgb(239, 242, 237), true, 8);
        classicPathView = addCardText(files, "مسار Generals الكلاسيكية: —", 12f,
                Color.rgb(171, 181, 176), false, 8);
        classicPathView.setMaxLines(2);
        classicPathView.setEllipsize(TextUtils.TruncateAt.END);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(-1, -2);
        blp.topMargin = dp(13);
        files.addView(buttons, blp);

        TextView choose = actionButton("اختيار مجلد اللعبة",
                Color.rgb(51, 60, 63), Color.rgb(239, 242, 237));
        choose.setTextSize(14f);
        choose.setOnClickListener(v -> {
            if (!hasDirectFileAccess()) showPermissionDialog();
            else openPicker();
        });
        buttons.addView(choose, new LinearLayout.LayoutParams(0, dp(50), 1f));

        TextView reset = actionButton("إعادة تعيين",
                Color.rgb(27, 32, 34), Color.rgb(220, 130, 118));
        reset.setTextSize(14f);
        reset.setOnClickListener(v -> resetGameFolders());
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(0, dp(50), 1f);
        rlp.setMarginStart(dp(8));
        buttons.addView(reset, rlp);

        LinearLayout reforged = card(p);
        addCardText(reforged, "REFORGED DATA", 11f,
                Color.rgb(160, 197, 92), true, 0);
        addCardText(reforged,
                "بيانات Reforged مضمّنة داخل التطبيق وتُجهّز تلقائيًا. " +
                "ملفات BIG الأصلية تبقى في مكانها ولا تُنسخ إلى Android/data.",
                13f, Color.rgb(171, 181, 176), false, 9);

        refreshLauncherStatus();
    }

    private void buildGraphicsTab() {
        LinearLayout p = page();
        sectionTitle(p, "الرسوميات", "مساحة إعدادات Native GLES الخاصة بالنسخة الجديدة.");
        LinearLayout c = card(p);
        addCardText(c, "محرك العرض", 17f, Color.rgb(239, 242, 237), true, 0);
        addCardText(c, "OpenGL ES 3.0  •  Native renderer", 14f,
                Color.rgb(160, 197, 92), true, 9);
        addCardText(c,
                "لاحقًا: الدقة، مقياس العرض، جودة المؤثرات، والحد الأقصى لمعدل الإطارات.",
                13f, Color.rgb(171, 181, 176), false, 8);
    }

    private void buildInterfaceTab() {
        LinearLayout p = page();
        sectionTitle(p, "الواجهة", "تخصيص واجهة المشغّل قبل الدخول إلى اللعبة.");
        LinearLayout c = card(p);
        addCardText(c, "اللغة", 17f, Color.rgb(239, 242, 237), true, 0);
        addCardText(c, "العربية (RTL)", 14f, Color.rgb(160, 197, 92), true, 9);
        addCardText(c,
                "لاحقًا: حجم النص، كثافة العناصر، ونمط الواجهة.",
                13f, Color.rgb(171, 181, 176), false, 8);
    }

    private void buildToolsTab() {
        LinearLayout p = page();
        sectionTitle(p, "الأدوات", "أدوات التشخيص وإدارة ملفات اللعبة.");
        TextView log = actionButton("عرض آخر سجل تشغيل",
                Color.rgb(51, 60, 63), Color.rgb(239, 242, 237));
        log.setOnClickListener(v -> {
            if (!showNativeDiagnosticsIfPresent())
                Toast.makeText(this, "لا يوجد سجل تشغيل جديد.", Toast.LENGTH_SHORT).show();
        });
        p.addView(log, new LinearLayout.LayoutParams(-1, dp(54)));

        TextView reset = actionButton("مسح مسارات اللعبة",
                Color.rgb(27, 32, 34), Color.rgb(220, 130, 118));
        reset.setOnClickListener(v -> resetGameFolders());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, dp(54));
        lp.topMargin = dp(10);
        p.addView(reset, lp);
    }

    private void buildHelpTab() {
        LinearLayout p = page();
        sectionTitle(p, "المساعدة", "تجهيز Zero Hour Reforged وتشغيله بأقل خطوات.");
        LinearLayout c = card(p);
        addCardText(c, "1", 18f, Color.rgb(160, 197, 92), true, 0);
        addCardText(c, "اختر مجلد تثبيت Zero Hour من زر «اختيار مجلد اللعبة».",
                14f, Color.rgb(239, 242, 237), false, 3);
        addCardText(c, "2", 18f, Color.rgb(160, 197, 92), true, 10);
        addCardText(c, "يجب أن تظهر INIZH.big وملفات Generals الأساسية كمتوفرة.",
                14f, Color.rgb(239, 242, 237), false, 3);
        addCardText(c, "3", 18f, Color.rgb(160, 197, 92), true, 10);
        addCardText(c, "بعد ظهور «تم العثور على الملفات» اضغط «تشغيل اللعبة».",
                14f, Color.rgb(239, 242, 237), false, 3);
        addCardText(c, "مهم: لا تحتاج لنسخ ملفات BIG يدويًا إلى Android/data.",
                13f, Color.rgb(171, 181, 176), false, 12);
    }

    private void refreshLauncherStatus() {
        if (zeroHourPathView == null) return;
        String root = savedRoot();
        String base = savedBaseRoot();
        if (root == null) {
            zeroHourPathView.setText("مسار Zero Hour: لم يتم اختيار مجلد اللعبة");
            fileStatusView.setText("الحالة: ⚠ لم يتم العثور على الملفات بعد");
            fileStatusView.setTextColor(Color.rgb(240, 184, 85));
            classicPathView.setText("مسار Generals الكلاسيكية: لم يتم تحديده");
            if (primaryLaunchButton != null) primaryLaunchButton.setAlpha(0.55f);
            return;
        }

        Status status = inspect(new File(root));
        String detectedBase = status.baseFolder == null ? base : status.baseFolder.getAbsolutePath();
        zeroHourPathView.setText("مسار Zero Hour:\n" + root);
        fileStatusView.setText(status.complete()
                ? "الحالة: ✅ تم العثور على الملفات"
                : "الحالة: ⚠ الملفات ناقصة — لا يمكن التشغيل");
        fileStatusView.setTextColor(status.complete()
                ? Color.rgb(160, 197, 92) : Color.rgb(240, 184, 85));
        classicPathView.setText("مسار Generals الكلاسيكية:\n" +
                (detectedBase == null ? "لم يتم العثور عليه" : detectedBase));
        if (primaryLaunchButton != null)
            primaryLaunchButton.setAlpha(status.complete() ? 1f : 0.55f);
    }

    private void resetGameFolders() {
        File root = rootMarker();
        File base = baseRootMarker();
        if (root != null) root.delete();
        if (base != null) base.delete();
        Toast.makeText(this, "تمت إعادة تعيين مسارات اللعبة.", Toast.LENGTH_SHORT).show();
        renderTab();
    }

    private void prepare() {
        if (!ensureReforgedData()) {
            showError("تعذر تجهيز ملفات Zero Hour Reforged المدمجة داخل التطبيق.");
            return;
        }

        String root = savedRoot();
        if (root != null && inspect(new File(root)).complete()) {
            refreshLauncherStatus();
            showNativeDiagnosticsIfPresent();
            return;
        }
        refreshLauncherStatus();
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
            refreshLauncherStatus();
        } catch (Exception e) {
            showError("تعذر حفظ مسارات ملفات اللعبة.");
        }
    }

    /*
     * Native and Java diagnostics share one normal app-specific external directory:
     * <external-files>/ZeroHourData/Logs
     *
     * The regular device path is:
     * /storage/emulated/0/Android/data/com.louay.generalszh/files/ZeroHourData/Logs
     *
     * No broad storage write permission is required for this app-owned directory.
     * Legacy locations are also checked so reports from older builds are not lost.
     */
    private File appExternalRoot() {
        return getExternalFilesDir(null);
    }

    private File nativeLogsDir() {
        File external = appExternalRoot();
        return external == null ? null : new File(external, "ZeroHourData/Logs");
    }

    private File legacyInternalLogsDir() {
        return new File(new File(getFilesDir(), "ZeroHourData"), "Logs");
    }

    private File legacyMalformedLogsDir() {
        File external = appExternalRoot();
        return external == null ? null : new File(external, "ZeroHourData\\Logs");
    }

    private File nativeCrashFile() {
        File dir = nativeLogsDir();
        return dir == null ? null : new File(dir, "ReleaseCrashInfo.txt");
    }

    private File nativeDebugFile() {
        File dir = nativeLogsDir();
        return dir == null ? null : new File(dir, "DebugLogFile.txt");
    }

    private File nativeStartupFile() {
        File dir = nativeLogsDir();
        return dir == null ? null : new File(dir, "NativeStartupLog.txt");
    }

    private File nativeDiagnosticsSeenFile() {
        File external = appExternalRoot();
        return external == null ? new File(getFilesDir(), NATIVE_DIAGNOSTICS_SEEN)
                : new File(external, NATIVE_DIAGNOSTICS_SEEN);
    }

    private File gameRunPendingFile() {
        File external = appExternalRoot();
        return external == null ? new File(getFilesDir(), GAME_RUN_PENDING)
                : new File(external, GAME_RUN_PENDING);
    }

    private void prepareGameRunDiagnostics() {
        File logs = nativeLogsDir();
        if (logs != null && !logs.isDirectory()) logs.mkdirs();

        // Keep native reports. The native side rotates current files during startup/crash.
        File javaCrash = logs == null ? null : new File(logs, "JavaCrashInfo.txt");
        if (javaCrash != null) javaCrash.delete();
        File javaStartup = logs == null ? null : new File(logs, "JavaStartupTrace.txt");
        if (javaStartup != null) javaStartup.delete();

        try {
            File pending = gameRunPendingFile();
            File parent = pending.getParentFile();
            if (parent != null && !parent.isDirectory()) parent.mkdirs();
            try (FileOutputStream out = new FileOutputStream(pending, false)) {
                out.write(Long.toString(System.currentTimeMillis()).getBytes(StandardCharsets.UTF_8));
                out.flush();
                out.getFD().sync();
            }
        } catch (Exception ignored) {}
    }

    private long readDiagnosticsSeen() {
        File file = nativeDiagnosticsSeenFile();
        if (!file.isFile()) return 0L;
        try {
            return Long.parseLong(new String(
                    java.nio.file.Files.readAllBytes(file.toPath()),
                    StandardCharsets.UTF_8).trim());
        } catch (Exception e) {
            return 0L;
        }
    }

    private void markDiagnosticsSeen(long timestamp) {
        try {
            File file = nativeDiagnosticsSeenFile();
            File parent = file.getParentFile();
            if (parent != null && !parent.isDirectory()) parent.mkdirs();
            try (FileOutputStream out = new FileOutputStream(file, false)) {
                out.write(Long.toString(timestamp).getBytes(StandardCharsets.UTF_8));
                out.flush();
                out.getFD().sync();
            }
        } catch (Exception ignored) {}
    }

    private String readTail(File file, int maxChars) {
        if (file == null || !file.isFile()) return "";
        try {
            String text = new String(
                    java.nio.file.Files.readAllBytes(file.toPath()),
                    StandardCharsets.UTF_8);
            if (text.length() <= maxChars) return text;
            return "…\n" + text.substring(text.length() - maxChars);
        } catch (Exception e) {
            return "تعذر قراءة السجل: " + e.getMessage();
        }
    }

    private long lastModified(File file) {
        return file != null && file.isFile() ? file.lastModified() : 0L;
    }

    /*
     * Find diagnostics in all current/legacy locations.
     * requireNew=true is used automatically when returning from :game.
     * requireNew=false is used by Tools and always shows the newest available report.
     */
    private boolean showNativeDiagnosticsIfPresent(boolean requireNew) {
        File currentLogs = nativeLogsDir();
        File legacyInternal = legacyInternalLogsDir();
        File legacyMalformed = legacyMalformedLogsDir();

        File crash = currentLogs == null ? null : new File(currentLogs, "ReleaseCrashInfo.txt");
        File crashPrev = currentLogs == null ? null : new File(currentLogs, "ReleaseCrashInfoPrev.txt");
        File debug = currentLogs == null ? null : new File(currentLogs, "DebugLogFile.txt");
        File debugPrev = currentLogs == null ? null : new File(currentLogs, "DebugLogFilePrev.txt");
        File startup = currentLogs == null ? null : new File(currentLogs, "NativeStartupLog.txt");
        File javaCrash = currentLogs == null ? null : new File(currentLogs, "JavaCrashInfo.txt");
        File javaStartup = currentLogs == null ? null : new File(currentLogs, "JavaStartupTrace.txt");

        File legacyCrash = new File(legacyInternal, "ReleaseCrashInfo.txt");
        File legacyDebug = new File(legacyInternal, "DebugLogFile.txt");
        File malformedDebug = legacyMalformed == null ? null : new File(legacyMalformed, "DebugLogFile.txt");

        // One older build placed the native crash report directly in ZeroHourData.
        File oldRootCrash = new File(new File(getFilesDir(), "ZeroHourData"), "ReleaseCrashInfo.txt");

        File newestCrash = lastModified(crash) >= lastModified(legacyCrash) ? crash : legacyCrash;
        if (lastModified(oldRootCrash) > lastModified(newestCrash)) newestCrash = oldRootCrash;
        File newestDebug = lastModified(debug) >= Math.max(lastModified(legacyDebug), lastModified(malformedDebug))
                ? debug
                : (lastModified(legacyDebug) >= lastModified(malformedDebug) ? legacyDebug : malformedDebug);

        long crashTime = Math.max(lastModified(newestCrash), lastModified(crashPrev));
        long debugTime = Math.max(lastModified(newestDebug), lastModified(debugPrev));
        long startupTime = lastModified(startup);
        long javaCrashTime = lastModified(javaCrash);
        long javaStartupTime = lastModified(javaStartup);
        long latest = Math.max(Math.max(crashTime, debugTime),
                Math.max(Math.max(startupTime, javaCrashTime), javaStartupTime));

        if (latest <= 0L) return false;

        if (requireNew) {
            File pendingFile = gameRunPendingFile();
            if (!pendingFile.isFile()) return false;

            long pending = 0L;
            try {
                pending = Long.parseLong(new String(
                        java.nio.file.Files.readAllBytes(pendingFile.toPath()),
                        StandardCharsets.UTF_8).trim());
            } catch (Exception ignored) {}

            if (pending <= 0L || latest <= pending || latest <= readDiagnosticsSeen()) return false;
        }

        StringBuilder body = new StringBuilder();
        body.append("مجلد السجلات:\n");
        if (currentLogs != null) body.append(currentLogs.getAbsolutePath()).append("\n");
        body.append("\n");

        if (startupTime > 0L) {
            body.append("===== NATIVE STARTUP TRACE =====\n");
            body.append(readTail(startup, 7000)).append("\n\n");
        }
        if (crashTime > 0L && newestCrash != null) {
            body.append("===== NATIVE CRASH REPORT =====\n");
            body.append(newestCrash.getAbsolutePath()).append("\n");
            body.append(readTail(newestCrash, 8500)).append("\n\n");
        }
        if (debugTime > 0L && newestDebug != null) {
            body.append("===== DEBUG LOG =====\n");
            body.append(newestDebug.getAbsolutePath()).append("\n");
            body.append(readTail(newestDebug, 5500)).append("\n\n");
        }
        if (javaCrashTime > 0L) {
            body.append("===== JAVA CRASH =====\n");
            body.append(javaCrash.getAbsolutePath()).append("\n");
            body.append(readTail(javaCrash, 6000)).append("\n\n");
        }
        if (javaStartupTime > 0L) {
            body.append("===== JAVA STARTUP TRACE =====\n");
            body.append(javaStartup.getAbsolutePath()).append("\n");
            body.append(readTail(javaStartup, 4000)).append("\n");
        }

        final String report = body.toString();
        markDiagnosticsSeen(latest);
        gameRunPendingFile().delete();

        new AlertDialog.Builder(this)
                .setTitle(requireNew ? "تشخيص انهيار اللعبة" : "آخر سجل تشغيل")
                .setMessage(report)
                .setNeutralButton("نسخ السجل", (d, w) -> {
                    android.content.ClipboardManager clipboard =
                            (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                    if (clipboard != null) {
                        clipboard.setPrimaryClip(
                                android.content.ClipData.newPlainText("Zero Hour diagnostics", report));
                        Toast.makeText(this, "تم نسخ السجل.", Toast.LENGTH_SHORT).show();
                    }
                })
                .setPositiveButton("تشغيل اللعبة", (d, w) -> launchGame())
                .setNegativeButton("إغلاق", null)
                .setCancelable(false)
                .show();
        return true;
    }

    private boolean showNativeDiagnosticsIfPresent() {
        return showNativeDiagnosticsIfPresent(false);
    }

    private void launchGame() {
        String root = savedRoot();
        if (root == null || root.isEmpty()) {
            Toast.makeText(this, "لم يتم اختيار مجلد Zero Hour.", Toast.LENGTH_LONG).show();
            return;
        }

        Status status = inspect(new File(root));
        if (!status.complete()) {
            Toast.makeText(this, "ملفات Zero Hour غير مكتملة. اختر مجلدًا صالحًا أولًا.", Toast.LENGTH_LONG).show();
            return;
        }

        File pending = gameRunPendingFile();
        try {
            File parent = pending.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs() && !parent.isDirectory()) {
                throw new IOException("could not create diagnostics directory");
            }
            try (FileOutputStream out = new FileOutputStream(pending, false)) {
                out.write(Long.toString(System.currentTimeMillis()).getBytes(StandardCharsets.UTF_8));
                out.flush();
                out.getFD().sync();
            }

            Intent intent = new Intent(this, GeneralsActivity.class);
            startActivity(intent);
        } catch (ActivityNotFoundException e) {
            pending.delete();
            Toast.makeText(this, "تعذر فتح واجهة اللعبة: " + e.getMessage(), Toast.LENGTH_LONG).show();
        } catch (IOException | SecurityException e) {
            pending.delete();
            Toast.makeText(this, "تعذر تجهيز تشغيل اللعبة: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
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
