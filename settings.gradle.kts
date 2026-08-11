pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
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

rootProject.name = "OpenIPC-mobile-gs"

// :gslib holds the native ground station - devourer, wfb-ng and the portable
// engine under core/ - behind a small Kotlin API.
include(":gslib")

// :app is the user-facing Compose application.
include(":app")
