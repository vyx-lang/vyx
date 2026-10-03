package com.vyx.plugin

import com.intellij.openapi.editor.Document
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.execution.ExecutionException
import com.intellij.execution.ExecutionResult
import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.execution.configurations.RunProfile
import com.intellij.execution.executors.DefaultDebugExecutor
import com.intellij.execution.process.CapturingProcessHandler
import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.platform.dap.DapBreakpointsDescription
import com.intellij.platform.dap.DapLaunchArgumentsProvider
import com.intellij.platform.dap.DapStartRequest
import com.intellij.platform.dap.DebugAdapterDescriptor
import com.intellij.platform.dap.DebugAdapterId
import com.intellij.platform.dap.DapCommandProcessor
import com.intellij.platform.dap.DapStackFrame
import com.intellij.platform.dap.DapScope
import com.intellij.platform.dap.DapVariable
import com.intellij.platform.dap.DapThread
import com.intellij.platform.dap.DebugAdapterSupportProvider
import com.intellij.platform.dap.LaunchRequestArguments
import com.intellij.platform.dap.connection.CommandLineDebugAdapterHandle
import com.intellij.platform.dap.connection.DebugAdapterHandle
import com.intellij.platform.dap.xdebugger.DapXDebugProcess
import com.intellij.platform.dap.xdebugger.DapXDebuggerPresentationFactory
import com.intellij.platform.dap.xdebugger.DapXSuspendContext
import com.intellij.platform.dap.xdebugger.DefaultDapXDebuggerEvaluator
import com.intellij.platform.dap.xdebugger.DefaultDapXDebuggerPresentationFactory
import com.intellij.platform.dap.xdebugger.DefaultDapXExecutionStack
import com.intellij.platform.dap.xdebugger.DefaultDapXSuspendContext
import com.intellij.platform.dap.xdebugger.DefaultDapXScope
import com.intellij.platform.dap.xdebugger.DefaultDapXValue
import com.intellij.platform.dap.xdebugger.DefaultDapXStackFrame
import com.intellij.psi.PsiDocumentManager
import com.intellij.xdebugger.breakpoints.XBreakpoint
import com.intellij.xdebugger.breakpoints.XBreakpointProperties
import com.intellij.xdebugger.breakpoints.XBreakpointType
import com.intellij.xdebugger.breakpoints.XLineBreakpointType
import com.intellij.xdebugger.XDebuggerUtil
import com.intellij.xdebugger.XSourcePosition
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import com.intellij.openapi.util.TextRange
import com.intellij.xdebugger.frame.XInlineDebuggerDataCallback
import com.intellij.xdebugger.XDebugSession
import com.intellij.xdebugger.evaluation.ExpressionInfo
import com.intellij.xdebugger.evaluation.XDebuggerEvaluator
import com.intellij.xdebugger.frame.XStackFrame
import kotlinx.coroutines.CoroutineScope

object VyxDebugAdapterId : DebugAdapterId("vyx", "Vyx")

class VyxLineBreakpointProperties :
    XBreakpointProperties<VyxLineBreakpointProperties.State>() {
    class State {
        var enabled: Boolean = true
    }

    private var value = State()
    override fun getState(): State = value
    override fun loadState(state: State) {
        value = state
    }
}

class VyxLineBreakpointType :
    XLineBreakpointType<VyxLineBreakpointProperties>("vyx-line", "Vyx Line Breakpoints") {
    override fun canPutAt(file: VirtualFile, line: Int, project: Project): Boolean =
        file.extension == "vyx" || file.extension == "vyi"

    override fun createBreakpointProperties(file: VirtualFile, line: Int) =
        VyxLineBreakpointProperties()
}

class VyxExceptionBreakpointType :
    XBreakpointType<XBreakpoint<VyxLineBreakpointProperties>, VyxLineBreakpointProperties>(
        "vyx-exception",
        "Vyx Exception Breakpoints",
    ) {
    override fun createProperties() = VyxLineBreakpointProperties()

    override fun getDisplayText(breakpoint: XBreakpoint<VyxLineBreakpointProperties>): String =
        "Vyx exception"
}

class VyxDebugAdapterSupportProvider : DebugAdapterSupportProvider<VyxDebugAdapterId> {
    override val adapterId: VyxDebugAdapterId = VyxDebugAdapterId

    override fun createDebugAdapterDescriptor(project: Project): DebugAdapterDescriptor<VyxDebugAdapterId> =
        VyxDebugAdapterDescriptor(project)
}

private class VyxDebugAdapterDescriptor(private val project: Project) :
    DebugAdapterDescriptor<VyxDebugAdapterId>() {
    override val id: VyxDebugAdapterId = VyxDebugAdapterId

    override suspend fun launchDebugAdapter(
        environment: ExecutionEnvironment,
        executionResult: ExecutionResult?,
        sessionId: String,
    ): DebugAdapterHandle {
        val config = environment.runProfile as? VyxRunConfig
        if (config != null) {
            val output = withContext(Dispatchers.IO) {
                CapturingProcessHandler(config.debugBuildCommand()).runProcess()
            }
            if (output.exitCode != 0) {
                val message = (output.stderr + "\n" + output.stdout).trim()
                throw ExecutionException(if (message.isEmpty()) "Vyx debug build failed" else message)
            }
            val program = config.debugExecutable().absoluteFile
            if (!program.isFile) {
                throw ExecutionException("Vyx debug executable was not produced: ${program.path}")
            }
        }

        val executable = resolveAdapterExecutable(project)
        val debugger = resolveDebuggerExecutable()
        val command = GeneralCommandLine(executable)
            .withWorkDirectory(project.basePath)
        configureLldbRuntime(command, executable)
        if (debugger != null) command.environment["VYX_LLDB"] = debugger
        command.environment["VYX_DAP_TRACE"] = "1"
        return CommandLineDebugAdapterHandle(command)
    }

    override val breakpointsDescription = DapBreakpointsDescription(
        VyxLineBreakpointType::class.java,
        VyxExceptionBreakpointType::class.java,
    )

    override fun createXDebugProcess(
        session: XDebugSession,
        dapDebugSession: com.intellij.platform.dap.DapDebugSession,
        xDebugProcessScope: CoroutineScope,
        globalScope: CoroutineScope,
        debugAdapterDescriptor: DebugAdapterDescriptor<*>,
        executionEnvironment: ExecutionEnvironment,
        executionResult: ExecutionResult?,
        startRequestType: DapStartRequest,
        startRequestArguments: Map<String, Any?>,
    ): DapXDebugProcess = VyxDapXDebugProcess(
        session,
        dapDebugSession,
        xDebugProcessScope,
        globalScope,
        debugAdapterDescriptor,
        executionEnvironment,
        executionResult,
        startRequestType,
        startRequestArguments,
    )

    private fun resolveAdapterExecutable(project: Project): String {
        return VyxToolPaths.resolveVyxDapForProject(project)
    }

    private fun existingExecutable(path: String): String? =
        File(path).takeIf { it.isFile }?.absoluteFile?.normalize()?.path

    private fun resolveDebuggerExecutable(): String? =
        sequenceOf(
            System.getenv("VYX_LLDB"),
            System.getenv("LLVM_ROOT")?.let { root ->
                File(root, if (VyxToolPaths.isWindows()) "bin/lldb.exe" else "bin/lldb").path
            },
            VyxToolPaths.findOnPath("lldb"),
        ).mapNotNull { candidate -> candidate?.let(::existingExecutable) }
            .firstOrNull()
}

private fun configureLldbRuntime(command: GeneralCommandLine, executable: String) {
    val adapterName = File(executable).nameWithoutExtension
    if (!VyxToolPaths.isWindows() ||
        (!adapterName.equals("vyxc-dap", true) && !adapterName.equals("lldb-dap", true))) {
        return
    }
    val pythonHome = findLldbPythonHome() ?: return
    command.environment["PYTHONHOME"] = pythonHome.path
    val path = command.environment["PATH"] ?: System.getenv("PATH").orEmpty()
    val entries = path.split(File.pathSeparatorChar)
    if (entries.none { it.trim().trim('"').equals(pythonHome.path, true) }) {
        command.environment["PATH"] = pythonHome.path + File.pathSeparator + path
    }
}

private fun findLldbPythonHome(): File? {
    fun containsRuntime(directory: File?): Boolean =
        directory?.isDirectory == true &&
            directory.listFiles()?.any {
                it.isFile && Regex("python3[0-9]{2}\\.dll", RegexOption.IGNORE_CASE).matches(it.name)
            } == true

    val environmentHome = System.getenv("PYTHONHOME")?.let(::File)
    if (containsRuntime(environmentHome)) return environmentHome!!.absoluteFile

    val python = VyxToolPaths.findOnPath("python") ?: VyxToolPaths.findOnPath("python3")
    if (python != null) {
        val home = runCatching {
            val output = CapturingProcessHandler(
                GeneralCommandLine(python, "-c", "import sys; print(sys.base_prefix)")
            ).runProcess(3000)
            if (output.exitCode == 0 && !output.isTimeout) File(output.stdout.trim()) else null
        }.getOrNull()
        if (containsRuntime(home)) return home!!.absoluteFile
    }

    val roots = listOfNotNull(
        System.getenv("APPDATA")?.let { File(it, "uv/python") },
        System.getenv("LOCALAPPDATA")?.let { File(it, "Programs/Python") },
    )
    for (root in roots) {
        if (!root.isDirectory) continue
        val home = root.listFiles()?.firstOrNull(::containsRuntime)
        if (home != null) return home.absoluteFile
    }
    return null
}

class VyxDapLaunchArgumentsProvider : DapLaunchArgumentsProvider {
    override fun isApplicable(executorId: String, profile: RunProfile): Boolean =
        executorId == DefaultDebugExecutor.EXECUTOR_ID &&
            profile is VyxRunConfig && profile.mode != VyxRunConfig.Mode.BUILD_ONLY

    override fun getLaunchArguments(project: Project, profile: RunProfile): LaunchRequestArguments {
        val config = profile as VyxRunConfig
        val program = config.debugExecutable().absoluteFile
        val cwd = File(config.workDir.ifBlank { project.basePath ?: "." }).absolutePath
        val arguments = linkedMapOf<String, Any>(
            "program" to program.path,
            "cwd" to cwd,
            "args" to config.parsedArguments(),
            "env" to config.environment(),
            "stopOnEntry" to false,
        )
        return LaunchRequestArguments(VyxDebugAdapterId, DapStartRequest.Launch, arguments)
    }
}

private class VyxDapXDebugProcess(
    session: XDebugSession,
    dapDebugSession: com.intellij.platform.dap.DapDebugSession,
    xDebugProcessScope: CoroutineScope,
    globalScope: CoroutineScope,
    debugAdapterDescriptor: DebugAdapterDescriptor<*>,
    executionEnvironment: ExecutionEnvironment,
    executionResult: ExecutionResult?,
    startRequestType: DapStartRequest,
    startRequestArguments: Map<String, Any?>,
) : DapXDebugProcess(
    session,
    dapDebugSession,
    xDebugProcessScope,
    globalScope,
    debugAdapterDescriptor,
    executionEnvironment,
    executionResult,
    startRequestType,
    startRequestArguments,
) {
    init {
        val field = DapXDebugProcess::class.java.getDeclaredField("presentationFactory")
        field.isAccessible = true
        field.set(this, VyxDapPresentationFactory())
    }

    override fun getEvaluator(): XDebuggerEvaluator {
        val processor = dapDebugSession.commandProcessor
        val delegate = DefaultDapXDebuggerEvaluator(processor, presentationFactory, dapDebugSession)
        return VyxDapXDebuggerEvaluator(delegate)
    }
}

private class VyxDapPresentationFactory : DapXDebuggerPresentationFactory {
    private val delegate = DefaultDapXDebuggerPresentationFactory()

    override fun createSuspendContext(
        commandProcessor: DapCommandProcessor,
        threads: List<DapThread>,
        activeThread: DapThread?,
    ): DapXSuspendContext = DefaultDapXSuspendContext(this, commandProcessor, threads, activeThread)

    override fun createExecutionStack(
        commandProcessor: DapCommandProcessor,
        thread: DapThread,
        isActive: Boolean,
    ): com.intellij.xdebugger.frame.XExecutionStack =
        DefaultDapXExecutionStack(this, commandProcessor, thread, isActive)

    override fun createStackFrame(
        commandProcessor: DapCommandProcessor,
        thread: DapThread,
        frame: DapStackFrame,
    ): XStackFrame = DefaultDapXStackFrame(this, commandProcessor, thread, frame).let { delegate ->
        VyxDapXStackFrame(delegate, delegate.sourcePosition)
    }

    override fun createScope(
        commandProcessor: DapCommandProcessor,
        scope: DapScope,
        index: Int,
    ): com.intellij.xdebugger.frame.XValueGroup =
        VyxDapXScope(this, commandProcessor, scope, index)

    override fun createValue(
        commandProcessor: DapCommandProcessor,
        variable: DapVariable,
        icon: javax.swing.Icon?,
    ): com.intellij.xdebugger.frame.XNamedValue =
        VyxDapXValue(delegate.createValue(commandProcessor, variable, icon) as DefaultDapXValue, null)
}

private class VyxDapXScope(
    factory: DapXDebuggerPresentationFactory,
    commandProcessor: DapCommandProcessor,
    scope: DapScope,
    index: Int,
) : com.intellij.xdebugger.frame.XValueGroup(scope.name) {
    private val position = vyxSourcePosition(scope.frame)
    private val delegate = DefaultDapXScope(factory, commandProcessor, scope, index)

    override fun computeChildren(node: com.intellij.xdebugger.frame.XCompositeNode) {
        val wrapped = object : com.intellij.xdebugger.frame.XCompositeNode by node {
            override fun addChildren(children: com.intellij.xdebugger.frame.XValueChildrenList, last: Boolean) {
                node.addChildren(vyxWrapValues(children, position), last)
            }
        }
        delegate.computeChildren(wrapped)
    }

    override fun isAutoExpand() = delegate.isAutoExpand
}

private class VyxDapXValue(
    private val delegate: DefaultDapXValue,
    private val position: XSourcePosition?,
) : com.intellij.xdebugger.frame.XNamedValue(delegate.name) {
    override fun computePresentation(
        node: com.intellij.xdebugger.frame.XValueNode,
        place: com.intellij.xdebugger.frame.XValuePlace,
    ) = delegate.computePresentation(node, place)

    override fun computeChildren(node: com.intellij.xdebugger.frame.XCompositeNode) {
        val wrapped = object : com.intellij.xdebugger.frame.XCompositeNode by node {
            override fun addChildren(children: com.intellij.xdebugger.frame.XValueChildrenList, last: Boolean) {
                node.addChildren(vyxWrapValues(children, position), last)
            }
        }
        delegate.computeChildren(wrapped)
    }

    override fun canNavigateToSource() = delegate.canNavigateToSource()

    override fun getEvaluationExpression() = delegate.evaluationExpression

    override fun computeInlineDebuggerData(callback: XInlineDebuggerDataCallback): com.intellij.util.ThreeState {
        val project = com.intellij.openapi.project.ProjectManager.getInstance().openProjects.firstOrNull()
        val session = project?.let { com.intellij.xdebugger.XDebuggerManager.getInstance(it).currentSession }
        val source = position ?: session?.currentStackFrame?.sourcePosition
        vyxInlinePositions(delegate.name, source).forEach(callback::computed)
        return com.intellij.util.ThreeState.YES
    }

    override fun computeSourcePosition(navigatable: com.intellij.xdebugger.frame.XNavigatable) {
        val positions = vyxInlinePositions(delegate.name, position)
        if (positions.isEmpty()) navigatable.setSourcePosition(null)
        else positions.forEach(navigatable::setSourcePosition)
    }
}

private class VyxDapXStackFrame(
    private val delegate: DefaultDapXStackFrame,
    private val position: XSourcePosition?,
) : XStackFrame() {
    override fun getEvaluator(): XDebuggerEvaluator = VyxDapXDebuggerEvaluator(
        DefaultDapXDebuggerEvaluator(delegate.commandProcessor, delegate.factory, delegate.frame),
    )

    override fun computeChildren(node: com.intellij.xdebugger.frame.XCompositeNode) {
        val wrapped = object : com.intellij.xdebugger.frame.XCompositeNode by node {
            override fun addChildren(children: com.intellij.xdebugger.frame.XValueChildrenList, last: Boolean) {
                node.addChildren(vyxWrapValues(children, position), last)
            }
        }
        delegate.computeChildren(wrapped)
    }

    override fun customizePresentation(component: com.intellij.ui.ColoredTextContainer) =
        delegate.customizePresentation(component)

    override fun getSourcePosition() = delegate.sourcePosition

    override fun getEqualityObject(): Any? = delegate.equalityObject
}

private class VyxDapXDebuggerEvaluator(
    private val delegate: DefaultDapXDebuggerEvaluator,
) : XDebuggerEvaluator() {
    override fun evaluate(
        expression: String,
        callback: XEvaluationCallback,
        expressionPosition: com.intellij.xdebugger.XSourcePosition?,
    ) {
        delegate.evaluate(expression, callback, expressionPosition)
    }

    override fun getExpressionRangeAtOffset(
        project: Project,
        document: Document,
        offset: Int,
        sideEffectsAllowed: Boolean,
    ): TextRange? {
        if (!isVyxDocument(project, document)) return null
        val text = document.immutableCharSequence
        if (offset < 0 || offset > text.length) return null
        val anchor = when {
            offset < text.length && isVyxIdentifierPart(text[offset]) -> offset
            offset > 0 && isVyxIdentifierPart(text[offset - 1]) -> offset - 1
            else -> return null
        }
        var start = anchor
        while (start > 0 && isVyxIdentifierPart(text[start - 1])) start--
        if (!isVyxIdentifierStart(text[start])) return null
        var end = anchor + 1
        while (end < text.length && isVyxIdentifierPart(text[end])) end++
        return TextRange(start, end)
    }

    override fun getExpressionInfoAtOffset(
        project: Project,
        document: Document,
        offset: Int,
        sideEffectsAllowed: Boolean,
    ): ExpressionInfo? {
        val range = getExpressionRangeAtOffset(project, document, offset, sideEffectsAllowed)
            ?: return null
        val expression = document.getText(range)
        return ExpressionInfo(range, expression, expression)
    }
}

private fun vyxWrapValues(
    children: com.intellij.xdebugger.frame.XValueChildrenList,
    position: XSourcePosition?,
): com.intellij.xdebugger.frame.XValueChildrenList {
    val wrapped = com.intellij.xdebugger.frame.XValueChildrenList()
    for (group in children.topGroups) wrapped.addTopGroup(group)
    for (value in children.topValues) {
        wrapped.addTopValue(if (value is DefaultDapXValue) VyxDapXValue(value, position) else value)
    }
    for (index in 0 until children.size()) {
        val child = children.getValue(index)
        wrapped.add(children.getName(index), if (child is DefaultDapXValue) VyxDapXValue(child, position) else child)
    }
    for (group in children.bottomGroups) wrapped.addBottomGroup(group)
    return wrapped
}

private fun vyxSourcePosition(frame: DapStackFrame): XSourcePosition? {
    val file = frame.source ?: return null
    val line = frame.startPosition.line - 1
    return if (line >= 0) XDebuggerUtil.getInstance().createPosition(file, line) else null
}

private fun vyxInlinePositions(
    name: String,
    framePosition: XSourcePosition?,
): List<XSourcePosition> {
    val file = framePosition?.file ?: return emptyList()
    val document = FileDocumentManager.getInstance().getDocument(file) ?: return emptyList()
    return vyxIdentifierLines(document, name).filter { it <= framePosition.line }.mapNotNull { line ->
        XDebuggerUtil.getInstance().createPosition(file, line)
    }
}

private fun vyxIdentifierOffset(document: Document, line: Int, name: String): Int? {
    val start = document.getLineStartOffset(line)
    val end = document.getLineEndOffset(line)
    val text = document.immutableCharSequence
    var index = start
    while (index <= end - name.length) {
        val matched = text.subSequence(index, index + name.length).toString() == name
        val leftOk = index == start || !isVyxIdentifierPart(text[index - 1])
        val right = index + name.length
        val rightOk = right == end || !isVyxIdentifierPart(text[right])
        if (matched && leftOk && rightOk) return index
        index++
    }
    return null
}

private fun vyxIdentifierLines(document: Document, name: String): List<Int> {
    if (name.isEmpty()) return emptyList()
    val lines = mutableListOf<Int>()
    for (line in 0 until document.lineCount) {
        if (vyxIdentifierOffset(document, line, name) != null) lines += line
    }
    return lines
}

private fun isVyxDocument(file: com.intellij.openapi.vfs.VirtualFile): Boolean =
    file.extension.equals("vyx", ignoreCase = true)

private fun isVyxDocument(project: Project, document: Document): Boolean {
    val file = PsiDocumentManager.getInstance(project).getPsiFile(document)
    return file is VyxFile ||
        file?.language == VyxLanguage.INSTANCE ||
        file?.virtualFile?.extension.equals("vyx", ignoreCase = true)
}

private fun isVyxIdentifierStart(ch: Char): Boolean = ch.isLetter() || ch == '_'

private fun isVyxIdentifierPart(ch: Char): Boolean = ch.isLetterOrDigit() || ch == '_'
