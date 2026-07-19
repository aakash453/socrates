pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "SocratesEngine"

include(":socrates")
project(":socrates").projectDir = File("platform/android")

include(":app")
project(":app").projectDir = File("platform/android/app")
