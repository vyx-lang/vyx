package com.vyx.plugin

import com.intellij.ide.plugins.PluginManagerCore
import com.intellij.openapi.extensions.PluginId
import com.intellij.openapi.project.Project
import java.io.File

/** Deterministic Vyx SDK/tool discovery shared by LSP, DAP, build and run. */
object VyxToolPaths {
    private const val PLUGIN_ID = "com.vyx.plugin"

    fun isWindows(): Boolean =
        System.getProperty("os.name").lowercase().contains("win")

    fun bare(baseName: String): String =
        if (isWindows()) "$baseName.exe" else baseName

    fun pluginInstallDir(): File? {
        val plugin = PluginManagerCore.getPlugin(PluginId.getId(PLUGIN_ID)) ?: return null
        val path = plugin.pluginPath.toFile()
        return if (path.isDirectory) path else path.parentFile
    }

    private fun existing(file: File?): String? =
        file?.takeIf { it.isFile }?.absoluteFile?.normalize()?.path

    private fun configured(raw: String?): String? {
        val value = raw?.trim().orEmpty()
        if (value.isEmpty()) return null
        existing(File(value))?.let { return it }
        val looksLikePath = File(value).isAbsolute || value.contains('/') || value.contains('\\')
        return if (looksLikePath) null else value
    }

    private fun bundled(baseName: String): String? {
        val root = pluginInstallDir() ?: return null
        val executable = bare(baseName)
        return sequenceOf(
            File(root, "bin/$executable"),
            File(root, "toolchain/$executable"),
            File(root, executable),
        ).mapNotNull(::existing).firstOrNull()
    }

    private fun siblingOfCompiler(compiler: String?, baseName: String): String? {
        val configuredCompiler = configured(compiler) ?: return null
        val compilerFile = File(configuredCompiler)
        if (!compilerFile.isFile) return null
        return existing(File(compilerFile.parentFile, bare(baseName)))
    }

    private fun environment(baseName: String): String? {
        val names = when (baseName) {
            "vyxc" -> listOf("VYX_COMPILER", "ZYN_VYXC")
            "vyxc-lsp" -> listOf("VYX_LSP")
            "vyxc-dap" -> listOf("VYX_DAP")
            else -> emptyList()
        }
        return names.asSequence()
            .mapNotNull { configured(System.getenv(it)) }
            .firstOrNull()
    }

    fun findOnPath(baseName: String): String? {
        val executable = bare(baseName)
        return System.getenv("PATH").orEmpty()
            .split(File.pathSeparatorChar)
            .asSequence()
            .map { it.trim().trim('"') }
            .filter { it.isNotEmpty() }
            .map { File(it, executable) }
            .mapNotNull(::existing)
            .firstOrNull()
    }

    private fun projectSdk(project: Project?, baseName: String): String? {
        var root = project?.basePath?.let(::File)?.absoluteFile ?: return null
        val executable = bare(baseName)
        repeat(8) {
            val candidates = sequenceOf(
                File(root, executable),
                File(root, "bin/$executable"),
                File(root, "toolchain/$executable"),
                File(root, "out/$executable"),
                File(root, "bootstrap_compiler/out/$executable"),
                File(root, "Zyn/bin/$executable"),
                File(root, "Zyn/toolchain/$executable"),
            )
            candidates.mapNotNull(::existing).firstOrNull()?.let { return it }
            root = root.parentFile ?: return null
        }
        return null
    }

    /** Used by the Settings preview when no Project is available. */
    fun resolve(configuredPath: String?, baseName: String): String =
        configured(configuredPath)
            ?: bundled(baseName)
            ?: environment(baseName)
            ?: bare(baseName)

    /**
     * Resolution order:
     * project setting → application setting → configured compiler sibling →
     * project/ancestor SDK → bundled plugin toolchain → environment → PATH.
     */
    fun resolveForProject(project: Project?, baseName: String): String {
        val projectSettings = project?.let { VyxSettings.getInstance(it) }
        val applicationSettings = runCatching { VyxApplicationSettings.getInstance() }.getOrNull()

        val projectValue = when (baseName) {
            "vyxc" -> projectSettings?.vyxcPath
            "vyxc-lsp" -> projectSettings?.lspPath
            "vyxc-dap" -> projectSettings?.dapPath
            else -> null
        }
        configured(projectValue)?.let { return it }

        val applicationValue = when (baseName) {
            "vyxc" -> applicationSettings?.vyxcPath
            "vyxc-lsp" -> applicationSettings?.lspPath
            "vyxc-dap" -> applicationSettings?.dapPath
            else -> null
        }
        configured(applicationValue)?.let { return it }

        if (baseName != "vyxc") {
            siblingOfCompiler(projectSettings?.vyxcPath, baseName)?.let { return it }
            siblingOfCompiler(applicationSettings?.vyxcPath, baseName)?.let { return it }
        }
        projectSdk(project, baseName)?.let { return it }
        bundled(baseName)?.let { return it }
        environment(baseName)?.let { return it }
        return bare(baseName)
    }

    /**
     * The SDK entry point `vyxc-dap` selects its matching debug runtime.
     * Prefer project/compiler SDK tools before a globally installed adapter.
     */
    fun resolveVyxDapForProject(project: Project?): String {
        val projectSettings = project?.let { VyxSettings.getInstance(it) }
        val applicationSettings = runCatching { VyxApplicationSettings.getInstance() }.getOrNull()

        siblingOfCompiler(projectSettings?.vyxcPath, "vyxc-dap")?.let { return it }
        siblingOfCompiler(applicationSettings?.vyxcPath, "vyxc-dap")?.let { return it }
        projectSdk(project, "vyxc-dap")?.let { return it }
        bundled("vyxc-dap")?.let { return it }
        configured(projectSettings?.dapPath)?.let { return it }
        configured(applicationSettings?.dapPath)?.let { return it }
        environment("vyxc-dap")?.let { return it }
        findOnPath("vyxc-dap")?.let { return it }
        return bare("vyxc-dap")
    }
}
