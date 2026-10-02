plugins {
    id("com.android.application")
    @@KOTLIN_PLUGIN@@
}

android {
    namespace = "@@APP_ID@@"
    compileSdk = @@COMPILE_SDK@@
    defaultConfig {
        applicationId = "@@APP_ID@@"
        minSdk = @@MIN_SDK@@
        targetSdk = @@TARGET_SDK@@
        versionCode = 1
        versionName = "@@VERSION@@"
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    @@KOTLIN_OPTIONS@@
    packaging { jniLibs { useLegacyPackaging = true } }
}

// Prebuilt Vyx/SDK libraries are produced by the real NDK toolchain before Gradle.
// An empty Java-only APK would not be a Zyn application.
val verifyZynNative by tasks.registering {
    doLast {
        val directories = file("src/main/jniLibs").listFiles()?.filter { it.isDirectory } ?: emptyList()
        check(directories.isNotEmpty()) { "No native libraries; run zyn publish --target android-arm64 (or android-x64) first" }
        for (directory in directories) {
            check(directory.name in listOf("arm64-v8a", "x86_64")) { "Unsupported ABI: ${directory.name}" }
            for (library in listOf("lib@@NATIVE@@.so", "libZyn.so", "libSDL3.so", "libCacao.so", "libslang-compiler.so", "libfreetype.so", "libharfbuzz.so", "libzynicuuc.so", "libzynicui18n.so", "libzynicudata.so", "libc++_shared.so")) {
                check(File(directory, library).isFile) { "Missing ${directory.name}/$library" }
            }
        }
        for (asset in listOf("Zyn.runtime.toml", "nut/solid.slang")) {
            check(file("src/main/assets/$asset").isFile) { "Missing Zyn asset $asset" }
        }
    }
}
tasks.named("preBuild").configure { dependsOn(verifyZynNative) }
