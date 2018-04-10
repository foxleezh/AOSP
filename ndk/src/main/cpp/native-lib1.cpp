#include <jni.h>
#include <string>

extern "C" {

JNIEXPORT jstring JNICALL Java_com_foxleezh_ndk_cpp_ChartNative1_getString
        (JNIEnv *env, jclass){
    return env->NewStringUTF("跳转RN");
}

}
