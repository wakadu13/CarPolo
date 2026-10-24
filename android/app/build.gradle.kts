plugins {
    id("com.android.application")
}

// versionCode = number of git commits, so each new build installs over the
// previous one (CI checks out the full history for this). Falls back to 1.
val buildNumber: Int = runCatching {
    providers.exec {
        commandLine("git", "rev-list", "--count", "HEAD")
    }.standardOutput.asText.get().trim().toInt()
}.getOrDefault(1).coerceAtLeast(1)

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(17)
    }
}

android {
    namespace = "fr.carpolo.game"
    compileSdk = 36

    defaultConfig {
        applicationId = "fr.carpolo.game"
        minSdk = 26
        targetSdk = 36
        versionCode = buildNumber
        versionName = "1.0.$buildNumber"
    }

    // Shared, non-secret development key: every APK (CI or local) carries the
    // same signature, so a new version always installs over the old one.
    signingConfigs {
        create("dev") {
            storeFile = file("carpolo-dev.keystore")
            storePassword = "carpolo"
            keyAlias = "carpolo"
            keyPassword = "carpolo"
        }
    }

    buildTypes {
        getByName("debug") {
            signingConfig = signingConfigs.getByName("dev")
        }
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("dev")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    // The game itself lives in ../www (plain HTML/JS). It is packaged as-is,
    // with index.html at the root of the APK assets.
    sourceSets {
        getByName("main") {
            assets.srcDirs(rootProject.file("../www"))
        }
    }

    lint {
        // A single WebView activity: don't let lint block the release APK.
        checkReleaseBuilds = false
        abortOnError = false
    }
}
