plugins {
    alias(libs.plugins.android.application) apply false
    // AGP 9 compiles Kotlin itself ("built-in Kotlin"); declaring the Kotlin Gradle plugin here
    // (without applying it) pins the Kotlin compiler version used by AGP.
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.kotlin.compose) apply false
}
