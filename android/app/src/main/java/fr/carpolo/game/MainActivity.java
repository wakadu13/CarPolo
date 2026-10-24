package fr.carpolo.game;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.graphics.Color;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

/**
 * Full-screen WebView that runs the HTML5 game packaged in the APK assets
 * (the repository's www/ folder, index.html at the asset root).
 *
 * JavaScript hooks the game can expose on window.CP (all optional):
 *   CP.onBack()       -> return 'exit' to close the app, anything else = handled
 *   CP.onAppPause()   -> app goes to background (pause game, mute audio)
 *   CP.onAppResume()  -> app comes back to foreground
 *
 * Written without lambdas so it also builds with the offline toolchain
 * (tools/build-apk-local.sh).
 */
public class MainActivity extends Activity {

    private static final String GAME_URL = "file:///android_asset/index.html";

    private WebView webView;
    private Object backCallback; // OnBackInvokedCallback on API 33+

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = window.getAttributes();
            lp.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            window.setAttributes(lp);
        }
        // Hardware volume keys control the game sound, even in menus.
        setVolumeControlStream(AudioManager.STREAM_MUSIC);

        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            WebView.setWebContentsDebuggingEnabled(true);
        }

        webView = new WebView(this);
        webView.setBackgroundColor(Color.BLACK);
        webView.setOverScrollMode(View.OVER_SCROLL_NEVER);
        webView.setVerticalScrollBarEnabled(false);
        webView.setHorizontalScrollBarEnabled(false);
        webView.setHapticFeedbackEnabled(false);

        WebSettings s = webView.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setAllowFileAccess(true);
        s.setSupportZoom(false);
        s.setBuiltInZoomControls(false);
        s.setDisplayZoomControls(false);
        s.setTextZoom(100);
        // Lets the game know it runs inside the app (no browser fullscreen request).
        s.setUserAgentString(s.getUserAgentString() + " CarPoloApp");

        webView.setWebChromeClient(new WebChromeClient());
        webView.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                Uri uri = request.getUrl();
                if ("file".equals(uri.getScheme())) {
                    return false; // game pages stay in the WebView
                }
                // Any external link opens in the browser (the app has no INTERNET permission).
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, uri));
                } catch (ActivityNotFoundException ignored) {
                    // nothing to open it with
                }
                return true;
            }
        });

        setContentView(webView, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        if (Build.VERSION.SDK_INT >= 33) {
            backCallback = Api33.registerBack(this);
        }

        enterImmersive();
        webView.loadUrl(GAME_URL);
    }

    /** Asks the game what to do with the back button; it returns 'exit' to quit. */
    void handleBack() {
        if (webView == null) {
            finish();
            return;
        }
        webView.evaluateJavascript("(window.CP&&CP.onBack)?CP.onBack():'exit'",
                new ValueCallback<String>() {
                    @Override
                    public void onReceiveValue(String value) {
                        if (value != null && value.contains("exit")) {
                            finish();
                        }
                    }
                });
    }

    @SuppressWarnings("deprecation")
    @Override
    public void onBackPressed() {
        // Only called below Android 13 (newer versions use the OnBackInvokedCallback).
        handleBack();
    }

    @Override
    protected void onPause() {
        if (webView != null) {
            webView.evaluateJavascript("window.CP&&CP.onAppPause&&CP.onAppPause()", null);
            webView.onPause();
        }
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (webView != null) {
            webView.onResume();
            webView.evaluateJavascript("window.CP&&CP.onAppResume&&CP.onAppResume()", null);
        }
        enterImmersive();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            enterImmersive();
        }
    }

    @Override
    protected void onDestroy() {
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null) {
            Api33.unregisterBack(this, backCallback);
            backCallback = null;
        }
        if (webView != null) {
            ViewGroup parent = (ViewGroup) webView.getParent();
            if (parent != null) {
                parent.removeView(webView);
            }
            webView.destroy();
            webView = null;
        }
        super.onDestroy();
    }

    /** Hides status and navigation bars; a swipe from the edge shows them briefly. */
    @SuppressWarnings("deprecation")
    private void enterImmersive() {
        if (Build.VERSION.SDK_INT >= 30) {
            Api30.hideSystemBars(getWindow());
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_FULLSCREEN);
        }
    }

    /** API 30+ calls, kept in their own class so older devices never load them. */
    private static final class Api30 {
        static void hideSystemBars(Window window) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
    }

    /** API 33+ back handling (predictive back is on by default when targeting 36). */
    private static final class Api33 {
        static Object registerBack(final MainActivity activity) {
            OnBackInvokedCallback callback = new OnBackInvokedCallback() {
                @Override
                public void onBackInvoked() {
                    activity.handleBack();
                }
            };
            activity.getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, callback);
            return callback;
        }

        static void unregisterBack(MainActivity activity, Object callback) {
            activity.getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(
                    (OnBackInvokedCallback) callback);
        }
    }
}
