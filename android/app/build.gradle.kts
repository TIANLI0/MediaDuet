import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

val keystoreProperties = Properties().apply {
    val file = rootProject.file("keystore.properties")
    if (file.isFile) file.inputStream().use { load(it) }
}
val protocolVectors = rootProject.file("../protocol/test-vectors/crypto-v1.json").absolutePath

android {
    namespace = "app.mediaduet"
    compileSdk = 37
    buildToolsVersion = "37.0.0"

    defaultConfig {
        applicationId = "app.mediaduet"
        minSdk = 29
        targetSdk = 37
        versionCode = 8
        versionName = "1.4.1"
        androidResources.localeFilters += listOf("en", "zh-rCN")
        // Ships for 64-bit ARM phones only. -Pmediaduet.emulatorAbi adds x86_64 for local emulator runs.
        ndk {
            abiFilters += listOf("arm64-v8a")
            if (project.hasProperty("mediaduet.emulatorAbi")) abiFilters += "x86_64"
        }
    }

    signingConfigs {
        if (keystoreProperties.getProperty("storeFile") != null) {
            create("release") {
                storeFile = rootProject.file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    testOptions {
        unitTests.all {
            it.systemProperty("mediaduet.vectors", protocolVectors)
        }
    }

    lint {
        abortOnError = true
        checkReleaseBuilds = false
        disable += listOf("GradleDependency", "NewerVersionAvailable", "AndroidGradlePluginVersion", "ObsoleteSdkInt")
        // i18n is a hard requirement: every string must be translated and nothing hard-coded.
        error += listOf("MissingTranslation", "HardcodedText", "ExtraTranslation", "MissingQuantity", "StringFormatMatches", "StringFormatInvalid")
    }

    // In-app language switching needs every language in the base APK of an app bundle.
    bundle {
        language {
            enableSplit = false
        }
    }

    packaging {
        resources {
            excludes += listOf("DebugProbesKt.bin", "META-INF/*.version", "kotlin-tooling-metadata.json")
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.ui.graphics)
    implementation(libs.compose.foundation)
    implementation(libs.compose.material3)
    implementation(libs.compose.ui.tooling.preview)
    debugImplementation(libs.compose.ui.tooling)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kyant.backdrop)
    implementation(libs.kyant.shapes)

    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
}
