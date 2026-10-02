package com.vyx.plugin

import com.intellij.execution.configurations.RunProfile
import com.intellij.execution.executors.DefaultRunExecutor
import com.intellij.execution.runners.DefaultProgramRunner
import com.intellij.openapi.project.Project

/**
 * Handles normal Run. Debug is intentionally left to IntelliJ's generic
 * DapProgramRunner through VyxDapLaunchArgumentsProvider.
 */
open class VyxProgramRunner : DefaultProgramRunner() {
    override fun getRunnerId(): String = "VyxProgramRunner"

    override fun canRun(executorId: String, profile: RunProfile): Boolean {
        if (profile !is VyxRunConfig) return false
        return executorId == DefaultRunExecutor.EXECUTOR_ID
    }
}

/** Alias kept so older references/plugin.xml still resolve if needed. */
class VyxDebugRunner : VyxProgramRunner() {
    override fun getRunnerId(): String = "VyxDebugRunner"
}

fun findDap(project: Project?): String = VyxToolPaths.resolveForProject(project, "vyxc-dap")
