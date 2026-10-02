package com.vyx.plugin

import com.intellij.openapi.util.TextRange
import com.intellij.psi.PsiFile
import com.intellij.psi.codeStyle.ExternalFormatProcessor

class VyxExternalFormatProcessor : ExternalFormatProcessor {
    override fun getId(): String = "vyx-indent"

    override fun activeForFile(source: PsiFile): Boolean =
        source.fileType == VyxFileType.INSTANCE

    override fun format(
        source: PsiFile,
        range: TextRange,
        canChangeWhiteSpacesOnly: Boolean,
        keepLineBreaks: Boolean,
        enableBulkUpdate: Boolean,
        cursorOffset: Int,
    ): TextRange? {
        val document = source.viewProvider.document ?: return null
        val formatted = VyxIndent.format(document.text)
        if (formatted == document.text) return range
        document.setText(formatted)
        return TextRange(0, formatted.length)
    }

    override fun indent(source: PsiFile, lineStartOffset: Int): String = "    "
}
