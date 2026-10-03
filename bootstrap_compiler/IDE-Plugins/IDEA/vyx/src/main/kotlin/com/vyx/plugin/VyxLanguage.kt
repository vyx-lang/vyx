package com.vyx.plugin
import com.intellij.lang.Language
class VyxLanguage : Language("Vyx") {
    companion object { @JvmField val INSTANCE = VyxLanguage() }
}
