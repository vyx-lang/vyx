package com.vyx.plugin

import com.intellij.openapi.project.Project
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.platform.lsp.api.LspServerSupportProvider
import com.intellij.platform.lsp.api.ProjectWideLspServerDescriptor
import com.intellij.platform.lsp.api.customization.LspFormattingSupport
import com.intellij.platform.lsp.api.customization.LspSemanticTokensSupport
import java.io.File
import java.nio.charset.StandardCharsets

class VyxLspServerSupportProvider : LspServerSupportProvider {
    override fun fileOpened(
        project: Project,
        file: VirtualFile,
        serverStarter: LspServerSupportProvider.LspServerStarter
    ) {
        if (file.extension == "vyx" || file.extension == "vyi") {
            serverStarter.ensureServerStarted(VyxLspServerDescriptor(project))
        }
    }
}

class VyxLspServerDescriptor(project: Project) : ProjectWideLspServerDescriptor(project, "Vyx") {
    override fun isSupportedFile(file: VirtualFile): Boolean =
        file.extension == "vyx" || file.extension == "vyi"

    override fun createCommandLine(): com.intellij.execution.configurations.GeneralCommandLine {
        val lspPath = VyxToolPaths.resolveForProject(project, "vyxc-lsp")
        val cmd = com.intellij.execution.configurations.GeneralCommandLine(lspPath)
            .withParameters("--stdio")
            .withWorkDirectory(project.basePath)
            .withCharset(StandardCharsets.UTF_8)
        val dir = File(lspPath).parent
        if (dir != null) {
            val oldPath = System.getenv("PATH") ?: ""
            cmd.withEnvironment("PATH", dir + File.pathSeparator + oldPath)
        }
        val llvm = System.getenv("LLVM_ROOT")
        if (!llvm.isNullOrBlank()) {
            cmd.withEnvironment("LLVM_ROOT", llvm)
        }
        return cmd
    }

    override val lspFormattingSupport: LspFormattingSupport
        get() = object : LspFormattingSupport() {
            override fun shouldFormatThisFileExclusivelyByServer(
                file: VirtualFile,
                ideCanFormatThisFileItself: Boolean,
                serverHasDocumentFormattingProvider: Boolean,
            ): Boolean =
                (file.extension == "vyx" || file.extension == "vyi") && serverHasDocumentFormattingProvider
        }

    @Suppress("DEPRECATION")
    override val lspSemanticTokensSupport: LspSemanticTokensSupport
        get() = VyxSemanticTokensSupport
}

private object VyxSemanticTokensSupport : LspSemanticTokensSupport() {
    override fun getTextAttributesKey(
        tokenType: String,
        modifiers: List<String>,
    ): TextAttributesKey = when (tokenType) {
        "namespace" -> VyxSyntaxHighlighter.MODULE
        "type" -> VyxSyntaxHighlighter.TYPE
        "class", "enum", "interface", "struct", "typeParameter" -> {
            if ("declaration" in modifiers) VyxSyntaxHighlighter.TYPE_DECLARATION
            else VyxSyntaxHighlighter.TYPE_REFERENCE
        }
        "parameter" -> VyxSyntaxHighlighter.PARAMETER
        "variable" -> {
            if ("readonly" in modifiers) VyxSyntaxHighlighter.CONSTANT
            else VyxSyntaxHighlighter.LOCAL_VARIABLE
        }
        "property", "enumMember" -> VyxSyntaxHighlighter.FIELD
        "function" -> {
            if ("declaration" in modifiers) VyxSyntaxHighlighter.FUNCTION_DECLARATION
            else VyxSyntaxHighlighter.FUNCTION_CALL
        }
        "method" -> VyxSyntaxHighlighter.METHOD
        "macro", "decorator" -> VyxSyntaxHighlighter.ATTRIBUTE
        "keyword", "modifier" -> VyxSyntaxHighlighter.KEYWORD
        "comment" -> VyxSyntaxHighlighter.COMMENT
        "string", "regexp" -> VyxSyntaxHighlighter.STRING
        "number" -> VyxSyntaxHighlighter.NUMBER
        "operator" -> VyxSyntaxHighlighter.OPERATOR
        else -> VyxSyntaxHighlighter.IDENTIFIER
    }
}
