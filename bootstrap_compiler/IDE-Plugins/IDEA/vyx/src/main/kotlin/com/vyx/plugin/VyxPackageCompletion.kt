package com.vyx.plugin

import com.intellij.openapi.project.Project
import java.io.File

/** Offline `use` / `import` package-registry completion (prefix match). */
object VyxPackageCompletion {
    data class UsePath(
        val path: String,
        val startCol: Int,
        val colonStyle: Boolean,
    )

    @Volatile
    private var cached: List<String> = emptyList()

    @Volatile
    private var cachedAt: Long = 0

    fun usePathAt(line: String, col: Int): UsePath? {
        val lim = col.coerceIn(0, line.length)
        var i = 0
        while (i < line.length && (line[i] == ' ' || line[i] == '\t')) i++
        if (i + 1 < line.length && line[i] == '/' && line[i + 1] == '/') return null
        if (line.startsWith("public ", i)) {
            i += 7
            while (i < line.length && (line[i] == ' ' || line[i] == '\t')) i++
        }
        val kw = when {
            line.startsWith("use ", i) -> 4
            line.startsWith("import ", i) -> 7
            else -> return null
        }
        i += kw
        while (i < line.length && (line[i] == ' ' || line[i] == '\t')) i++
        val identStart = i
        while (i < line.length && isIdentChar(line[i])) i++
        var after = i
        while (after < line.length && (line[after] == ' ' || line[after] == '\t')) after++
        if (after < line.length && line[after] == '=') {
            i = after + 1
            while (i < line.length && (line[i] == ' ' || line[i] == '\t')) i++
        } else {
            i = identStart
        }
        if (lim < i) return null
        var j = i
        while (j < lim) {
            val c = line[j]
            if (isIdentChar(c) || c == '.' || c == ':') j++ else break
        }
        for (k in j until lim) {
            if (line[k] == ';') return null
        }
        var afterPath = j
        while (afterPath < lim && (line[afterPath] == ' ' || line[afterPath] == '\t')) afterPath++
        if (afterPath + 3 <= lim && line.startsWith("as ", afterPath)) return null
        val raw = line.substring(i, lim)
        return UsePath(normalize(raw), i, raw.contains(':'))
    }

    fun prefixMatches(packages: Iterable<String>, prefix: String): List<String> {
        val out = LinkedHashSet<String>()
        for (pkg in packages) {
            addCandidate(out, pkg, prefix)
            var d = 0
            while (d < pkg.length) {
                if (pkg[d] == '.') addCandidate(out, pkg.substring(0, d), prefix)
                d++
            }
        }
        return out.toList()
    }

    fun packages(project: Project?): List<String> {
        val now = System.currentTimeMillis()
        if (cached.isNotEmpty() && now - cachedAt < 8000) return cached
        val found = LinkedHashSet<String>()
        for (root in packageRoots(project)) {
            scanTree(root, found, 0)
        }
        val list = found.toList()
        cached = list
        cachedAt = now
        return list
    }

    fun rewriteSeps(name: String, colonStyle: Boolean): String =
        if (colonStyle) name.replace(".", "::") else name

    internal fun normalize(raw: String): String {
        val out = StringBuilder()
        var lastSep = true
        for (c in raw) {
            if (c == '.' || c == ':') {
                if (!lastSep && out.isNotEmpty()) {
                    out.append('.')
                    lastSep = true
                }
            } else if (c != ' ' && c != '\t') {
                out.append(c)
                lastSep = false
            }
        }
        if (raw.isNotEmpty()) {
            val last = raw.last()
            if ((last == '.' || last == ':') && (out.isEmpty() || out.last() != '.')) {
                out.append('.')
            }
        }
        return out.toString()
    }

    internal fun moduleNameFromText(content: String): String {
        for (rawLine in content.lineSequence()) {
            val line = rawLine.trimStart()
            if (line.startsWith("//")) continue
            if (line.startsWith("@")) continue
            if (!line.startsWith("module")) return ""
            if (line.length > 6 && line[6] != ' ' && line[6] != '\t') return ""
            var p = 6
            while (p < line.length && (line[p] == ' ' || line[p] == '\t')) p++
            var end = p
            while (end < line.length) {
                val c = line[end]
                if (c == ';' || c == '{' || c == ' ' || c == '\t' || c == '@') break
                if (!isIdentChar(c) && c != '.') break
                end++
            }
            return if (end > p) line.substring(p, end) else ""
        }
        return ""
    }

    private fun addCandidate(out: MutableSet<String>, name: String, prefix: String) {
        if (name.isEmpty() || name.startsWith("__")) return
        if (prefix.isNotEmpty() && !name.startsWith(prefix)) return
        out.add(name)
    }

    private fun isIdentChar(c: Char): Boolean =
        c.isLetterOrDigit() || c == '_'

    private fun packageRoots(project: Project?): List<File> {
        val dirs = ArrayList<File>()
        System.getenv("VYX_STD_PACKAGES")?.trim()?.takeIf { it.isNotEmpty() }?.let { dirs += File(it) }
        val compiler = VyxToolPaths.resolveForProject(project, "vyxc")
        val exe = File(compiler).absoluteFile
        exe.parentFile?.let { out ->
            dirs += File(out, "std_packages")
            out.parentFile?.let { boot ->
                dirs += File(boot, "std_packages")
                dirs += File(boot, "std")
            }
        }
        var root = project?.basePath?.let(::File)?.absoluteFile
        var depth = 0
        while (root != null && depth < 6) {
            dirs += File(root, "std_packages")
            dirs += File(root, "std")
            dirs += File(root, "bootstrap_compiler/std_packages")
            dirs += File(root, "bootstrap_compiler/std")
            root = root.parentFile
            depth++
        }
        return dirs.filter { it.isDirectory }.distinctBy { it.canonicalPath }
    }

    private fun scanTree(dir: File, out: MutableSet<String>, depth: Int) {
        if (depth > 10 || !dir.isDirectory) return
        val children = dir.listFiles() ?: return
        for (child in children) {
            if (child.isDirectory) {
                if (child.name in SKIP_DIRS) continue
                scanTree(child, out, depth + 1)
            } else if (child.name.endsWith(".vyx") || child.name.endsWith(".vyi")) {
                val name = runCatching { moduleNameFromText(child.readText()) }.getOrDefault("")
                if (name.isNotEmpty() && !name.startsWith("__")) out.add(name)
            }
        }
    }

    private val SKIP_DIRS = setOf(
        ".git", ".cache", "node_modules", "target", ".idea", ".vs",
        "vendor", "out", "cmake-build-debug", "cmake-build-release",
    )
}
