package com.vyx.plugin

import com.intellij.openapi.fileChooser.FileChooserDescriptorFactory
import com.intellij.openapi.options.Configurable
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.TextFieldWithBrowseButton
import com.intellij.ui.components.JBLabel
import com.intellij.util.ui.JBUI
import java.awt.GridBagConstraints
import java.awt.GridBagLayout
import java.awt.Insets
import javax.swing.*

/**
 * Languages and Frameworks - Vyx
 * Single UI. Apply writes project settings and mirrors IDE-wide defaults
 * so New Project wizard can prefill from the same page.
 */
class VyxSettingsConfigurable(private val project: Project) : Configurable {
    private var pathField: TextFieldWithBrowseButton? = null
    private var lspField: TextFieldWithBrowseButton? = null
    private var dapField: TextFieldWithBrowseButton? = null
    private var resolvedLabel: JBLabel? = null

    override fun getDisplayName() = "Vyx"

    override fun createComponent(): JComponent {
        val panel = JPanel(GridBagLayout())
        val gbc = GridBagConstraints()
        gbc.anchor = GridBagConstraints.NORTHWEST
        gbc.fill = GridBagConstraints.HORIZONTAL
        gbc.insets = Insets(8, 8, 4, 8)
        val exeFilter = FileChooserDescriptorFactory.createSingleFileDescriptor()

        fun addPathRow(row: Int, title: String, field: TextFieldWithBrowseButton, browseTitle: String, browseDesc: String) {
            gbc.gridy = row; gbc.gridx = 0; gbc.weightx = 0.0
            panel.add(JLabel(title), gbc)
            gbc.gridy = row + 1; gbc.gridx = 0; gbc.weightx = 1.0
            field.addBrowseFolderListener(browseTitle, browseDesc, project, exeFilter)
            field.textField.document.addDocumentListener(object : javax.swing.event.DocumentListener {
                override fun insertUpdate(e: javax.swing.event.DocumentEvent?) = refreshResolved()
                override fun removeUpdate(e: javax.swing.event.DocumentEvent?) = refreshResolved()
                override fun changedUpdate(e: javax.swing.event.DocumentEvent?) = refreshResolved()
            })
            panel.add(field, gbc)
        }

        pathField = TextFieldWithBrowseButton()
        lspField = TextFieldWithBrowseButton()
        dapField = TextFieldWithBrowseButton()

        addPathRow(0, "vyxc (compiler) - empty = plugin dir / PATH:", pathField!!, "Select vyxc", "Path to vyxc compiler")
        addPathRow(2, "vyxc-lsp (language server) - empty = plugin dir / PATH:", lspField!!, "Select vyxc-lsp", "Path to vyxc-lsp")
        addPathRow(4, "vyxc-dap (debug adapter) - empty = plugin dir / PATH:", dapField!!, "Select vyxc-dap", "Path to vyxc-dap")

        gbc.gridy = 6; gbc.gridx = 0; gbc.weightx = 1.0
        gbc.insets = Insets(12, 8, 4, 8)
        panel.add(JLabel("Resolved toolchain (live preview):"), gbc)

        resolvedLabel = JBLabel("").apply {
            border = JBUI.Borders.empty(4, 8, 8, 8)
            verticalAlignment = SwingConstants.TOP
        }
        gbc.gridy = 7; gbc.weighty = 0.0
        panel.add(resolvedLabel, gbc)

        gbc.gridy = 8; gbc.weighty = 1.0
        panel.add(JPanel(), gbc)

        refreshResolved()
        return panel
    }

    private fun refreshResolved() {
        val vyxc = VyxToolPaths.resolve(pathField?.text, "vyxc")
        val lsp = VyxToolPaths.resolve(lspField?.text, "vyxc-lsp")
        val dap = VyxToolPaths.resolve(dapField?.text, "vyxc-dap")
        resolvedLabel?.text =
            "<html><body style='width:420px;font-family:monospace'>" +
                "vyxc = <b>${esc(vyxc)}</b><br>" +
                "vyxc-lsp = <b>${esc(lsp)}</b><br>" +
                "vyxc-dap = <b>${esc(dap)}</b><br>" +
                "<span style='color:gray'>Order: Settings path -&gt; plugin install dir -&gt; PATH<br>" +
                "Apply also stores defaults for New Project wizard.</span>" +
                "</body></html>"
    }

    private fun esc(s: String): String =
        s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

    override fun isModified(): Boolean {
        val settings = VyxSettings.getInstance(project)
        return pathField?.text != settings.vyxcPath ||
            lspField?.text != settings.lspPath ||
            dapField?.text != settings.dapPath
    }

    override fun apply() {
        val settings = VyxSettings.getInstance(project)
        settings.vyxcPath = pathField?.text ?: ""
        settings.lspPath = lspField?.text ?: ""
        settings.dapPath = dapField?.text ?: ""
        val app = VyxApplicationSettings.getInstance()
        app.vyxcPath = settings.vyxcPath
        app.lspPath = settings.lspPath
        app.dapPath = settings.dapPath
    }

    override fun reset() {
        val settings = VyxSettings.getInstance(project)
        val app = VyxApplicationSettings.getInstance()
        pathField?.text = settings.vyxcPath.ifBlank { app.vyxcPath }
        lspField?.text = settings.lspPath.ifBlank { app.lspPath }
        dapField?.text = settings.dapPath.ifBlank { app.dapPath }
        refreshResolved()
    }
}