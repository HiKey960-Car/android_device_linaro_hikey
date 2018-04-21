package tk.rabidbeaver.carsettings;

import android.app.Activity;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.Switch;
import android.widget.ArrayAdapter;
import android.widget.CompoundButton;
import android.content.SharedPreferences;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.util.Log;

import java.util.HashSet;
import java.util.Set;

import java.io.DataInputStream;
import java.io.DataOutputStream;

public class CarSettings extends Activity {
    LocalSocket mSocket;
    DataInputStream is;
    DataOutputStream os;

    EditText inputbox;

    private boolean setContainsString(Set<String> set, String string){
        Object[] setarr = set.toArray();
        for (int i=0; i<set.size(); i++){
            if (((String)setarr[i]).contentEquals(string)) return true;
        }
        return false;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_configure_swi);

        SharedPreferences prefs = getSharedPreferences("Settings", MODE_PRIVATE);
        Set<String> selectedDevices = prefs.getStringSet("devices", new HashSet<String>());

        boolean autoconnect = prefs.getBoolean("autoconnect", false);
        boolean autohotspot = prefs.getBoolean("autohotspot", false);

        BluetoothAdapter mBluetoothAdapter = BluetoothAdapter.getDefaultAdapter();
        Set<BluetoothDevice> pairedDevices = mBluetoothAdapter.getBondedDevices();
        PairedDev[] pda = new PairedDev[pairedDevices.size()];
        boolean[] selected = new boolean[pairedDevices.size()];

        int i = 0;
        for (BluetoothDevice device : pairedDevices) {
            Log.d("BluetoothTethering",device.getName()+", "+device.getAddress());
            if (selectedDevices.size() > 0 && setContainsString(selectedDevices, device.getAddress())) selected[i] = true;
            pda[i] = new PairedDev(device);
            i++;
        }

        final MultiSpinner spinner = (MultiSpinner) findViewById(R.id.devspin);
        final ArrayAdapter spinnerArrayAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, pda);
        spinner.setAdapter(spinnerArrayAdapter, false, null);

        spinner.setSelected(selected);

        Switch panswitch = findViewById(R.id.autopan);
        panswitch.setChecked(autoconnect);
        panswitch.setOnCheckedChangeListener(new Switch.OnCheckedChangeListener(){
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                SharedPreferences.Editor editor = getSharedPreferences("Settings", MODE_PRIVATE).edit();
                editor.putBoolean("autoconnect", isChecked);
                Set<String> selectedItems = new HashSet<>();
                for (int i=0; i<spinnerArrayAdapter.getCount(); i++){
                    if (spinner.getSelected()[i]) selectedItems.add(((PairedDev)spinnerArrayAdapter.getItem(i)).getDev());
                }
                editor.putStringSet("devices", selectedItems);
                editor.apply();
            }
        });

        Switch hotspotswitch = findViewById(R.id.onhotspot);
        hotspotswitch.setChecked(autohotspot);
        hotspotswitch.setOnCheckedChangeListener(new Switch.OnCheckedChangeListener(){
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                SharedPreferences.Editor editor = getSharedPreferences("Settings", MODE_PRIVATE).edit();
                editor.putBoolean("autohotspot", isChecked);
                editor.apply();
            }
        });

        mSocket = new LocalSocket();

        try {
            mSocket.connect(new LocalSocketAddress("/data/vendor/swi", LocalSocketAddress.Namespace.FILESYSTEM));
            is = new DataInputStream(mSocket.getInputStream());
            os = new DataOutputStream(mSocket.getOutputStream());
        } catch (Exception e){
            e.printStackTrace();
            this.finish();
        }

        Button startbtn = findViewById(R.id.startbtn);
        startbtn.setOnClickListener(new Button.OnClickListener(){
            @Override
            public void onClick(View btn){
                try {
                    os.writeBytes("P");
                } catch (Exception e){}
            }
        });

        Button stopbtn = findViewById(R.id.endbtn);
        stopbtn.setOnClickListener(new Button.OnClickListener(){
            @Override
            public void onClick(View btn){
                try {
                    os.writeBytes("E");
                } catch (Exception e){}
            }
        });

        Button reconnect = findViewById(R.id.reconnect);
        reconnect.setOnClickListener(new Button.OnClickListener(){
            @Override
            public void onClick(View btn){
                mSocket = new LocalSocket();

                try {
                    mSocket.connect(new LocalSocketAddress("/data/vendor/swi", LocalSocketAddress.Namespace.FILESYSTEM));
                    is = new DataInputStream(mSocket.getInputStream());
                    os = new DataOutputStream(mSocket.getOutputStream());
                } catch (Exception e){
                    e.printStackTrace();
                }
            }
        });

        inputbox = findViewById(R.id.input);

        Button savebtn = findViewById(R.id.savebtn);
        savebtn.setOnClickListener(new Button.OnClickListener(){
            @Override
            public void onClick(View btn){
                int value = Integer.valueOf(inputbox.getText().toString());
                try {
                    os.writeByte(value);
                } catch (Exception e){}
            }
        });
    }

    private class PairedDev {
        String dev;
        String name;
        PairedDev(BluetoothDevice btd){
            name = btd.getName();
            dev = btd.getAddress();
        }
        public String toString(){return name;}
        String getDev(){return dev;}
    }
}
