// Android: receives the https URL of the launch intent and of later intents.
//
// The Java side lives in Strybok_UPL.xml: GameActivity.onCreate and GameActivity.onNewIntent call
// nativeStrybokOnUrl(String). JNI binds the method by name, so the function below must keep its
// exact name; "com.epicgames.unreal.GameActivity" is the activity class of Unreal Engine 5.

#include "StrybokPlatformLinks.h"

#if PLATFORM_ANDROID

#include <jni.h>

extern "C" JNIEXPORT void JNICALL Java_com_epicgames_unreal_GameActivity_nativeStrybokOnUrl(JNIEnv* Env, jobject Thiz, jstring Url)
{
	if (Env == nullptr || Url == nullptr)
	{
		return;
	}
	// Runs on the Android UI thread, possibly before the engine exists: only buffer the URL.
	const char* Utf8 = Env->GetStringUTFChars(Url, nullptr);
	if (Utf8 != nullptr)
	{
		FStrybokPlatformLinks::Push(std::string(Utf8));
		Env->ReleaseStringUTFChars(Url, Utf8);
	}
}

#endif // PLATFORM_ANDROID
