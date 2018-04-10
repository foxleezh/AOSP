package com.foxleezh.ndk;

import android.app.Application;
import android.util.Log;

/**
 * Created by foxleezh on 18-4-10.
 */

public class BaseApplication extends Application{

    @Override
    public void onCreate() {
        super.onCreate();
        Log.d("124","14");
    }

    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("native-lib");
    }
}
