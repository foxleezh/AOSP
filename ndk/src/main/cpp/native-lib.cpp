#include <jni.h>
#include <string>

extern "C"
JNIEXPORT jstring

JNICALL
Java_com_foxleezh_ndk_cpp_NativeTest_getString
        (JNIEnv *env, jclass){
    std::string hello = "Hello from C++";
    return env->NewStringUTF(hello.c_str());
}
