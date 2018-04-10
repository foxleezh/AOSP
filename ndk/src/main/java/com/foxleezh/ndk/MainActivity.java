package com.foxleezh.ndk;


import android.os.Bundle;
import android.support.v7.app.AppCompatActivity;
import android.widget.Button;
import android.widget.TextView;

import com.foxleezh.ndk.cpp.NativeTest;


public class MainActivity extends AppCompatActivity {


    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // Example of a call to a native method
        TextView btn = findViewById(R.id.sample_text);
        String str = NativeTest.getString();
        btn.setText(str);
    }

}
