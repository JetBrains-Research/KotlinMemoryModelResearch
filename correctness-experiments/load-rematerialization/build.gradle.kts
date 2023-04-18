plugins {
    kotlin("multiplatform") version "1.7.20"
}

group = "me.user"
version = "1.0-SNAPSHOT"

repositories {
    mavenCentral()
}

kotlin {
    listOf(macosX64(), macosArm64(), mingwX64(), linuxX64()).forEach {
        it.binaries {
            executable {
                entryPoint = "main"
            }
        }
    }

    sourceSets {
        val nativeMain by creating
        val nativeTest by creating

        val macosX64Main by getting {
            dependsOn(nativeMain)
        }
        val macosArm64Main by getting {
            dependsOn(nativeMain)
        }
        val linuxX64Main by getting {
            dependsOn(nativeMain)
        }
        val mingwX64Main by getting {
            dependsOn(nativeMain)
        }

        val macosX64Test by getting {
            dependsOn(nativeTest)
        }
        val macosArm64Test by getting {
            dependsOn(nativeTest)
        }
        val linuxX64Test by getting {
            dependsOn(nativeTest)
        }
        val mingwX64Test by getting {
            dependsOn(nativeTest)
        }
    }
}