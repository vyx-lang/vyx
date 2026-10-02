package com.vyx.plugin

/**
 * Conservative indent formatter used when LSP formatting is unavailable.
 * Keeps comments/strings intact; 4-space indent from brace/bracket/paren depth.
 */
object VyxIndent {
    fun format(src: String): String {
        var inBlock = false
        var inString = false
        var inChar = false
        var indent = 0
        val out = StringBuilder()
        var emitted = false
        val normalized = src.replace("\r\n", "\n").replace('\r', '\n')
        val lines = normalized.split('\n')
        for (raw in lines) {
            val trimmed = raw.trimEnd { it == ' ' || it == '\t' }
            val body = trimmed.trimStart { it == ' ' || it == '\t' }
            if (body.isEmpty()) {
                if (emitted) out.append('\n')
                emitted = true
                continue
            }
            val info = scanLine(body, inBlock, inString, inChar)
            inBlock = info.inBlock
            inString = info.inString
            inChar = info.inChar
            var level = indent - info.leadingClose
            if (level < 0) level = 0
            if (emitted) out.append('\n')
            repeat(level) { out.append("    ") }
            out.append(body)
            indent = (level + info.delta).coerceAtLeast(0)
            emitted = true
        }
        if (out.isEmpty() || out.last() != '\n') out.append('\n')
        return out.toString()
    }

    private data class Scan(
        val leadingClose: Int,
        val delta: Int,
        val inBlock: Boolean,
        val inString: Boolean,
        val inChar: Boolean,
    )

    private fun scanLine(
        line: String,
        inBlock0: Boolean,
        inString0: Boolean,
        inChar0: Boolean,
    ): Scan {
        var inBlock = inBlock0
        var inString = inString0
        var inChar = inChar0
        var leading = 0
        var delta = 0
        var seenCode = false
        var i = 0
        while (i < line.length) {
            val c = line[i]
            val nxt = if (i + 1 < line.length) line[i + 1] else '\u0000'
            when {
                inBlock -> {
                    if (c == '*' && nxt == '/') {
                        inBlock = false
                        i += 2
                    } else {
                        i++
                    }
                }
                inString -> {
                    if (c == '\\') i += 2 else {
                        if (c == '"') inString = false
                        i++
                    }
                }
                inChar -> {
                    if (c == '\\') i += 2 else {
                        if (c == '\'') inChar = false
                        i++
                    }
                }
                c == '/' && nxt == '/' -> break
                c == '/' && nxt == '*' -> {
                    inBlock = true
                    i += 2
                }
                c == '"' -> {
                    inString = true
                    i++
                }
                c == '\'' -> {
                    inChar = true
                    i++
                }
                c == ' ' || c == '\t' -> i++
                !seenCode && (c == '}' || c == ']' || c == ')') -> {
                    leading++
                    i++
                }
                else -> {
                    seenCode = true
                    when (c) {
                        '{', '[', '(' -> delta++
                        '}', ']', ')' -> delta--
                    }
                    i++
                }
            }
        }
        return Scan(leading, delta, inBlock, inString, inChar)
    }
}
