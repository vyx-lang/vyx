package com.vyx.plugin

import com.intellij.codeInsight.editorActions.enter.EnterBetweenBracesAndBracketsDelegate

/**
 * Registered via enterBetweenBracesDelegate for language=Vyx.
 * Enables Enter-to-indent between { } / ( ) / [ ] pairs.
 */
class VyxEnterBetweenBracesHandler : EnterBetweenBracesAndBracketsDelegate()