package com.vyx.plugin

import java.io.File
import java.nio.file.Files
import org.junit.Assert.*
import org.junit.Test

class VyxCliTest {
    @Test
    fun testProjectRunUsesManifestTargetAndWorkingDirectory() {
        val directory = Files.createTempDirectory("vyx project ").toFile()
        val compiler = File(directory, "vyxc.exe").apply { writeText("") }
        try {
            val command = VyxCli.projectBuildCommand(compiler.absolutePath, directory, "qt_counter", run = true)
            assertEquals(compiler.absolutePath, command.exePath)
            assertEquals(listOf("--src=project", directory.absolutePath, "--target", "qt_counter", "--run=aot"), command.parametersList.list)
            assertEquals(directory, command.workDirectory)
            val automatic = VyxCli.projectBuildCommand(compiler.absolutePath, directory, "(auto)", run = true)
            assertEquals(listOf("--src=project", directory.absolutePath, "--run=aot"), automatic.parametersList.list)
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test
    fun testArgumentsPreserveQuotedWordsAndEmptyValues() {
        val arguments = "\"two words\" \"\" --target=program-value C:\\data"
        assertEquals(listOf("two words", "", "--target=program-value", "C:\\data"), VyxCli.programArguments(arguments))
    }

    @Test
    fun testDebugUsesCompilerArtifactInsteadOfManifestNameOrOutDirectory() {
        val directory = Files.createTempDirectory("vyx artifacts ").toFile()
        try {
            File(directory, "Vyx.toml").writeText("[package]\nname = \"wrong_guess\"\n")
            val executable = File(directory, "custom products/actual.exe").apply {
                parentFile.mkdirs()
                writeText("native output")
            }
            val artifact = File(directory, "artifact.txt").apply { writeText(executable.absolutePath + "\n") }
            assertEquals(executable, VyxCli.readBuiltExe(artifact))
            artifact.writeText(File(directory, "absent.exe").absolutePath + "\n")
            assertThrows(IllegalArgumentException::class.java) { VyxCli.readBuiltExe(artifact) }
        } finally {
            directory.deleteRecursively()
        }
    }
}
