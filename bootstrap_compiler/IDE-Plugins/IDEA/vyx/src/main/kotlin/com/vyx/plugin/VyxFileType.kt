package com.vyx.plugin

import com.intellij.openapi.fileTypes.LanguageFileType
import javax.swing.Icon

class VyxFileType private constructor() : LanguageFileType(VyxLanguage.INSTANCE) {
    companion object {
        @JvmField
        val INSTANCE = VyxFileType()
    }

    override fun getName(): String = "Vyx"
    override fun getDescription(): String = "Vyx language file"
    override fun getDefaultExtension(): String = "vyx"
    override fun getIcon(): Icon = VyxIcons.Vyx
}