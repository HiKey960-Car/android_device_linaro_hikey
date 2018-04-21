package tk.rabidbeaver.carsettings;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;
import android.net.ConnectivityManager;
import android.content.SharedPreferences;

public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        SharedPreferences prefs = context.getSharedPreferences("Settings", Context.MODE_PRIVATE);
        if (prefs.getBoolean("autohotspot", false)){
            ConnectivityManager mCm;
            mCm = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
            OnStartTetheringCallback mStartTetheringCallback = new OnStartTetheringCallback();
            mCm.startTethering(ConnectivityManager.TETHERING_WIFI, true, mStartTetheringCallback);
        }
    }

    private static final class OnStartTetheringCallback extends ConnectivityManager.OnStartTetheringCallback {
        @Override
        public void onTetheringStarted() {
            Log.d("BootReceiver", "Wifi tethering started");
        }

        @Override
        public void onTetheringFailed() {
            Log.e("BootReceiver", "Wifi tethering failed");
        }
    }
}
