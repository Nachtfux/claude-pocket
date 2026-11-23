import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "ai.pocket.heyclaude"
    compileSdk = 34

    defaultConfig {
        applicationId = "ai.pocket.heyclaude"
        minSdk = 26
        // LineageOS 18.1 on the Echo Spot is Android 11 (API 30).
        targetSdk = 30
        versionCode = 1
        versionName = "0.1.0"
        // The Echo Spot (MT8163) runs ARM; dropping x86/x86_64 ONNX Runtime
        // libraries cuts the APK from ~80 MB to ~35 MB.
        ndk {
            abiFilters += listOf("armeabi-v7a", "arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            // Sideload-only app: sign release with the debug key so
            // `assembleRelease` yields an installable APK without a keystore.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    androidResources {
        noCompress += "onnx"
    }

    lint {
        // targetSdk 30 is intentional (device runs Android 11); don't fail on
        // Play-Store-oriented checks such as ExpiredTargetSdkVersion.
        abortOnError = false
        checkReleaseBuilds = false
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation("com.microsoft.onnxruntime:onnxruntime-android:1.20.0")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    testImplementation("junit:junit:4.13.2")
}

// ---------------------------------------------------------------------------
// openWakeWord models (CC BY-NC-SA 4.0) are not committed. They are fetched
// into src/main/assets/models/ before every build; files that already exist
// are skipped. A user-trained hey_claude.onnx can be dropped next to them (or
// pushed to the app's external files dir at runtime) — it is never downloaded.
// ---------------------------------------------------------------------------
val wakeWordModelBaseUrl = "https://github.com/dscripka/openWakeWord/releases/download/v0.5.1"
val wakeWordModels = listOf(
    "melspectrogram.onnx",
    "embedding_model.onnx",
    "hey_jarvis_v0.1.onnx",
)
val wakeWordModelDir: File = file("src/main/assets/models")

fun downloadWithCurl(url: String, target: File): Boolean {
    return try {
        val proc = ProcessBuilder(
            "curl", "-fsSL", "--retry", "3", "--connect-timeout", "20",
            "-o", target.absolutePath, url,
        ).inheritIO().start()
        proc.waitFor() == 0 && target.isFile && target.length() > 0
    } catch (e: Exception) {
        false
    }
}

fun downloadWithJava(url: String, target: File): Boolean {
    return try {
        val conn = uri(url).toURL().openConnection() as java.net.HttpURLConnection
        conn.instanceFollowRedirects = true
        conn.connectTimeout = 20_000
        conn.readTimeout = 60_000
        conn.inputStream.use { input -> target.outputStream().use { input.copyTo(it) } }
        target.isFile && target.length() > 0
    } catch (e: Exception) {
        logger.warn("Java download of $url failed: ${e.message}")
        false
    }
}

val downloadWakeWordModels by tasks.registering {
    group = "hey claude"
    description = "Downloads the openWakeWord ONNX models into src/main/assets/models/."
    doLast {
        wakeWordModelDir.mkdirs()
        for (name in wakeWordModels) {
            val target = File(wakeWordModelDir, name)
            if (target.isFile && target.length() > 0) continue
            val url = "$wakeWordModelBaseUrl/$name"
            val tmp = File(wakeWordModelDir, "$name.part")
            tmp.delete()
            logger.lifecycle("Downloading $url")
            // curl first: it honours HTTPS_PROXY and the system trust store
            // (macOS ships it). Plain JVM download is the fallback.
            val ok = downloadWithCurl(url, tmp) || downloadWithJava(url, tmp)
            if (!ok || tmp.length() < 1024) {
                tmp.delete()
                throw GradleException(
                    "Could not download $url — download it manually into ${wakeWordModelDir.path}",
                )
            }
            if (!tmp.renameTo(target)) {
                throw GradleException("Could not move ${tmp.path} to ${target.path}")
            }
        }
    }
}

tasks.named("preBuild") {
    dependsOn(downloadWakeWordModels)
}
