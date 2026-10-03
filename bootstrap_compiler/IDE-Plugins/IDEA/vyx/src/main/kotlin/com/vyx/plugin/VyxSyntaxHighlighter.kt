package com.vyx.plugin

import com.intellij.lexer.Lexer
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighter
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase
import com.intellij.openapi.fileTypes.SyntaxHighlighterFactory
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.psi.tree.IElementType

class VyxSyntaxHighlighterFactory : SyntaxHighlighterFactory() {
    override fun getSyntaxHighlighter(project: Project?, virtualFile: VirtualFile?): SyntaxHighlighter =
        VyxSyntaxHighlighter()
}

class VyxSyntaxHighlighter : SyntaxHighlighterBase() {
    companion object {
        val KEYWORD = TextAttributesKey.createTextAttributesKey(
            "VYX_KEYWORD", DefaultLanguageHighlighterColors.KEYWORD
        )
        val STRING = TextAttributesKey.createTextAttributesKey(
            "VYX_STRING", DefaultLanguageHighlighterColors.STRING
        )
        val NUMBER = TextAttributesKey.createTextAttributesKey(
            "VYX_NUMBER", DefaultLanguageHighlighterColors.NUMBER
        )
        val COMMENT = TextAttributesKey.createTextAttributesKey(
            "VYX_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT
        )
        val TYPE = TextAttributesKey.createTextAttributesKey(
            "VYX_TYPE", DefaultLanguageHighlighterColors.CLASS_NAME
        )
        val IDENTIFIER = TextAttributesKey.createTextAttributesKey(
            "VYX_IDENTIFIER", DefaultLanguageHighlighterColors.IDENTIFIER
        )
        val FUNCTION_DECLARATION = TextAttributesKey.createTextAttributesKey(
            "VYX_FUNCTION_DECLARATION", DefaultLanguageHighlighterColors.FUNCTION_DECLARATION
        )
        val FUNCTION_CALL = TextAttributesKey.createTextAttributesKey(
            "VYX_FUNCTION_CALL", DefaultLanguageHighlighterColors.FUNCTION_CALL
        )
        val METHOD = TextAttributesKey.createTextAttributesKey(
            "VYX_METHOD", DefaultLanguageHighlighterColors.INSTANCE_METHOD
        )
        val FIELD = TextAttributesKey.createTextAttributesKey(
            "VYX_FIELD", DefaultLanguageHighlighterColors.INSTANCE_FIELD
        )
        val PARAMETER = TextAttributesKey.createTextAttributesKey(
            "VYX_PARAMETER", DefaultLanguageHighlighterColors.PARAMETER
        )
        val LOCAL_VARIABLE = TextAttributesKey.createTextAttributesKey(
            "VYX_LOCAL_VARIABLE", DefaultLanguageHighlighterColors.LOCAL_VARIABLE
        )
        val CONSTANT = TextAttributesKey.createTextAttributesKey(
            "VYX_CONSTANT", DefaultLanguageHighlighterColors.CONSTANT
        )
        val TYPE_DECLARATION = TextAttributesKey.createTextAttributesKey(
            "VYX_TYPE_DECLARATION", DefaultLanguageHighlighterColors.CLASS_NAME
        )
        val TYPE_REFERENCE = TextAttributesKey.createTextAttributesKey(
            "VYX_TYPE_REFERENCE", DefaultLanguageHighlighterColors.CLASS_REFERENCE
        )
        val MODULE = TextAttributesKey.createTextAttributesKey(
            "VYX_MODULE", DefaultLanguageHighlighterColors.CLASS_REFERENCE
        )
        val OPERATOR = TextAttributesKey.createTextAttributesKey(
            "VYX_OPERATOR", DefaultLanguageHighlighterColors.OPERATION_SIGN
        )
        val BRACES = TextAttributesKey.createTextAttributesKey(
            "VYX_BRACES", DefaultLanguageHighlighterColors.BRACES
        )
        val PARENTHESES = TextAttributesKey.createTextAttributesKey(
            "VYX_PARENTHESES", DefaultLanguageHighlighterColors.PARENTHESES
        )
        val BRACKETS = TextAttributesKey.createTextAttributesKey(
            "VYX_BRACKETS", DefaultLanguageHighlighterColors.BRACKETS
        )
        val SEMICOLON = TextAttributesKey.createTextAttributesKey(
            "VYX_SEMICOLON", DefaultLanguageHighlighterColors.SEMICOLON
        )
        val COMMA = TextAttributesKey.createTextAttributesKey(
            "VYX_COMMA", DefaultLanguageHighlighterColors.COMMA
        )
        val DOT = TextAttributesKey.createTextAttributesKey(
            "VYX_DOT", DefaultLanguageHighlighterColors.DOT
        )
        val ATTRIBUTE = TextAttributesKey.createTextAttributesKey(
            "VYX_ATTRIBUTE", DefaultLanguageHighlighterColors.METADATA
        )
    }

    override fun getHighlightingLexer(): Lexer = VyxLexer()

    override fun getTokenHighlights(tokenType: IElementType?): Array<TextAttributesKey> = when {
        tokenType === VyxTokenTypes.KEYWORD -> arrayOf(KEYWORD)
        tokenType === VyxTokenTypes.STRING -> arrayOf(STRING)
        tokenType === VyxTokenTypes.NUMBER -> arrayOf(NUMBER)
        tokenType === VyxTokenTypes.COMMENT -> arrayOf(COMMENT)
        tokenType === VyxTokenTypes.TYPE -> arrayOf(TYPE)
        tokenType === VyxTokenTypes.IDENTIFIER -> arrayOf(IDENTIFIER)
        tokenType === VyxTokenTypes.FUNCTION_DECLARATION -> arrayOf(FUNCTION_DECLARATION)
        tokenType === VyxTokenTypes.FUNCTION_CALL -> arrayOf(FUNCTION_CALL)
        tokenType === VyxTokenTypes.METHOD -> arrayOf(METHOD)
        tokenType === VyxTokenTypes.FIELD -> arrayOf(FIELD)
        tokenType === VyxTokenTypes.PARAMETER -> arrayOf(PARAMETER)
        tokenType === VyxTokenTypes.LOCAL_VARIABLE -> arrayOf(LOCAL_VARIABLE)
        tokenType === VyxTokenTypes.CONSTANT -> arrayOf(CONSTANT)
        tokenType === VyxTokenTypes.TYPE_DECLARATION -> arrayOf(TYPE_DECLARATION)
        tokenType === VyxTokenTypes.TYPE_REFERENCE -> arrayOf(TYPE_REFERENCE)
        tokenType === VyxTokenTypes.MODULE -> arrayOf(MODULE)
        tokenType === VyxTokenTypes.OPERATOR || tokenType === VyxTokenTypes.COLON -> arrayOf(OPERATOR)
        tokenType === VyxTokenTypes.LBRACE || tokenType === VyxTokenTypes.RBRACE -> arrayOf(BRACES)
        tokenType === VyxTokenTypes.LPAREN || tokenType === VyxTokenTypes.RPAREN -> arrayOf(PARENTHESES)
        tokenType === VyxTokenTypes.LBRACKET || tokenType === VyxTokenTypes.RBRACKET -> arrayOf(BRACKETS)
        tokenType === VyxTokenTypes.SEMICOLON -> arrayOf(SEMICOLON)
        tokenType === VyxTokenTypes.COMMA -> arrayOf(COMMA)
        tokenType === VyxTokenTypes.DOT -> arrayOf(DOT)
        tokenType === VyxTokenTypes.ATTRIBUTE -> arrayOf(ATTRIBUTE)
        else -> TextAttributesKey.EMPTY_ARRAY
    }
}
