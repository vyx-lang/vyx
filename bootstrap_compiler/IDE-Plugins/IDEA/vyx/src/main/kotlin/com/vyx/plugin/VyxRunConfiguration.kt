package com.vyx.plugin

import com.intellij.execution.ExecutionResult
import com.intellij.execution.Executor
import com.intellij.execution.configurations.*
import com.intellij.execution.executors.DefaultDebugExecutor
import com.intellij.execution.executors.DefaultRunExecutor
import com.intellij.execution.process.OSProcessHandler
import com.intellij.execution.process.NopProcessHandler
import com.intellij.execution.process.ProcessTerminatedListener
import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.execution.runners.ProgramRunner
import com.intellij.openapi.fileChooser.FileChooserDescriptorFactory
import com.intellij.openapi.options.SettingsEditor
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.TextFieldWithBrowseButton
import java.awt.GridBagConstraints
import java.awt.GridBagLayout
import java.awt.Insets
import java.io.File
import javax.swing.*

class VyxConfigurationType : ConfigurationType {
    override fun getDisplayName() = "Vyx"
    override fun getConfigurationTypeDescription() =
        "Run / build Vyx programs (vyxc --src=file/--src=project --run=aot or vyxc build)"
    override fun getIcon(): Icon = VyxIcons.Vyx
    override fun getId() = "VYX_RUN"
    override fun getConfigurationFactories() = arrayOf(
        VyxSingleFileFactory(this),
        VyxProjectFactory(this)
    )
}

class VyxSingleFileFactory(type: ConfigurationType) : ConfigurationFactory(type) {
    override fun createTemplateConfiguration(project: Project) =
        VyxRunConfig(project, this, "Vyx Run File")
    override fun getId() = "VYX_SINGLE"
    override fun getName() = "Run File"
    override fun getOptionsClass() = VyxRunOptions::class.java
}

class VyxProjectFactory(type: ConfigurationType) : ConfigurationFactory(type) {
    override fun createTemplateConfiguration(project: Project): RunConfiguration {
        val config = VyxRunConfig(project, this, "Vyx Build & Run")
        config.mode = VyxRunConfig.Mode.BUILD_AND_RUN
        return config
    }
    override fun getId() = "VYX_PROJECT"
    override fun getName() = "Build & Run Project"
    override fun getOptionsClass() = VyxRunOptions::class.java
}

class VyxRunOptions : RunConfigurationOptions() {
    var scriptPath by string("")
    var arguments by string("")
    var workDir by string("")
    var envVars by string("")
    var targetName by string("")
    var mode by string("RUN_FILE")
}

class VyxRunConfig(project: Project, factory: ConfigurationFactory, name: String)
    : RunConfigurationBase<VyxRunOptions>(project, factory, name) {

    enum class Mode {
        /** Compile + run a single .vyx via --src=file --run=aot */
        RUN_FILE,
        /** Project build then launch out/<name>.exe (or --src=project --run=aot) */
        BUILD_AND_RUN,
        /** Only `vyxc build` */
        BUILD_ONLY,
    }

    var scriptPath: String
        get() = options.scriptPath ?: ""
        set(v) { options.scriptPath = v }

    var arguments: String
        get() = options.arguments ?: ""
        set(v) { options.arguments = v }

    var workDir: String
        get() = options.workDir?.ifBlank { project.basePath ?: "" } ?: (project.basePath ?: "")
        set(v) { options.workDir = v }

    var envVars: String
        get() = options.envVars ?: ""
        set(v) { options.envVars = v }

    var targetName: String
        get() = options.targetName ?: ""
        set(v) { options.targetName = v }

    var mode: Mode
        get() = try {
            Mode.valueOf(options.mode ?: "RUN_FILE")
        } catch (_: Exception) {
            Mode.RUN_FILE
        }
        set(v) { options.mode = v.name }

    public override fun getOptions(): VyxRunOptions = super.getOptions() as VyxRunOptions

    override fun getConfigurationEditor() = VyxEditor()

    override fun checkConfiguration() {
        val vyxc = VyxCli.findVyxc(project)
        if (mode == Mode.RUN_FILE && scriptPath.isBlank()) {
            throw RuntimeConfigurationError("Select a .vyx file to run")
        }
        if (mode == Mode.RUN_FILE && scriptPath.isNotBlank() && !File(scriptPath).exists()) {
            throw RuntimeConfigurationWarning("File not found: $scriptPath")
        }
        if (vyxc.contains('/') || vyxc.contains('\\')) {
            if (!File(vyxc).exists()) {
                throw RuntimeConfigurationWarning("vyxc not found: $vyxc (set under Settings → Vyx)")
            }
        }
    }

    override fun getState(executor: Executor, env: ExecutionEnvironment): RunProfileState {
        val isDebug = executor.id == DefaultDebugExecutor.EXECUTOR_ID
        if (isDebug) {
            // VyxDapLaunchArgumentsProvider performs the synchronous -g build
            // before IntelliJ starts its generic DAP runner.  The profile state
            // only supplies the console/process lifetime expected by DAP.
            return object : CommandLineState(env) {
                override fun startProcess(): com.intellij.execution.process.ProcessHandler =
                    NopProcessHandler()
            }
        }
        return object : CommandLineState(env) {
            override fun startProcess(): com.intellij.execution.process.ProcessHandler {
                val wd = File(if (workDir.isNotEmpty()) workDir else (project.basePath ?: "."))
                val cmd: GeneralCommandLine = when (mode) {
                    Mode.RUN_FILE -> {
                        VyxCli.runFile(project, scriptPath, debug = isDebug).also { c ->
                            if (workDir.isNotEmpty()) c.workDirectory = wd
                        }
                    }
                    Mode.BUILD_AND_RUN -> {
                        // Prefer single integrated project run so console shows compile+program output.
                        // For Debug we still use -g and then launch the produced exe if present after build,
                        // so DAP/native debug can attach later; for Run, --src=project --run=aot is enough.
                        if (isDebug) {
                            // build -g then run exe
                            val build = VyxCli.buildProject(project, wd, targetName, debug = true)
                            applyEnv(build)
                            val buildProc = build.createProcess()
                            val code = buildProc.waitFor()
                            if (code != 0) {
                                // re-run build with handler so user sees full log
                                val fail = VyxCli.buildProject(project, wd, targetName, debug = true)
                                applyEnv(fail)
                                val handler = OSProcessHandler(fail)
                                ProcessTerminatedListener.attach(handler)
                                return handler
                            }
                            val exe = VyxCli.resolveBuiltExe(wd, targetName)
                            GeneralCommandLine(exe.absolutePath).also { c -> c.workDirectory = wd }
                        } else {
                            VyxCli.buildAndRunProject(project, wd, debug = false)
                        }
                    }
                    Mode.BUILD_ONLY -> VyxCli.buildProject(project, wd, targetName, debug = isDebug)
                }

                if (arguments.isNotBlank() && mode != Mode.BUILD_ONLY) {
                    // Extra program args only make sense when launching an exe directly.
                    // For --run=aot path they go after the compiler args.
                    if (mode == Mode.RUN_FILE || (mode == Mode.BUILD_AND_RUN && !isDebug)) {
                        // leave as compiler-driven run; program args not forwarded by current CLI reliably
                    } else {
                        arguments.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }.forEach {
                            cmd.addParameter(it)
                        }
                    }
                }

                applyEnv(cmd)
                val handler = OSProcessHandler(cmd)
                ProcessTerminatedListener.attach(handler)
                return handler
            }
        }
    }

    internal fun debugExecutable(): File {
        val wd = File(if (workDir.isNotEmpty()) workDir else (project.basePath ?: "."))
        if (mode == Mode.RUN_FILE) {
            val name = File(scriptPath).nameWithoutExtension.ifBlank { "main" }
            return File(File(wd, ".idea/vyx-debug"),
                if (VyxToolPaths.isWindows()) "$name.exe" else name)
        }
        return VyxCli.resolveBuiltExe(wd, targetName)
    }

    internal fun debugBuildCommand(): GeneralCommandLine {
        val wd = File(if (workDir.isNotEmpty()) workDir else (project.basePath ?: "."))
        val command = when (mode) {
            Mode.RUN_FILE -> VyxCli.compileFileForDebug(project, scriptPath, debugExecutable())
            Mode.BUILD_AND_RUN, Mode.BUILD_ONLY ->
                VyxCli.buildProject(project, wd, targetName, debug = true)
        }
        applyEnv(command)
        return command
    }

    internal fun parsedArguments(): List<String> =
        arguments.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }

    internal fun environment(): Map<String, String> {
        val values = linkedMapOf<String, String>()
        if (envVars.isBlank()) return values
        envVars.split(";", "\n").forEach { kv ->
            val parts = kv.split("=", limit = 2)
            if (parts.size == 2) {
                values[parts[0].trim()] = parts[1].trim()
            }
        }
        return values
    }

    internal fun applyEnv(cmd: GeneralCommandLine) {
        cmd.environment.putAll(environment())
    }
}

class VyxEditor : SettingsEditor<VyxRunConfig>() {
    private val modeCombo = JComboBox(
        arrayOf(
            "Run File (vyxc --src=file --run=aot)",
            "Build & Run Project (vyxc --src=project --run=aot)",
            "Build Only (vyxc build)"
        )
    )
    private val scriptField = TextFieldWithBrowseButton()
    private val argsField = JTextField()
    private val workDirField = TextFieldWithBrowseButton()
    private val envField = JTextField()
    private val targetCombo = JComboBox<String>()
    private val resolvedVyxc = JLabel()

    override fun createEditor(): JComponent {
        val p = JPanel(GridBagLayout())
        val gbc = GridBagConstraints()
        gbc.fill = GridBagConstraints.HORIZONTAL
        gbc.weightx = 1.0
        gbc.insets = Insets(4, 4, 4, 4)

        scriptField.addBrowseFolderListener(
            "Select Vyx File", "Choose .vyx file", null,
            FileChooserDescriptorFactory.createSingleFileDescriptor("vyx")
        )
        workDirField.addBrowseFolderListener(
            "Working Directory", "Choose working directory", null,
            FileChooserDescriptorFactory.createSingleFolderDescriptor()
        )

        fun row(y: Int, label: String, comp: JComponent) {
            gbc.gridy = y; gbc.gridx = 0; gbc.weightx = 0.0
            p.add(JLabel(label), gbc)
            gbc.gridx = 1; gbc.weightx = 1.0
            p.add(comp, gbc)
        }

        row(0, "Mode:", modeCombo)
        row(1, "Script / file:", scriptField)
        row(2, "Arguments:", argsField)
        row(3, "Working dir:", workDirField)
        row(4, "Env vars (K=V;…):", envField)
        targetCombo.addItem("(auto)")
        row(5, "Build target:", targetCombo)
        gbc.gridy = 6; gbc.gridx = 0; gbc.gridwidth = 2
        resolvedVyxc.foreground = java.awt.Color.GRAY
        p.add(resolvedVyxc, gbc)
        return p
    }

    override fun applyEditorTo(c: VyxRunConfig) {
        c.mode = when (modeCombo.selectedIndex) {
            1 -> VyxRunConfig.Mode.BUILD_AND_RUN
            2 -> VyxRunConfig.Mode.BUILD_ONLY
            else -> VyxRunConfig.Mode.RUN_FILE
        }
        c.scriptPath = scriptField.text
        c.arguments = argsField.text
        c.workDir = workDirField.text
        c.envVars = envField.text
        c.targetName = targetCombo.selectedItem as? String ?: ""
        if (c.scriptPath.isNotEmpty() && (c.name == "Unnamed" || c.name.startsWith("Vyx "))) {
            c.name = File(c.scriptPath).nameWithoutExtension
        }
    }

    override fun resetEditorFrom(c: VyxRunConfig) {
        modeCombo.selectedIndex = when (c.mode) {
            VyxRunConfig.Mode.BUILD_AND_RUN -> 1
            VyxRunConfig.Mode.BUILD_ONLY -> 2
            else -> 0
        }
        scriptField.text = c.scriptPath
        argsField.text = c.arguments
        workDirField.text = c.workDir
        envField.text = c.envVars
        targetCombo.removeAllItems()
        targetCombo.addItem("(auto)")
        val wd = if (c.workDir.isNotEmpty()) c.workDir else (c.project.basePath ?: ".")
        val toml = File(wd, "Vyx.toml")
        if (toml.exists()) {
            for (line in toml.readLines()) {
                val m = Regex("""\[target\.([A-Za-z0-9_\-]+)]""").find(line)
                if (m != null) targetCombo.addItem(m.groupValues[1])
            }
        }
        if (c.targetName.isNotEmpty()) targetCombo.selectedItem = c.targetName
        resolvedVyxc.text = "vyxc = ${VyxCli.findVyxc(c.project)}"
    }
}
