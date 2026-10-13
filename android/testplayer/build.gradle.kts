plugins {
    alias(libs.plugins.android.application)
}

// TEST FIXTURE ONLY - not part of the MediaDuet product and never shipped.
// A minimal player exposing a real MediaSession so the MediaSessionManager path of the
// MediaDuet app can be verified on an emulator without third-party apps.
android {
    namespace = "app.mediaduet.testplayer"
    compileSdk = 37
    buildToolsVersion = "37.0.0"

    defaultConfig {
        applicationId = "app.mediaduet.testplayer"
        minSdk = 29
        targetSdk = 37
        versionCode = 1
        versionName = "test-fixture"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        disable += listOf("GradleDependency", "NewerVersionAvailable", "AndroidGradlePluginVersion")
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
}
