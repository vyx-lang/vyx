package com.vyx.plugin

import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.openapi.project.Project
import com.intellij.util.execution.ParametersListUtil
import java.io.File

/**
 * Real vyxc CLI (bl-2026-07-14+):
 * - project build: `vyxc build` [-g -O0]
 * - project build+run: `vyxc --run=aot --src=project <dir> --target <name> -- <program args>`
 * - single file compile+run: `vyxc --src=file <path> --run=aot` [-g -O0]
 * - toml scripts: `vyxc run <script|package:script>` (NOT source files)
 * Old `vyxc --run <file>` is removed (E0003).
 */
object VyxCli {
    fun findVyxc(project: Project?): String = VyxToolPaths.resolveForProject(project, "vyxc")

    fun runFile(project: Project?, filePath: String, debug: Boolean = false): GeneralCommandLine {
        val args = mutableListOf(findVyxc(project))
        if (debug) args.addAll(listOf("-g", "-O0"))
        args.add("--src=file")
        args.add(filePath)
        args.add("--run=aot")
        val cmd = GeneralCommandLine(args)
        val parent = File(filePath).parentFile
        if (parent != null) cmd.workDirectory = parent
        return cmd
    }

    fun compileFileForDebug(project: Project?, filePath: String, output: File): GeneralCommandLine {
        output.parentFile?.mkdirs()
        val cmd = GeneralCommandLine(
            findVyxc(project),
            "-g",
            "-O0",
            "--src=file",
            filePath,
            "--emit=exe",
            "-o",
            output.absolutePath,
        )
        cmd.workDirectory = File(filePath).parentFile ?: output.parentFile
        return cmd
    }

    fun buildProject(project: Project?, workDir: File, target: String? = null, debug: Boolean = false,
                     artifactFile: File? = null): GeneralCommandLine {
        return projectBuildCommand(findVyxc(project), workDir, target, debug, artifactFile)
    }

    internal fun projectBuildCommand(compiler: String, workDir: File, target: String? = null,
                                     debug: Boolean = false, artifactFile: File? = null,
                                     run: Boolean = false): GeneralCommandLine {
        val args = mutableListOf(compiler, "--src=project", workDir.absolutePath)
        if (debug) args.addAll(listOf("-g", "-O0"))
        if (!target.isNullOrBlank() && target != "(auto)") {
            args.add("--target")
            args.add(target)
        }
        if (artifactFile != null) args.addAll(listOf("--artifact-file", artifactFile.absolutePath))
        if (run) args.add("--run=aot")
        val cmd = GeneralCommandLine(args)
        cmd.workDirectory = workDir
        return cmd
    }

    /** The manifest builder selects and launches its own executable artifact. */
    fun buildAndRunProject(project: Project?, workDir: File, target: String? = null,
                           debug: Boolean = false): GeneralCommandLine {
        return projectBuildCommand(findVyxc(project), workDir, target, debug, run = true)
    }

    internal fun programArguments(raw: String): List<String> =
        ParametersListUtil.parse(raw, false, false, true)

    fun readBuiltExe(artifactFile: File): File {
        val path = artifactFile.readText(Charsets.UTF_8).trimEnd('\r', '\n')
        val executable = File(path)
        require(path.isNotEmpty() && executable.isAbsolute && executable.isFile) {
            "Compiler did not publish a valid executable artifact: ${artifactFile.path}"
        }
        return executable
    }
}
