package org.uninet.demo;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * The UniNet Android demo: the same program as examples/python/basic.py, with a
 * screen instead of a terminal. Join, see who is there, send and receive on
 * "chat.room".
 *
 * The UI is built in code rather than from a layout XML so the whole app is two
 * source files and needs no resource compilation. It is a demo, not a design.
 *
 * TWO WAYS TO CONNECT, and they are genuinely different:
 *
 *   Wi-Fi  ordinary discovery over the UDP beacon. Both devices must be on the
 *          same network, and this app must hold a MulticastLock or Android's
 *          Wi-Fi driver filters the beacon away and nothing is ever found.
 *
 *   USB    no multicast exists over a USB cable, so discovery uses a rendezvous
 *          endpoint instead. The device dials 127.0.0.1:31337, which `adb
 *          reverse` forwards to the workstation. See examples/android/README.md.
 */
public final class MainActivity extends Activity {

    // Must match scripts/test-usb-link.sh on the workstation side.
    private static final String GOSSIP   = "tcp://127.0.0.1:31337";
    private static final String ENDPOINT = "tcp://127.0.0.1:31338";

    private TextView status;
    private TextView log;
    private ScrollView logScroll;
    private EditText input;
    private Button usbButton;
    private Button wifiButton;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private WifiManager.MulticastLock multicastLock;
    private boolean joined = false;
    private int tick = 0;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setContentView(buildUi());
        append("UniNet " + UniNet.nativeVersion()
               + (UniNet.nativeHasLz4() ? "  (lz4)" : "  (NO lz4: messages from a"
                                                    + " desktop peer will be dropped)"));
        handler.post(pump);

        // Lets a script drive the app instead of a finger, which is what makes
        // an end-to-end USB test possible:
        //     adb shell am start -n org.uninet.demo/.MainActivity --es mode usb
        String mode = getIntent() != null ? getIntent().getStringExtra("mode") : null;
        if ("usb".equals(mode)) {
            join(GOSSIP, ENDPOINT);
        } else if ("wifi".equals(mode)) {
            join("", "");
        } else {
            append("Pick USB or Wi-Fi to join.");
        }
    }

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(12);
        root.setPadding(pad, pad, pad, pad);

        status = new TextView(this);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        status.setText("Not connected.");
        root.addView(status);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        usbButton = new Button(this);
        usbButton.setText("Connect over USB");
        usbButton.setOnClickListener(v -> join(GOSSIP, ENDPOINT));
        wifiButton = new Button(this);
        wifiButton.setText("Connect over Wi-Fi");
        wifiButton.setOnClickListener(v -> join("", ""));
        buttons.addView(usbButton, equalWidth());
        buttons.addView(wifiButton, equalWidth());
        root.addView(buttons);

        logScroll = new ScrollView(this);
        log = new TextView(this);
        log.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        log.setTypeface(android.graphics.Typeface.MONOSPACE);
        log.setTextIsSelectable(true);
        logScroll.addView(log);
        LinearLayout.LayoutParams grow = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        root.addView(logScroll, grow);

        LinearLayout send = new LinearLayout(this);
        send.setOrientation(LinearLayout.HORIZONTAL);
        send.setGravity(Gravity.CENTER_VERTICAL);
        input = new EditText(this);
        input.setHint("message");
        input.setInputType(InputType.TYPE_CLASS_TEXT);
        input.setSingleLine(true);
        Button sendButton = new Button(this);
        sendButton.setText("Send");
        sendButton.setOnClickListener(v -> sendTyped());
        send.addView(input, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        send.addView(sendButton);
        root.addView(send);
        return root;
    }

    private LinearLayout.LayoutParams equalWidth() {
        return new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                                               getResources().getDisplayMetrics());
    }

    private void join(String gossip, String endpoint) {
        if (joined) return;
        // Only for the beacon. In gossip mode every connection is TCP, so the
        // lock buys nothing, and taking it would just cost battery.
        if (gossip.isEmpty()) acquireMulticastLock();

        String error = UniNet.nativeJoin("Android Tablet", gossip, endpoint);
        if (!error.isEmpty()) {
            append("! could not join: " + error);
            status.setTextColor(Color.RED);
            status.setText("Failed: " + error);
            return;
        }
        joined = true;
        usbButton.setEnabled(false);
        wifiButton.setEnabled(false);
        append(gossip.isEmpty() ? "joined over Wi-Fi" : "joined over USB (" + gossip + ")");
        handler.postDelayed(heartbeat, 2000);
    }

    private void sendTyped() {
        String text = input.getText().toString().trim();
        if (text.isEmpty() || !joined) return;
        UniNet.nativePublish(text);
        input.setText("");
    }

    /** Mirrors basic.py's loop: one message every two seconds. */
    private final Runnable heartbeat = new Runnable() {
        @Override public void run() {
            if (!joined) return;
            UniNet.nativePublish("hello #" + (++tick));
            handler.postDelayed(this, 2000);
        }
    };

    /**
     * Drains the native queue on the UI thread. The network thread never calls
     * into the JVM; this is where its output becomes visible. Same idea as
     * Session.Update() in Unity.
     */
    private final Runnable pump = new Runnable() {
        @Override public void run() {
            for (String line : UniNet.nativeDrain()) append(line);
            if (joined) {
                int peers = UniNet.nativePeerCount();
                status.setTextColor(peers > 0 ? Color.rgb(0, 128, 0) : Color.rgb(180, 120, 0));
                status.setText(UniNet.nativeDescribe());
            }
            handler.postDelayed(this, 200);
        }
    };

    private void append(String line) {
        log.append(line + "\n");
        logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
        // Mirrored to logcat so a test on the workstation can read what the
        // device saw. Without this the only record is a TextView nothing can
        // reach, and "did the tablet receive it?" is unanswerable from a script.
        android.util.Log.i(TAG, line);
    }

    static final String TAG = "UniNetDemo";

    private void acquireMulticastLock() {
        if (multicastLock != null) return;
        WifiManager wifi = (WifiManager)
                getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi == null) return;
        // Without this Android's Wi-Fi driver filters multicast and subnet
        // broadcast in hardware, so the beacon never reaches the app: discovery
        // finds nothing at all and reports no error anywhere.
        multicastLock = wifi.createMulticastLock("uninet-demo");
        multicastLock.setReferenceCounted(true);
        multicastLock.acquire();
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        // Before the lock: leaving is a network operation.
        if (joined) UniNet.nativeLeave();
        joined = false;
        if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        multicastLock = null;
        super.onDestroy();
    }
}
