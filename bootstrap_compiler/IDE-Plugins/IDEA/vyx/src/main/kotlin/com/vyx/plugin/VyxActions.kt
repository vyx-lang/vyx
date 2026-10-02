package com.vyx.plugin

import com.intellij.execution.process.OSProcessHandler
import com.intellij.execution.process.ProcessTerminatedListener
import com.intellij.execution.filters.TextConsoleBuilderFactory
import com.intellij.openapi.actionSystem.AnAction
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.actionSystem.CommonDataKeys
import com.intellij.openapi.project.Project
import java.io.File

fun findVyxc(project: Project?): String = VyxCli.findVyxc(project)

private fun runInConsole(project: Project, cmd: com.intellij.execution.configurations.GeneralCommandLine) {
    val handler = OSProcessHandler(cmd)
    ProcessTerminatedListener.attach(handler)
    val console = TextConsoleBuilderFactory.getInstance().createBuilder(project).console
    console.attachToProcess(handler)
    // Show console tool window if available
    try {
        val tw = com.intellij.openapi.wm.ToolWindowManager.getInstance(project).getToolWindow("Run")
        tw?.activate(null)
    } catch (_: Throwable) {
    }
    handler.startNotify()
}

class VyxRunFileAction : AnAction("Run Current File", "vyxc --src=file <file> --run=aot", VyxIcons.Vyx) {
    override fun actionPerformed(e: AnActionEvent) {
        val project = e.project ?: return
        val file = e.getData(CommonDataKeys.VIRTUAL_FILE) ?: return
        if (file.extension != "vyx") return
        val cmd = VyxCli.runFile(project, file.path, debug = false)
        runInConsole(project, cmd)
    }
    override fun update(e: AnActionEvent) {
        val file = e.getData(CommonDataKeys.VIRTUAL_FILE)
        e.presentation.isEnabled = file?.extension == "vyx"
    }
}

class VyxBuildAction : AnAction("Build Project", "vyxc build", VyxIcons.Vyx) {
    override fun actionPerformed(e: AnActionEvent) {
        val project = e.project ?: return
        val wd = File(project.basePath ?: ".")
        val cmd = VyxCli.buildProject(project, wd)
        runInConsole(project, cmd)
    }
}

class VyxTestAction : AnAction("Run Tests", "vyxc run test (scripts)", com.intellij.icons.AllIcons.RunConfigurations.TestState.Run) {
    override fun actionPerformed(e: AnActionEvent) {
        val project = e.project ?: return
        // TOML [scripts.test] style: vyxc run test
        val cmd = com.intellij.execution.configurations.GeneralCommandLine(VyxCli.findVyxc(project), "run", "test")
        cmd.workDirectory = File(project.basePath ?: ".")
        runInConsole(project, cmd)
    }
}

class VyxFmtAction : AnAction("Format File", "Reformat with Vyx indent / LSP formatter", com.intellij.icons.AllIcons.Actions.ReformatCode) {
    override fun actionPerformed(e: AnActionEvent) {
        val project = e.project ?: return
        val file = e.getData(CommonDataKeys.PSI_FILE) ?: return
        if (file.fileType != VyxFileType.INSTANCE) return
        com.intellij.openapi.command.WriteCommandAction.runWriteCommandAction(project) {
            com.intellij.psi.codeStyle.CodeStyleManager.getInstance(project).reformat(file)
        }
    }
    override fun update(e: AnActionEvent) {
        val file = e.getData(CommonDataKeys.VIRTUAL_FILE)
        e.presentation.isEnabled = file?.extension == "vyx" || file?.extension == "vyi"
    }
}

class VyxDocAction : AnAction("Generate Docs", "Docs (placeholder)", com.intellij.icons.AllIcons.Toolwindows.Documentation) {
    override fun actionPerformed(e: AnActionEvent) {
        val project = e.project ?: return
        com.intellij.openapi.ui.Messages.showInfoMessage(
            project,
            "Current vyxc has no `doc` subcommand yet.",
            "Vyx Docs"
        )
    }
}