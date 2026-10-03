import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

export type ToolName = 'vyxc' | 'vyxc-lsp' | 'vyxc-dap';

export function exeSuffix(): string {
    return process.platform === 'win32' ? '.exe' : '';
}

export function fileExists(p: string): boolean {
    try {
        return fs.existsSync(p) && fs.statSync(p).isFile();
    } catch {
        return false;
    }
}

function existingFile(candidate: string): string | undefined {
    return fileExists(candidate) ? path.normalize(candidate) : undefined;
}

function looksLikePath(value: string): boolean {
    return path.isAbsolute(value) || value.includes('/') || value.includes('\\');
}

/** Walk workspace folders and parents for a self-host SDK layout. */
function projectSdk(baseName: string): string | undefined {
    const exe = `${baseName}${exeSuffix()}`;
    const folders = vscode.workspace.workspaceFolders ?? [];
    for (const folder of folders) {
        let root = folder.uri.fsPath;
        for (let i = 0; i < 8; i++) {
            const hits = [
                path.join(root, exe),
                path.join(root, 'bin', exe),
                path.join(root, 'toolchain', exe),
                path.join(root, 'out', exe),
                path.join(root, 'bootstrap_compiler', 'out', exe),
            ];
            for (const hit of hits) {
                const found = existingFile(hit);
                if (found) {
                    return found;
                }
            }
            const parent = path.dirname(root);
            if (parent === root) {
                break;
            }
            root = parent;
        }
    }
    return undefined;
}

function envOverride(baseName: ToolName): string | undefined {
    const keys =
        baseName === 'vyxc' ? ['VYX_COMPILER', 'ZYN_VYXC']
            : baseName === 'vyxc-lsp' ? ['VYX_LSP']
                : ['VYX_DAP'];
    for (const key of keys) {
        const raw = (process.env[key] ?? '').trim();
        if (!raw) {
            continue;
        }
        if (fileExists(raw)) {
            return path.normalize(raw);
        }
        if (!looksLikePath(raw)) {
            return raw;
        }
    }
    return undefined;
}

export function resolveLlvmRoot(extensionRoot?: string): string {
    const configured = vscode.workspace.getConfiguration('vyx').get<string>('llvmRoot', '').trim();
    if (configured && fs.existsSync(configured)) {
        return path.normalize(configured);
    }
    if (process.env.LLVM_ROOT && fs.existsSync(process.env.LLVM_ROOT)) {
        return path.normalize(process.env.LLVM_ROOT);
    }
    const folders = vscode.workspace.workspaceFolders ?? [];
    for (const folder of folders) {
        let root = folder.uri.fsPath;
        for (let i = 0; i < 8; i++) {
            const clang = path.join(root, 'clang');
            if (fs.existsSync(path.join(clang, 'bin'))) {
                return clang;
            }
            const parent = path.dirname(root);
            if (parent === root) {
                break;
            }
            root = parent;
        }
    }
    if (extensionRoot) {
        const sibling = path.join(extensionRoot, 'clang');
        if (fs.existsSync(path.join(sibling, 'bin'))) {
            return sibling;
        }
    }
    return process.env.LLVM_ROOT ?? '';
}

export function toolEnv(compilerOrLspPath: string, extensionRoot?: string): NodeJS.ProcessEnv {
    const llvmRoot = resolveLlvmRoot(extensionRoot);
    const dir = path.dirname(compilerOrLspPath);
    const llvmBin = llvmRoot ? path.join(llvmRoot, 'bin') : '';
    const parts = [dir, llvmBin, process.env.PATH ?? ''].filter(p => p.length > 0);
    const env: NodeJS.ProcessEnv = {
        ...process.env,
        PATH: parts.join(path.delimiter),
    };
    if (llvmRoot) {
        env.LLVM_ROOT = llvmRoot;
    }
    return env;
}

/**
 * Resolve order: setting → plugin sibling → workspace SDK → env → PATH name.
 */
export function resolveTool(
    configKey: 'compilerPath' | 'lspPath' | 'dapPath',
    baseName: ToolName,
    extensionRoot?: string,
    log?: (msg: string) => void,
): string {
    const cfg = vscode.workspace.getConfiguration('vyx');
    const configured = (cfg.get<string>(configKey, '') ?? '').trim();
    if (configured) {
        if (fileExists(configured)) {
            return path.normalize(configured);
        }
        if (!looksLikePath(configured)) {
            return configured;
        }
        log?.(`${configKey}="${configured}" not found; continuing`);
    }

    if (extensionRoot) {
        const sibling = existingFile(path.join(extensionRoot, `${baseName}${exeSuffix()}`));
        if (sibling) {
            return sibling;
        }
        const bin = existingFile(path.join(extensionRoot, 'bin', `${baseName}${exeSuffix()}`));
        if (bin) {
            return bin;
        }
        const toolchain = existingFile(path.join(extensionRoot, 'toolchain', `${baseName}${exeSuffix()}`));
        if (toolchain) {
            return toolchain;
        }
    }

    if (baseName !== 'vyxc') {
        const compiler = resolveTool('compilerPath', 'vyxc', extensionRoot);
        if (looksLikePath(compiler) && fileExists(compiler)) {
            const nextTo = existingFile(path.join(path.dirname(compiler), `${baseName}${exeSuffix()}`));
            if (nextTo) {
                return nextTo;
            }
        }
    }

    const sdk = projectSdk(baseName);
    if (sdk) {
        return sdk;
    }

    const fromEnv = envOverride(baseName);
    if (fromEnv) {
        return fromEnv;
    }

    return `${baseName}${exeSuffix()}`;
}
