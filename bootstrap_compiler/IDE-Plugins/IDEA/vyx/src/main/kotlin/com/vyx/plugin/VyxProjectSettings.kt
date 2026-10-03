package com.vyx.plugin

/**
 * Settings collected by the New Project wizard for a Vyx project.
 * Templates mirror the Rust plugin: Application + libraries (static/shared).
 */
data class VyxProjectSettings(
    var packageName: String = "hello",
    var kind: Kind = Kind.APPLICATION,
    var createGitignore: Boolean = true,
    var createReadme: Boolean = true,
    var vyxcPath: String = "",
    var createRunConfiguration: Boolean = true,
) {
    enum class Kind(
        val label: String,
        val tomlType: String,
        val entryFile: String,
        val isLibrary: Boolean,
    ) {
        APPLICATION(
            "Binary (application)",
            "executable",
            "src/main.vyx",
            false
        ),
        LIBRARY_STATIC(
            "Library (static)",
            "static",
            "src/lib.vyx",
            true
        ),
        LIBRARY_SHARED(
            "Library (shared)",
            "shared",
            "src/lib.vyx",
            true
        ),
    }
}