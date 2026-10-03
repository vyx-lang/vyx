package com.vyx.plugin

import com.intellij.codeInsight.template.TemplateActionContext
import com.intellij.codeInsight.template.TemplateContextType

class VyxLiveTemplateContext : TemplateContextType("VYX", "Vyx") {
    override fun isInContext(templateActionContext: TemplateActionContext): Boolean {
        return templateActionContext.file.name.endsWith(".vyx")
    }
}
