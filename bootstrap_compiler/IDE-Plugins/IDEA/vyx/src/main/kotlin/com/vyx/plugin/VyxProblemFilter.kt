package com.vyx.plugin

import com.intellij.openapi.project.Project
import com.intellij.openapi.util.Condition
import com.intellij.openapi.vfs.VirtualFile

class VyxProblemFilter : Condition<VirtualFile> {
    override fun value(file: VirtualFile): Boolean {
        return file.extension == "vyx"
    }
}
