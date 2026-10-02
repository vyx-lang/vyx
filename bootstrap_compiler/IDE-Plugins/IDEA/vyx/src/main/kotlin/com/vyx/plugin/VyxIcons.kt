package com.vyx.plugin

import com.intellij.openapi.util.IconLoader
import javax.swing.Icon

/**
 * Brand icons loaded from resources generated from repo-root vyx.png.
 * Paths are classpath-relative under src/main/resources.
 */
object VyxIcons {
    /** File / run / project small icon (16 + @2x 32). */
    @JvmField
    val Vyx: Icon = IconLoader.getIcon("/icons/vyx.png", VyxIcons::class.java)

    /** Larger mark for tool windows / generators when needed. */
    @JvmField
    val VyxTool: Icon = IconLoader.getIcon("/icons/vyx_tool.png", VyxIcons::class.java)
}