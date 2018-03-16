#include <jni.h>
#include <string>

extern "C" {

JNIEXPORT jstring
JNICALL
Java_com_foxleezh_aosp_MainActivity_getString(JNIEnv *env, jobject) {
    return env ->NewStringUTF("aosp");
}

}
