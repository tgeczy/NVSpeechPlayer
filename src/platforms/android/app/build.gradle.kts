import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

val sigProps = Properties()
val sigFile = rootProject.file("signing.properties")
if (sigFile.exists()) sigProps.load(sigFile.inputStream())

android {
    namespace = "com.tgspeechbox.tts"
    compileSdk = 35

    ndkVersion = "27.2.12479018"

    signingConfigs {
        create("release") {
            storeFile = file(sigProps.getProperty("STORE_FILE", ""))
            storePassword = sigProps.getProperty("STORE_PASSWORD", "")
            keyAlias = sigProps.getProperty("KEY_ALIAS", "")
            keyPassword = sigProps.getProperty("KEY_PASSWORD", "")
        }
    }

    defaultConfig {
        applicationId = "com.tgspeechbox.tts"
        minSdk = 26
        targetSdk = 35
        versionCode = 350
        versionName = "3.10-beta5"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-fvisibility=hidden")
                arguments += listOf(
                    "-DCMAKE_BUILD_TYPE=MinSizeRel",
                    "-DESPEAK_NG_DIR=${rootProject.projectDir}/../../../../espeak-ng",
                    "-DTGSB_DIR=${rootProject.projectDir}/../../.."
                )
            }
        }

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../jni/CMakeLists.txt")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
        }
    }

    // Instrument the installed release app (signature must match).
    testBuildType = "release"

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
    }

    sourceSets {
        getByName("main") {
            kotlin.srcDirs("src/main/kotlin")
        }
        getByName("androidTest") {
            kotlin.srcDirs("src/androidTest/kotlin")
        }
    }
}

// ── Build-time asset copy ────────────────────────────────────
// Single canonical data lives at repo root:
//   resources/espeak-ng-data/   (eSpeak dictionary + voice data)
//   packs/                      (language packs + phonemes.yaml)
// Copy into the build's asset tree so the APK is self-contained.
val repoRoot = rootProject.projectDir.resolve("../../..")
val generatedAssets = layout.buildDirectory.dir("generated/assets/main")

val copyEspeakData by tasks.registering(Copy::class) {
    from(repoRoot.resolve("resources/espeak-ng-data"))
    into(generatedAssets.map { it.dir("espeak-ng-data") })
}

val copyPacks by tasks.registering(Copy::class) {
    from(repoRoot.resolve("packs"))
    into(generatedAssets.map { it.dir("tgsb/packs") })
}

android.sourceSets.getByName("main") {
    assets.srcDir(generatedAssets)
}

tasks.named("preBuild") {
    dependsOn(copyEspeakData, copyPacks)
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")

    // Compose BOM pins all Compose library versions together
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)

    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")

    // Activity integration (setContent {})
    implementation("androidx.activity:activity-compose:1.9.3")

    // Navigation for bottom tabs
    implementation("androidx.navigation:navigation-compose:2.8.5")

    // ViewModel + Compose integration
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")

    debugImplementation("androidx.compose.ui:ui-tooling")

    // Android probe instrumentation test (issue #100 / #95 /s/->/z/ investigation).
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("junit:junit:4.13.2")
}
