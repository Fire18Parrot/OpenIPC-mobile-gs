plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "org.openipc.gslib"
    compileSdk = 35
    ndkVersion = "27.0.12077973"

    defaultConfig {
        minSdk = 24

        externalNativeBuild {
            cmake {
                // C++20 is required by devourer (it uses std::span).
                cppFlags += listOf("-std=c++20", "-fno-omit-frame-pointer")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }

        // The Realtek adapters devourer drives. armeabi-v7a is kept for older
        // headsets; everything current is arm64.
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        consumerProguardFiles("consumer-rules.pro")
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
}
