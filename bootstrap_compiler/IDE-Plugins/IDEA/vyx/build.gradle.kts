import org.jetbrains.intellij.platform.gradle.tasks.PrepareSandboxTask

plugins {
    id("java")
    // IDEA 2026.2 ships Kotlin metadata 2.4.0; compiler must be >= 2.4
    id("org.jetbrains.kotlin.jvm") version "2.4.0"
    id("org.jetbrains.intellij.platform") version "2.18.1"
}

val bundledToolchainNames = listOf(
    "vyxc-lsp.exe",
    "vyxc-dap.exe",
    "vyx_rt.dll",
    "vyx_run_rt.dll",
)
val bootstrapOut = layout.projectDirectory.dir("../../../out")
val targetIdePath = providers.gradleProperty("vyxIdePath")
    .orElse("D:/Jetbrains/CLion")
    .get()

group = "com.vyx.plugin"
version = "1.0.7"

repositories {
    mavenLocal()
    mavenCentral()
    intellijPlatform {
        defaultRepositories()
    }
}

// Use local IDE to avoid downloading ~2GB ideaIU installer from JetBrains CDN.
// Path: D:\Jetbrains\IntelliJ IDEA (Toolbox-managed, product-info version 2026.2)
dependencies {
    testImplementation("junit:junit:4.13.2")
    intellijPlatform {
        local(targetIdePath)
        bundledModule("intellij.platform.dap")
        testFramework(org.jetbrains.intellij.platform.gradle.TestFrameworkType.Platform)
    }
}

intellijPlatform {
    pluginConfiguration {
        ideaVersion {
            // The native DAP module used by Vyx is public starting with 2026.2.
            sinceBuild = "262"
        }

        changeNotes = """
            use/import package-registry completion with prefix match.
            LSP format, code actions, references and rename. Reformat Code
            uses vyxc-lsp with a local indent engine fallback.
        """.trimIndent()
    }
}

tasks {
    withType<JavaCompile> {
        sourceCompatibility = "25"
        targetCompatibility = "25"
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_25)
    }
}

tasks.withType<PrepareSandboxTask>().configureEach {
    // `out/vyxc.exe` can be held open by the currently running IDE.  The
    // authoritative self-host result is `boot.exe`; package that exact binary
    // under the public `vyxc.exe` name so compiler/LSP/DAP are always one gen.
    from(bootstrapOut) {
        include("boot.exe")
        rename { "vyxc.exe" }
        into(pluginName.map { "$it/bin" })
    }
    from(bootstrapOut) {
        include(bundledToolchainNames)
        into(pluginName.map { "$it/bin" })
    }
}
