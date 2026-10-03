package com.vyx.plugin

import com.intellij.lang.BracePair
import com.intellij.lang.PairedBraceMatcher
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IElementType

class VyxBraceMatcher : PairedBraceMatcher {
    private val pairs = arrayOf(
        BracePair(VyxTokenTypes.LBRACE, VyxTokenTypes.RBRACE, true),
        BracePair(VyxTokenTypes.LPAREN, VyxTokenTypes.RPAREN, false),
        BracePair(VyxTokenTypes.LBRACKET, VyxTokenTypes.RBRACKET, false),
    )

    override fun getPairs(): Array<BracePair> = pairs

    override fun isPairedBracesAllowedBeforeType(lbraceType: IElementType, contextType: IElementType?): Boolean = true

    override fun getCodeConstructStart(file: PsiFile, openingBraceOffset: Int): Int = openingBraceOffset
}