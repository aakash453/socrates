plugins {
    id("com.android.library")
}

android {
    namespace = "com.multiverse.socrates"
    compileSdk = 35
    ndkVersion = "27.1.12297006"

    defaultConfig {
        minSdk = 29
        externalNativeBuild {
            cmake {
                arguments(
                    "-DANDROID_STL=c++_shared",
                    "-DSOCRATES_ENABLE_TESTS=OFF",
                    "-DSOCRATES_WARNINGS_AS_ERRORS=OFF",
                    "-DSOCRATES_ENABLE_QNN=OFF",
                    "-DCMAKE_PREFIX_PATH=${file("../../build/conan-android-arm64").absolutePath}"
                )
                abiFilters("arm64-v8a")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            externalNativeBuild {
                cmake {
                    val qnnRoot = findProperty("QNN_SDK_ROOT") as? String
                        ?: System.getenv("QNN_SDK_ROOT") ?: ""
                    arguments(
                        "-DSOCRATES_ENABLE_QNN=ON",
                        "-DSOCRATES_ENABLE_QNN_SDK_ROOT=${qnnRoot}"
                    )
                }
            }
        }
        debug {
            externalNativeBuild {
                cmake {
                    arguments("-DSOCRATES_ENABLE_QNN=OFF")
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

