package tk.rabidbeaver.carsettings;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;
import android.bluetooth.BluetoothPan;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.content.SharedPreferences;
import java.util.Set;

public class BluetoothReceiver extends BroadcastReceiver {

    private boolean setContainsString(Set<String> set, String string){
        Object[] setarr = set.toArray();
        for (int i=0; i<set.size(); i++){
            if (((String)setarr[i]).contentEquals(string)) return true;
        }
        return false;
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        SharedPreferences prefs = context.getSharedPreferences("Settings", Context.MODE_PRIVATE);
        Set<String> selectedDevices = prefs.getStringSet("devices", null);
        BluetoothDevice device;

        if (prefs.getBoolean("autoconnect", false)
                && intent.getIntExtra("android.bluetooth.adapter.extra.CONNECTION_STATE", -1) == BluetoothAdapter.STATE_CONNECTED
                && !selectedDevices.isEmpty()
                && (device = intent.getParcelableExtra("android.bluetooth.device.extra.DEVICE")) != null
                && setContainsString(selectedDevices, device.getAddress())){

            Log.d("BluetoothReceiver","Bluetooth has CONNECTED");

            final BluetoothProfile.ServiceListener mBtProfileServiceListener =
                new android.bluetooth.BluetoothProfile.ServiceListener() {
                    public void onServiceConnected(int profile, BluetoothProfile proxy) {
                        if (!((BluetoothPan) proxy).connect(device)){
                            Log.e("BluetoothEventReceiver", "Unable to start connection");
                        }
                    }

                    public void onServiceDisconnected(int profile) {}
                };

            BluetoothAdapter mBluetoothAdapter = BluetoothAdapter.getDefaultAdapter();

            if (mBluetoothAdapter != null) {
                mBluetoothAdapter.getProfileProxy(context, mBtProfileServiceListener, BluetoothProfile.PAN);
            }
        }
    }
}
