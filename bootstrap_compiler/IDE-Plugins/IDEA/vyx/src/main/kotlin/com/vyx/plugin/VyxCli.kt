package com.vyx.plugin

import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.openapi.project.Project
import java.io.File

/**
 * Real vyxc CLI (bl-2026-07-14+):
 * - project build: `vyxc build` [-g]
 * - project build+run: `vyxc --src=project <dir> --run=aot` [-g]
 * - single file compile+run: `vyxc --src=file <path> --run=aot` [-g]
 * - toml scripts: `vyxc run <script|package:script>` (NOT source files)
 * Old `vyxc --run <file>` is removed (E0003).
 */
object VyxCli {
    fun findVyxc(project: Project?): String = VyxToolPaths.resolveForProject(project, "vyxc")

    fun runFile(project: Project?, filePath: String, debug: Boolean = false): GeneralCommandLine {
        val args = mutableListOf(findVyxc(project))
        if (debug) args.add("-g")
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
            "--src=file",
            filePath,
            "--emit=exe",
            "-o",
            output.absolutePath,
        )
        cmd.workDirectory = File(filePath).parentFile ?: output.parentFile
        return cmd
    }

    fun buildProject(project: Project?, workDir: File, target: String? = null, debug: Boolean = false): GeneralCommandLine {
        val args = mutableListOf(findVyxc(project), "build")
        if (debug) args.add("-g")
        if (!target.isNullOrBlank() && target != "(auto)") {
            args.add("--target")
            args.add(target)
        }
        val cmd = GeneralCommandLine(args)
        cmd.workDirectory = workDir
        return cmd
    }

    /** Build + execute project entry (uses package entry / target). */
    fun buildAndRunProject(project: Project?, workDir: File, debug: Boolean = false): GeneralCommandLine {
        val args = mutableListOf(findVyxc(project))
        if (debug) args.add("-g")
        args.add("--src=project")
        args.add(workDir.absolutePath)
        args.add("--run=aot")
        val cmd = GeneralCommandLine(args)
        cmd.workDirectory = workDir
        return cmd
    }

    fun resolveBuiltExe(wd: File, targetName: String = ""): File {
        var name = "main"
        val tomlFile = File(wd, "Vyx.toml")
        if (tomlFile.exists()) {
            for (line in tomlFile.readLines()) {
                val m = Regex("""name\s*=\s*"(.+?)"""").find(line)
                if (m != null) {
                    name = m.groupValues[1]
                    break
                }
            }
        }
        if (targetName.isNotBlank() && targetName != "(auto)") {
            name = targetName
        }
        val outDir = File(wd, "out")
        val candidates = listOf(
            File(outDir, if (VyxToolPaths.isWindows()) "$name.exe" else name),
            File(wd, if (VyxToolPaths.isWindows()) "$name.exe" else name),
            File(outDir, name),
            File(wd, name)
        )
        return candidates.firstOrNull { it.exists() } ?: candidates.first()
    }
}
