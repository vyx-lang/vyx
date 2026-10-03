package com.vyx.plugin
import com.intellij.extapi.psi.PsiFileBase
import com.intellij.openapi.fileTypes.FileType
import com.intellij.psi.FileViewProvider
class VyxFile(viewProvider: FileViewProvider) : PsiFileBase(viewProvider, VyxLanguage.INSTANCE) {
    override fun getFileType(): FileType = VyxFileType.INSTANCE
    override fun toString(): String = "Vyx File"
}
