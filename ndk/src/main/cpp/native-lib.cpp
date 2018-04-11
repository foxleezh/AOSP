#include <jni.h>
#include <string>
#include<android/log.h>
#include <stdlib.h>

#define TAG  "jni-test"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG,TAG,__VA_ARGS__);
extern "C"
{

JNIEXPORT jstring

JNICALL
Java_com_foxleezh_ndk_cpp_NativeTest_getString
        (JNIEnv *env, jclass) {
    std::string hello = "Hello from C++";
    LOGD("text is %s", hello.c_str());
    return env->NewStringUTF(hello.c_str());
}
}

