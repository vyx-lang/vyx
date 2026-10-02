package com.vyx.plugin

import com.intellij.lexer.LexerBase
import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType

object VyxTokenTypes {
    val KEYWORD = IElementType("VYX_KEYWORD", VyxLanguage.INSTANCE)
    val IDENTIFIER = IElementType("VYX_IDENTIFIER", VyxLanguage.INSTANCE)
    val FUNCTION_DECLARATION = IElementType("VYX_FUNCTION_DECLARATION", VyxLanguage.INSTANCE)
    val FUNCTION_CALL = IElementType("VYX_FUNCTION_CALL", VyxLanguage.INSTANCE)
    val METHOD = IElementType("VYX_METHOD", VyxLanguage.INSTANCE)
    val FIELD = IElementType("VYX_FIELD", VyxLanguage.INSTANCE)
    val PARAMETER = IElementType("VYX_PARAMETER", VyxLanguage.INSTANCE)
    val LOCAL_VARIABLE = IElementType("VYX_LOCAL_VARIABLE", VyxLanguage.INSTANCE)
    val CONSTANT = IElementType("VYX_CONSTANT", VyxLanguage.INSTANCE)
    val TYPE_DECLARATION = IElementType("VYX_TYPE_DECLARATION", VyxLanguage.INSTANCE)
    val TYPE_REFERENCE = IElementType("VYX_TYPE_REFERENCE", VyxLanguage.INSTANCE)
    val MODULE = IElementType("VYX_MODULE", VyxLanguage.INSTANCE)
    val NUMBER = IElementType("VYX_NUMBER", VyxLanguage.INSTANCE)
    val STRING = IElementType("VYX_STRING", VyxLanguage.INSTANCE)
    val COMMENT = IElementType("VYX_COMMENT", VyxLanguage.INSTANCE)
    val OPERATOR = IElementType("VYX_OPERATOR", VyxLanguage.INSTANCE)
    val TYPE = IElementType("VYX_TYPE", VyxLanguage.INSTANCE)
    val ATTRIBUTE = IElementType("VYX_ATTRIBUTE", VyxLanguage.INSTANCE)
    val LBRACE = IElementType("VYX_LBRACE", VyxLanguage.INSTANCE)
    val RBRACE = IElementType("VYX_RBRACE", VyxLanguage.INSTANCE)
    val LPAREN = IElementType("VYX_LPAREN", VyxLanguage.INSTANCE)
    val RPAREN = IElementType("VYX_RPAREN", VyxLanguage.INSTANCE)
    val LBRACKET = IElementType("VYX_LBRACKET", VyxLanguage.INSTANCE)
    val RBRACKET = IElementType("VYX_RBRACKET", VyxLanguage.INSTANCE)
    val SEMICOLON = IElementType("VYX_SEMICOLON", VyxLanguage.INSTANCE)
    val COMMA = IElementType("VYX_COMMA", VyxLanguage.INSTANCE)
    val DOT = IElementType("VYX_DOT", VyxLanguage.INSTANCE)
    val COLON = IElementType("VYX_COLON", VyxLanguage.INSTANCE)

    /**
     * Keep in sync with VSCode syntaxes/vyx.tmLanguage.json keywords /
     * modifiers / types / constants as far as a simple lexer can go.
     */
    val KEYWORDS = setOf(
        // control
        "if", "elif", "else", "while", "for", "foreach", "match", "case", "default",
        "return", "break", "continue", "yield", "defer", "await", "try", "catch", "throw",
        // declarations
        "fn", "let", "var", "struct", "class", "interface", "enum", "error",
        "module", "use", "import", "export", "extern", "type", "concept", "comptime",
        "bench", "task", "impl", "trait",
        // other
        "as", "in", "where", "requires", "override", "static_assert", "fail",
        "unsafe", "box", "ref", "new", "sizeof", "typeof",
        // modifiers
        "public", "private", "mut", "const", "static", "async",
        // constants / self
        "true", "false", "null", "self", "none", "Self"
    )

    val TYPES = setOf(
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
        "f32", "f64", "bool", "char", "string", "wstring",
        "isize", "usize", "rawptr", "void",
        "Vec", "Dict", "Set", "Stack", "Queue", "Option", "Result",
        "Ref", "Scope", "Weak", "Channel", "Mutex", "Event", "Delegate", "Buffer"
    )

    val TYPE_DECLARATION_KEYWORDS = setOf(
        "class", "struct", "enum", "interface", "trait", "concept", "type"
    )
}

class VyxLexer : LexerBase() {
    companion object {
        private val FN_WORD = Regex("(^|[^A-Za-z0-9_])fn([^A-Za-z0-9_]|$)")
        private val TYPE_BODY_WORD = Regex(
            "(^|[^A-Za-z0-9_])(class|struct|enum|interface|trait|concept)([^A-Za-z0-9_]|$)"
        )
        private val TWO_CHAR_OPERATORS = setOf(
            "->", "=>", "==", "!=", "<=", ">=", "&&", "||",
            "+=", "-=", "*=", "/=", "%=", "<<", ">>", "&=", "|=", "^="
        )
    }

    private var buffer: CharSequence = ""
    private var startOffset = 0
    private var endOffset = 0
    private var currentOffset = 0
    private var tokenType: IElementType? = null
    private var tokenStart = 0
    private var tokenEnd = 0
    private var declaredFunctions: Set<String> = emptySet()

    override fun start(buffer: CharSequence, startOffset: Int, endOffset: Int, initialState: Int) {
        this.buffer = buffer
        this.startOffset = startOffset
        this.endOffset = endOffset
        this.currentOffset = startOffset
        this.declaredFunctions = collectDeclaredFunctions(buffer)
        advance()
    }

    override fun getState(): Int = 0
    override fun getTokenType(): IElementType? = tokenType
    override fun getTokenStart(): Int = tokenStart
    override fun getTokenEnd(): Int = tokenEnd
    override fun getBufferSequence(): CharSequence = buffer
    override fun getBufferEnd(): Int = endOffset

    override fun advance() {
        if (currentOffset >= endOffset) {
            tokenType = null
            return
        }
        tokenStart = currentOffset
        val c = buffer[currentOffset]
        when {
            c == '\n' || c == '\r' || c == ' ' || c == '\t' -> {
                while (currentOffset < endOffset) {
                    val ch = buffer[currentOffset]
                    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') break
                    currentOffset++
                }
                tokenType = TokenType.WHITE_SPACE
            }
            c == '/' && currentOffset + 1 < endOffset && buffer[currentOffset + 1] == '/' -> {
                while (currentOffset < endOffset && buffer[currentOffset] != '\n') currentOffset++
                tokenType = VyxTokenTypes.COMMENT
            }
            c == '/' && currentOffset + 1 < endOffset && buffer[currentOffset + 1] == '*' -> {
                currentOffset += 2
                while (currentOffset + 1 < endOffset &&
                    !(buffer[currentOffset] == '*' && buffer[currentOffset + 1] == '/')
                ) {
                    currentOffset++
                }
                if (currentOffset + 1 < endOffset) currentOffset += 2
                tokenType = VyxTokenTypes.COMMENT
            }
            c == '"' -> {
                currentOffset++
                while (currentOffset < endOffset && buffer[currentOffset] != '"') {
                    if (buffer[currentOffset] == '\\' && currentOffset + 1 < endOffset) currentOffset++
                    currentOffset++
                }
                if (currentOffset < endOffset) currentOffset++
                tokenType = VyxTokenTypes.STRING
            }
            c == '\'' -> {
                currentOffset++
                while (currentOffset < endOffset && buffer[currentOffset] != '\'') {
                    if (buffer[currentOffset] == '\\' && currentOffset + 1 < endOffset) currentOffset++
                    currentOffset++
                }
                if (currentOffset < endOffset) currentOffset++
                tokenType = VyxTokenTypes.STRING
            }
            c == '@' -> {
                currentOffset++
                if (currentOffset >= endOffset || buffer[currentOffset] != '[') {
                    while (currentOffset < endOffset &&
                        (buffer[currentOffset].isLetterOrDigit() || buffer[currentOffset] == '_')
                    ) {
                        currentOffset++
                    }
                }
                tokenType = VyxTokenTypes.ATTRIBUTE
            }
            c in '0'..'9' -> {
                if (c == '0' && currentOffset + 1 < endOffset &&
                    (buffer[currentOffset + 1] == 'x' || buffer[currentOffset + 1] == 'X')
                ) {
                    currentOffset += 2
                    while (currentOffset < endOffset) {
                        val ch = buffer[currentOffset]
                        if (ch in '0'..'9' || ch in 'a'..'f' || ch in 'A'..'F') currentOffset++
                        else break
                    }
                } else {
                    while (currentOffset < endOffset &&
                        (buffer[currentOffset] in '0'..'9' || buffer[currentOffset] == '.')
                    ) {
                        currentOffset++
                    }
                }
                tokenType = VyxTokenTypes.NUMBER
            }
            c.isLetter() || c == '_' -> {
                while (currentOffset < endOffset) {
                    val ch = buffer[currentOffset]
                    if (!(ch.isLetterOrDigit() || ch == '_')) break
                    currentOffset++
                }
                val text = buffer.subSequence(tokenStart, currentOffset).toString()
                tokenType = when {
                    text in VyxTokenTypes.KEYWORDS -> VyxTokenTypes.KEYWORD
                    text in VyxTokenTypes.TYPES -> VyxTokenTypes.TYPE
                    else -> classifyIdentifier(text)
                }
            }
            c == '{' -> { currentOffset++; tokenType = VyxTokenTypes.LBRACE }
            c == '}' -> { currentOffset++; tokenType = VyxTokenTypes.RBRACE }
            c == '(' -> { currentOffset++; tokenType = VyxTokenTypes.LPAREN }
            c == ')' -> { currentOffset++; tokenType = VyxTokenTypes.RPAREN }
            c == '[' -> { currentOffset++; tokenType = VyxTokenTypes.LBRACKET }
            c == ']' -> { currentOffset++; tokenType = VyxTokenTypes.RBRACKET }
            c == ';' -> { currentOffset++; tokenType = VyxTokenTypes.SEMICOLON }
            c == ',' -> { currentOffset++; tokenType = VyxTokenTypes.COMMA }
            c == '.' && currentOffset + 1 < endOffset && buffer[currentOffset + 1] == '.' -> {
                currentOffset += 2
                tokenType = VyxTokenTypes.OPERATOR
            }
            c == '.' -> { currentOffset++; tokenType = VyxTokenTypes.DOT }
            c == ':' && currentOffset + 1 < endOffset && buffer[currentOffset + 1] == ':' -> {
                currentOffset += 2
                tokenType = VyxTokenTypes.OPERATOR
            }
            c == ':' -> { currentOffset++; tokenType = VyxTokenTypes.COLON }
            else -> {
                currentOffset += operatorLength(currentOffset)
                tokenType = VyxTokenTypes.OPERATOR
            }
        }
        if (currentOffset == tokenStart) {
            currentOffset++
            tokenType = TokenType.BAD_CHARACTER
        }
        tokenEnd = currentOffset
    }

    private fun classifyIdentifier(text: String): IElementType {
        if (isInsideAttribute(tokenStart)) return VyxTokenTypes.ATTRIBUTE

        val previousWord = previousWord(tokenStart)
        val previousChar = previousSignificantChar(tokenStart)
        val nextChar = nextSignificantChar(currentOffset)
        val callableChar = nextCallableChar(currentOffset)

        if (previousWord == "fn") return VyxTokenTypes.FUNCTION_DECLARATION
        if (previousWord in VyxTokenTypes.TYPE_DECLARATION_KEYWORDS) {
            return VyxTokenTypes.TYPE_DECLARATION
        }
        if (previousWord == "module" || previousWord == "use" || previousWord == "import" ||
            isNamespaceDirective(tokenStart)
        ) {
            return VyxTokenTypes.MODULE
        }
        if (previousWord == "let" || previousWord == "var") {
            return if (isDirectlyInsideTypeBody(tokenStart)) {
                VyxTokenTypes.FIELD
            } else {
                VyxTokenTypes.LOCAL_VARIABLE
            }
        }
        if (previousWord == "const") return VyxTokenTypes.CONSTANT

        if (nextChar == ':' && isInsideFunctionParameters(tokenStart)) {
            return VyxTokenTypes.PARAMETER
        }
        if (previousChar == '.') {
            return if (callableChar == '(') VyxTokenTypes.METHOD else VyxTokenTypes.FIELD
        }
        if (nextChar == ':' && !isAfterDoubleColon(currentOffset)) return VyxTokenTypes.FIELD
        if (callableChar == '(') {
            return if (text.firstOrNull()?.isUpperCase() == true && isDirectlyInsideTypeBody(tokenStart)) {
                VyxTokenTypes.FUNCTION_DECLARATION
            } else if (text.firstOrNull()?.isUpperCase() == true) {
                VyxTokenTypes.TYPE_REFERENCE
            } else {
                VyxTokenTypes.FUNCTION_CALL
            }
        }
        if (text in declaredFunctions) return VyxTokenTypes.FUNCTION_CALL
        if (isTypePosition(tokenStart) || text.firstOrNull()?.isUpperCase() == true) {
            return VyxTokenTypes.TYPE_REFERENCE
        }
        if (isConstantName(text)) return VyxTokenTypes.CONSTANT
        return VyxTokenTypes.IDENTIFIER
    }

    private fun previousWord(before: Int): String {
        var i = before - 1
        while (i >= startOffset && buffer[i].isWhitespace()) i--
        val end = i + 1
        while (i >= startOffset && (buffer[i].isLetterOrDigit() || buffer[i] == '_')) i--
        return if (end > i + 1) buffer.subSequence(i + 1, end).toString() else ""
    }

    private fun previousSignificantChar(before: Int): Char? {
        var i = before - 1
        while (i >= startOffset && buffer[i].isWhitespace()) i--
        return if (i >= startOffset) buffer[i] else null
    }

    private fun nextSignificantChar(after: Int): Char? {
        var i = after
        while (i < endOffset && buffer[i].isWhitespace()) i++
        return if (i < endOffset) buffer[i] else null
    }

    /**
     * Returns the character that determines whether an identifier is invoked.
     * Besides a direct `name(...)`, Vyx permits version-qualified calls such as
     * `name@1.0.0["_1"](...)`; the base name is still a function/method.
     */
    private fun nextCallableChar(after: Int): Char? {
        var i = skipWhitespace(after, endOffset)
        if (i < endOffset && buffer[i] == '@') {
            i++
            while (i < endOffset) {
                val ch = buffer[i]
                if (ch.isLetterOrDigit() || ch == '.' || ch == '-' || ch == '_' || ch == '+') i++
                else break
            }
            i = skipWhitespace(i, endOffset)
            if (i < endOffset && buffer[i] == '[') {
                var depth = 1
                var quote: Char? = null
                var escaped = false
                i++
                while (i < endOffset && depth > 0) {
                    val ch = buffer[i]
                    if (quote != null) {
                        if (escaped) escaped = false
                        else if (ch == '\\') escaped = true
                        else if (ch == quote) quote = null
                    } else if (ch == '"' || ch == '\'') {
                        quote = ch
                    } else if (ch == '[') {
                        depth++
                    } else if (ch == ']') {
                        depth--
                    }
                    i++
                }
            }
            i = skipWhitespace(i, endOffset)
        }
        return if (i < endOffset) buffer[i] else null
    }

    private fun skipWhitespace(from: Int, limit: Int): Int {
        var i = from
        while (i < limit && buffer[i].isWhitespace()) i++
        return i
    }

    /**
     * The syntax highlighter runs before semantic tokens arrive.  Index local
     * function declarations once per lexer pass so a function used as a value
     * (`let f: fn(...) = func;`) receives the same color as `func(...)`.
     */
    private fun collectDeclaredFunctions(source: CharSequence): Set<String> {
        val names = LinkedHashSet<String>()
        val limit = source.length
        var i = 0
        while (i < limit) {
            val ch = source[i]
            if (ch == '/' && i + 1 < limit && source[i + 1] == '/') {
                i += 2
                while (i < limit && source[i] != '\n' && source[i] != '\r') i++
                continue
            }
            if (ch == '/' && i + 1 < limit && source[i + 1] == '*') {
                i += 2
                while (i + 1 < limit && !(source[i] == '*' && source[i + 1] == '/')) i++
                i = (i + 2).coerceAtMost(limit)
                continue
            }
            if (ch == '"' || ch == '\'') {
                val quote = ch
                i++
                while (i < limit) {
                    if (source[i] == '\\' && i + 1 < limit) i += 2
                    else if (source[i] == quote) { i++; break }
                    else i++
                }
                continue
            }
            if (ch.isLetter() || ch == '_') {
                val wordStart = i
                i++
                while (i < limit && (source[i].isLetterOrDigit() || source[i] == '_')) i++
                if (source.subSequence(wordStart, i).toString() == "fn") {
                    var nameStart = i
                    while (nameStart < limit && source[nameStart].isWhitespace()) nameStart++
                    if (nameStart < limit && (source[nameStart].isLetter() || source[nameStart] == '_')) {
                        var nameEnd = nameStart + 1
                        while (nameEnd < limit &&
                            (source[nameEnd].isLetterOrDigit() || source[nameEnd] == '_')
                        ) nameEnd++
                        names += source.subSequence(nameStart, nameEnd).toString()
                    }
                }
                continue
            }
            i++
        }
        return names
    }

    private fun isTypePosition(position: Int): Boolean {
        var i = position - 1
        while (i >= startOffset && buffer[i].isWhitespace()) i--
        if (i < startOffset) return false
        if (buffer[i] == ':') {
            return i == startOffset || i == 0 || buffer[i - 1] != ':'
        }
        if (buffer[i] == '>') {
            i--
            while (i >= startOffset && buffer[i].isWhitespace()) i--
            if (i >= startOffset && buffer[i] == '-') return true
        }
        val word = previousWord(position)
        return word == "new" || word == "as" || word == "is" || word == "type"
    }

    private fun isInsideFunctionParameters(position: Int): Boolean {
        var depth = 0
        var i = position - 1
        var open = -1
        while (i >= startOffset) {
            when (buffer[i]) {
                ')' -> depth++
                '(' -> if (depth == 0) {
                    open = i
                    break
                } else depth--
                '{', '}', ';' -> if (depth == 0) return false
            }
            i--
        }
        if (open < 0) return false
        var boundary = open - 1
        while (boundary >= startOffset && buffer[boundary] != '{' && buffer[boundary] != '}' &&
            buffer[boundary] != ';'
        ) {
            boundary--
        }
        val prefix = buffer.subSequence(boundary + 1, open).toString()
        return FN_WORD.containsMatchIn(prefix)
    }

    private fun isNamespaceDirective(position: Int): Boolean {
        var lineStart = position
        while (lineStart > startOffset && buffer[lineStart - 1] != '\n' && buffer[lineStart - 1] != '\r') {
            lineStart--
        }
        val prefix = buffer.subSequence(lineStart, position).toString().trimStart()
        return prefix.startsWith("module ") || prefix.startsWith("use ") ||
            prefix.startsWith("import ") || prefix.startsWith("public module ") ||
            prefix.startsWith("public use ") || prefix.startsWith("public import ")
    }

    private fun isDirectlyInsideTypeBody(position: Int): Boolean {
        var depth = 0
        var i = position - 1
        var open = -1
        while (i >= startOffset) {
            when (buffer[i]) {
                '}' -> depth++
                '{' -> if (depth == 0) {
                    open = i
                    break
                } else depth--
            }
            i--
        }
        if (open < 0) return false
        var boundary = open - 1
        while (boundary >= startOffset && buffer[boundary] != '{' && buffer[boundary] != '}' &&
            buffer[boundary] != ';'
        ) {
            boundary--
        }
        val header = buffer.subSequence(boundary + 1, open).toString()
        return TYPE_BODY_WORD.containsMatchIn(header)
    }

    private fun isInsideAttribute(position: Int): Boolean {
        var i = position - 1
        while (i >= startOffset) {
            if (buffer[i] == ']') return false
            if (buffer[i] == '[' && i > startOffset && buffer[i - 1] == '@') return true
            if (buffer[i] == '\n' || buffer[i] == '\r') return false
            i--
        }
        return false
    }

    private fun isAfterDoubleColon(position: Int): Boolean {
        var i = position
        while (i < endOffset && buffer[i].isWhitespace()) i++
        return i + 1 < endOffset && buffer[i] == ':' && buffer[i + 1] == ':'
    }

    private fun isConstantName(text: String): Boolean {
        var hasLetter = false
        for (ch in text) {
            if (ch.isLetter()) {
                hasLetter = true
                if (ch.isLowerCase()) return false
            } else if (!ch.isDigit() && ch != '_') {
                return false
            }
        }
        return hasLetter
    }

    private fun operatorLength(offset: Int): Int {
        if (offset + 1 >= endOffset) return 1
        val pair = "${buffer[offset]}${buffer[offset + 1]}"
        if (pair in TWO_CHAR_OPERATORS) {
            if (offset + 2 < endOffset && pair in setOf("<<", ">>") && buffer[offset + 2] == '=') {
                return 3
            }
            return 2
        }
        return 1
    }
}
