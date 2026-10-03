package com.vyx.plugin
import com.intellij.lang.ASTNode; import com.intellij.lang.ParserDefinition; import com.intellij.lang.PsiParser; import com.intellij.lexer.Lexer
import com.intellij.openapi.project.Project; import com.intellij.psi.*; import com.intellij.psi.tree.IFileElementType; import com.intellij.psi.tree.TokenSet
class VyxParserDefinition : ParserDefinition {
    companion object { val FILE = IFileElementType(VyxLanguage.INSTANCE) }
    override fun createLexer(project: Project?): Lexer = VyxLexer()
    override fun createParser(project: Project?): PsiParser = VyxParser()
    override fun getFileNodeType(): IFileElementType = FILE
    override fun getCommentTokens(): TokenSet = TokenSet.create(VyxTokenTypes.COMMENT)
    override fun getStringLiteralElements(): TokenSet = TokenSet.create(VyxTokenTypes.STRING)
    override fun getWhitespaceTokens(): TokenSet = TokenSet.create(TokenType.WHITE_SPACE)
    override fun createElement(node: ASTNode): PsiElement = com.intellij.psi.impl.source.tree.LeafPsiElement(node.elementType, node.text)
    override fun createFile(viewProvider: FileViewProvider): PsiFile = VyxFile(viewProvider)
}
