package com.foxleezh.ndk.cpp;

/**
 * Created by foxleezh on 18-3-7.
 */

public class ChartNative {

    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("native-lib");
    }

    public static native String getString();
}
