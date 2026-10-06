import * as vscode from 'vscode';
import * as path from 'path';
import { spawn } from 'child_process';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions
} from 'vscode-languageclient/node';
import { exeSuffix, fileExists, resolveLlvmRoot, resolveTool, toolEnv } from './toolchain';

let client: LanguageClient | undefined;
let outputChannel: vscode.OutputChannel | undefined;
let statusItem: vscode.StatusBarItem | undefined;
let extensionRoot: string | undefined;

function log(message: string): void {
    if (!outputChannel) {
        outputChannel = vscode.window.createOutputChannel('Vyx');
    }
    outputChannel.appendLine(`[Vyx] ${message}`);
}

function getCompilerPath(): string {
    return resolveTool('compilerPath', 'vyxc', extensionRoot, log);
}

function getLspPath(): string {
    return resolveTool('lspPath', 'vyxc-lsp', extensionRoot, log);
}

function getDapPath(): string {
    return resolveTool('dapPath', 'vyxc-dap', extensionRoot, log);
}

function spawnEnv(toolPath: string): NodeJS.ProcessEnv {
    return toolEnv(toolPath, extensionRoot);
}

function setStatus(text: string, ok: boolean): void {
    if (!statusItem) {
        return;
    }
    statusItem.text = ok ? `$(check) ${text}` : `$(error) ${text}`;
    statusItem.tooltip = 'Vyx language server';
}

function createClient(): LanguageClient {
    if (!outputChannel) {
        outputChannel = vscode.window.createOutputChannel('Vyx');
    }
    const command = getLspPath();
    const firstWorkspace = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
    const serverOptions: ServerOptions = {
        command,
        args: ['--stdio'],
        options: {
            cwd: firstWorkspace,
            env: spawnEnv(command)
        }
    };
    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'vyx' },
            { scheme: 'untitled', language: 'vyx' }
        ],
        synchronize: {
            configurationSection: 'vyx',
            fileEvents: [
                vscode.workspace.createFileSystemWatcher('**/*.vyx'),
                vscode.workspace.createFileSystemWatcher('**/*.vyi'),
                vscode.workspace.createFileSystemWatcher('**/Vyx.toml')
            ]
        },
        outputChannel,
        traceOutputChannel: outputChannel,
        markdown: { isTrusted: false },
        diagnosticCollectionName: 'vyx',
        initializationOptions: {
            llvmRoot: resolveLlvmRoot(extensionRoot)
        }
    };
    return new LanguageClient('vyxLanguageServer', 'Vyx Language Server', serverOptions, clientOptions);
}

async function startLanguageServer(): Promise<void> {
    if (client) {
        return;
    }
    const lspPath = getLspPath();
    log(`Starting language server: ${lspPath}`);
    try {
        client = createClient();
        await client.start();
        log('Language server started.');
        setStatus('Vyx LSP', true);
    } catch (err) {
        client = undefined;
        const msg = err instanceof Error ? err.message : String(err);
        log(`Failed to start language server (${lspPath}): ${msg}`);
        setStatus('Vyx LSP off', false);
        void vscode.window.showErrorMessage(
            `Vyx language server failed to start (${lspPath}). Set vyx.lspPath, place vyxc-lsp next to the extension, or add it to PATH.`
        );
    }
}

async function stopLanguageServer(): Promise<void> {
    if (!client) {
        return;
    }
    const current = client;
    client = undefined;
    await current.stop();
    setStatus('Vyx LSP off', false);
}

async function restartLanguageServer(): Promise<void> {
    await stopLanguageServer();
    await startLanguageServer();
    vscode.window.showInformationMessage('Vyx language server restarted.');
}

async function ensureVyxLanguageModeForDocument(document: vscode.TextDocument): Promise<void> {
    if (document.languageId === 'vyx' || document.isUntitled) {
        return;
    }
    if (document.uri.scheme !== 'file') {
        return;
    }
    if (!(document.fileName.endsWith('.vyx') || document.fileName.endsWith('.vyi'))) {
        return;
    }
    await vscode.languages.setTextDocumentLanguage(document, 'vyx');
}

function quote(arg: string): string {
    if (process.platform === 'win32') {
        return `"${arg.replace(/"/g, '\\"')}"`;
    }
    return `'${arg.replace(/'/g, `'\\''`)}'`;
}

function sendToTerminal(name: string, line: string): void {
    const terminal = vscode.window.createTerminal({ name, env: spawnEnv(getCompilerPath()) });
    terminal.sendText(line);
    terminal.show();
}

async function runCompiler(args: string[], cwd: string): Promise<{ code: number; stdout: string; stderr: string }> {
    const compiler = getCompilerPath();
    return new Promise(resolve => {
        const child = spawn(compiler, args, {
            cwd,
            env: spawnEnv(compiler),
            windowsHide: true
        });
        let stdout = '';
        let stderr = '';
        child.stdout.on('data', (chunk: Buffer) => { stdout += chunk.toString(); });
        child.stderr.on('data', (chunk: Buffer) => { stderr += chunk.toString(); });
        child.on('error', err => {
            resolve({ code: -1, stdout, stderr: err.message });
        });
        child.on('close', code => {
            resolve({ code: code ?? -1, stdout, stderr });
        });
    });
}

function revealCompilerOutput(stdout: string, stderr: string): void {
    const text = `${stdout}\n${stderr}`.trim();
    if (!text) {
        return;
    }
    log(text);
    outputChannel?.show(true);
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    extensionRoot = context.extensionPath;
    outputChannel = vscode.window.createOutputChannel('Vyx');
    statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 80);
    statusItem.command = 'vyx.debugExtensionStatus';
    statusItem.show();
    log(`Activating ${context.extension.id}`);

    const setCompilerPath = vscode.commands.registerCommand('vyx.setCompilerPath', async () => {
        const current = vscode.workspace.getConfiguration('vyx').get<string>('compilerPath', '');
        const value = await vscode.window.showInputBox({
            title: 'Set Vyx compiler path',
            prompt: 'Absolute path to vyxc / boot, or a command on PATH',
            value: current || getCompilerPath(),
            ignoreFocusOut: true
        });
        if (value === undefined) {
            return;
        }
        await vscode.workspace.getConfiguration('vyx').update(
            'compilerPath',
            value.trim(),
            vscode.ConfigurationTarget.Workspace
        );
        vscode.window.showInformationMessage(`Vyx compiler path set to: ${value.trim() || '(auto)'}`);
    });

    const restartCommand = vscode.commands.registerCommand('vyx.restartLanguageServer', () => restartLanguageServer());

    const debugCommand = vscode.commands.registerCommand('vyx.debugExtensionStatus', async () => {
        const editor = vscode.window.activeTextEditor;
        const details = [
            `extension=${context.extension.id}`,
            `compilerPath=${getCompilerPath()}`,
            `lspPath=${getLspPath()}`,
            `dapPath=${getDapPath()}`,
            `llvmRoot=${resolveLlvmRoot(extensionRoot)}`,
            `activeEditor=${editor ? editor.document.fileName : 'none'}`,
            `languageId=${editor ? editor.document.languageId : 'none'}`,
            `lspRunning=${client ? 'yes' : 'no'}`
        ].join('\n');
        log(details.replace(/\n/g, ' | '));
        outputChannel?.show(true);
        void vscode.window.showInformationMessage(details);
    });

    const runFileCommand = vscode.commands.registerCommand('vyx.runFile', async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor || editor.document.languageId !== 'vyx') {
            return;
        }
        await editor.document.save();
        const file = editor.document.fileName;
        sendToTerminal('Vyx Run', `${quote(getCompilerPath())} --src=file ${quote(file)} --run=aot`);
    });

    const buildCommand = vscode.commands.registerCommand('vyx.buildProject', async () => {
        const folder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
        const cwd = folder ?? path.dirname(vscode.window.activeTextEditor?.document.fileName ?? '.');
        sendToTerminal('Vyx Build', `${quote(getCompilerPath())} --src=project ${quote(cwd)}`);
    });

    const testCommand = vscode.commands.registerCommand('vyx.testFile', async () => {
        sendToTerminal('Vyx Test', `${quote(getCompilerPath())} run test`);
    });

    const formatCommand = vscode.commands.registerCommand('vyx.formatFile', async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor || editor.document.languageId !== 'vyx') {
            return;
        }
        await vscode.commands.executeCommand('editor.action.formatDocument');
    });

    const docsCommand = vscode.commands.registerCommand('vyx.generateDocs', async () => {
        vscode.window.showInformationMessage('Current vyxc has no `doc` subcommand. Hover and document symbols come from vyxc-lsp.');
    });

    const debugAdapterFactory = vscode.debug.registerDebugAdapterDescriptorFactory('vyx', {
        createDebugAdapterDescriptor(session: vscode.DebugSession): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
            const dapPath = getDapPath();
            const env = spawnEnv(dapPath) as { [key: string]: string };
            const debuggerPath: unknown = session.configuration.debuggerPath;
            if (typeof debuggerPath === 'string' && debuggerPath.trim()) {
                // Existing configurations may select the CLI; use its sibling DAP.
                const selected = debuggerPath.trim();
                const name = path.basename(selected).toLowerCase();
                env.VYX_DAP_ADAPTER = name === 'lldb' || name === 'lldb.exe'
                    ? path.join(path.dirname(selected), name.endsWith('.exe') ? 'lldb-dap.exe' : 'lldb-dap')
                    : selected;
            }
            log(`Starting DAP: ${dapPath}`);
            return new vscode.DebugAdapterExecutable(dapPath, [], {
                cwd: vscode.workspace.workspaceFolders?.[0]?.uri.fsPath,
                env
            });
        }
    });

    const debugConfigProvider = vscode.debug.registerDebugConfigurationProvider('vyx', {
        resolveDebugConfiguration(
            folder: vscode.WorkspaceFolder | undefined,
            config: vscode.DebugConfiguration,
        ): vscode.ProviderResult<vscode.DebugConfiguration> {
            if (!config.type && !config.request && !config.name) {
                const editor = vscode.window.activeTextEditor;
                if (editor && editor.document.languageId === 'vyx') {
                    config.type = 'vyx';
                    config.request = 'launch';
                    config.name = 'Debug Vyx Program';
                    const srcFile = editor.document.fileName;
                    config.program = srcFile.replace(/\.vyx$/, exeSuffix());
                    config.cwd = path.dirname(srcFile);
                }
            }
            if (!config.program) {
                vscode.window.showErrorMessage('Cannot start debugging: set "program" to the compiled executable.');
                return undefined;
            }
            if (!config.cwd) {
                config.cwd = folder?.uri.fsPath;
            }
            return config;
        },
        provideDebugConfigurations(): vscode.ProviderResult<vscode.DebugConfiguration[]> {
            return [{
                type: 'vyx',
                request: 'launch',
                name: 'Vyx: Debug current file',
                program: '${fileDirname}/${fileBasenameNoExtension}' + exeSuffix(),
                cwd: '${fileDirname}',
                preLaunchTask: 'vyx: compile current file (debug)'
            }];
        }
    });

    const debugFileCommand = vscode.commands.registerCommand('vyx.debugFile', async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor || editor.document.languageId !== 'vyx') {
            vscode.window.showErrorMessage('Open a .vyx file to debug.');
            return;
        }
        await editor.document.save();
        const file = editor.document.fileName;
        const exeName = file.replace(/\.vyx$/, exeSuffix());
        log(`Compile for debug: ${getCompilerPath()} -g -O0 --src=file --emit=exe -o ${exeName}`);
        vscode.window.setStatusBarMessage('Vyx: compiling with debug info…', 5000);
        const result = await runCompiler(
            ['-g', '-O0', '--src=file', file, '--emit=exe', '-o', exeName],
            path.dirname(file)
        );
        if (result.code !== 0 || !fileExists(exeName)) {
            revealCompilerOutput(result.stdout, result.stderr);
            vscode.window.showErrorMessage(`Debug build failed (exit ${result.code}). See Vyx output.`);
            return;
        }
        await vscode.debug.startDebugging(undefined, {
            type: 'vyx',
            request: 'launch',
            name: `Debug ${path.basename(file)}`,
            program: exeName,
            cwd: path.dirname(file)
        });
    });

    const taskProvider = vscode.tasks.registerTaskProvider('vyx', {
        provideTasks(): vscode.Task[] {
            const compiler = getCompilerPath();
            const folder = vscode.workspace.workspaceFolders?.[0];
            const def = (name: string, args: string[], matcher?: string): vscode.Task => {
                const task = new vscode.Task(
                    { type: 'vyx', command: args[0] ?? name },
                    folder ?? vscode.TaskScope.Workspace,
                    name,
                    'vyx',
                    new vscode.ShellExecution(compiler, args, { env: spawnEnv(compiler) as { [k: string]: string } }),
                    matcher ?? '$vyx'
                );
                task.group = name.includes('build') || name.includes('compile')
                    ? vscode.TaskGroup.Build
                    : undefined;
                return task;
            };
            return [
                def('build project', ['--src=project', '${workspaceFolder}']),
                def('compile current file', ['--src=file', '${file}', '--emit=exe']),
                def('compile current file (debug)', ['-g', '-O0', '--src=file', '${file}', '--emit=exe', '-o', '${fileDirname}/${fileBasenameNoExtension}' + exeSuffix()]),
                def('run current file', ['--src=file', '${file}', '--run=aot'])
            ];
        },
        resolveTask(task: vscode.Task): vscode.Task {
            return task;
        }
    });

    const configListener = vscode.workspace.onDidChangeConfiguration(async event => {
        if (event.affectsConfiguration('vyx')) {
            await restartLanguageServer();
        }
    });
    const openListener = vscode.workspace.onDidOpenTextDocument(doc => ensureVyxLanguageModeForDocument(doc));
    const changeEditorListener = vscode.window.onDidChangeActiveTextEditor(async editor => {
        if (editor) {
            await ensureVyxLanguageModeForDocument(editor.document);
        }
    });

    context.subscriptions.push(
        setCompilerPath, restartCommand, debugCommand, runFileCommand, buildCommand,
        testCommand, formatCommand, docsCommand, debugAdapterFactory, debugConfigProvider,
        debugFileCommand, taskProvider, configListener, openListener, changeEditorListener
    );
    if (statusItem) {
        context.subscriptions.push(statusItem);
    }
    if (outputChannel) {
        context.subscriptions.push(outputChannel);
    }

    const editor = vscode.window.activeTextEditor;
    if (editor) {
        await ensureVyxLanguageModeForDocument(editor.document);
    }
    await startLanguageServer();
}

export async function deactivate(): Promise<void> {
    await stopLanguageServer();
}
