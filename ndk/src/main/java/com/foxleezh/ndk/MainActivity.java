package com.foxleezh.ndk;


import android.os.Bundle;
import android.support.v7.app.AppCompatActivity;
import android.widget.Button;
import com.foxleezh.ndk.cpp.ChartNative;
import com.foxleezh.ndk.cpp.ChartNative1;


public class MainActivity extends AppCompatActivity {


    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // Example of a call to a native method
        Button btn = findViewById(R.id.sample_text);
        String str = ChartNative.getString();
        String str1 = ChartNative1.getString();
        btn.setText(str);
    }

}
