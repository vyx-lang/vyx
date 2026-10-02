package com.vyx.plugin

import com.intellij.psi.tree.IElementType
import org.junit.Assert.assertEquals
import org.junit.Test

class VyxLexerTest {
    @Test
    fun `classifies declaration and usage roles before LSP is ready`() {
        val source = """
            module demo;
            class Test {
                var field: i32;
                Test() {}
                fn add(self, value: i32) -> i32 {
                    let local = value;
                    self.field = local;
                    return helper(value);
                }
            }
        """.trimIndent()

        val actual = lex(source)
        assertEquals(VyxTokenTypes.MODULE, actual.first { it.first == "demo" }.second)
        assertEquals(VyxTokenTypes.TYPE_DECLARATION, actual.first { it.first == "Test" }.second)
        assertEquals(VyxTokenTypes.FIELD, actual.first { it.first == "field" }.second)
        assertEquals(
            VyxTokenTypes.FUNCTION_DECLARATION,
            actual.filter { it.first == "Test" }[1].second,
        )
        assertEquals(VyxTokenTypes.FUNCTION_DECLARATION, actual.first { it.first == "add" }.second)
        assertEquals(VyxTokenTypes.PARAMETER, actual.first { it.first == "value" }.second)
        assertEquals(VyxTokenTypes.LOCAL_VARIABLE, actual.first { it.first == "local" }.second)
        assertEquals(
            listOf(VyxTokenTypes.FIELD, VyxTokenTypes.FIELD),
            actual.filter { it.first == "field" }.map { it.second },
        )
        assertEquals(VyxTokenTypes.FUNCTION_CALL, actual.first { it.first == "helper" }.second)
    }

    @Test
    fun `keeps function values and version qualified calls colored as functions`() {
        val source = """
            fn func(a: i32) -> i32 { return a + 1; }
            fn main() {
                let f: fn(i32) -> i32 = func;
                print(f(1));
                print(add(1, 2));
                print(add@1.0.0["_1"](1, 2));
                service.run@2.1.0["fast"]();
            }
        """.trimIndent()

        val actual = lex(source)
        assertEquals(
            listOf(VyxTokenTypes.FUNCTION_DECLARATION, VyxTokenTypes.FUNCTION_CALL),
            actual.filter { it.first == "func" }.map { it.second },
        )
        assertEquals(
            listOf(VyxTokenTypes.FUNCTION_CALL, VyxTokenTypes.FUNCTION_CALL),
            actual.filter { it.first == "add" }.map { it.second },
        )
        assertEquals(VyxTokenTypes.METHOD, actual.first { it.first == "run" }.second)
    }

    private fun lex(source: String): List<Pair<String, IElementType>> {
        val lexer = VyxLexer()
        lexer.start(source, 0, source.length, 0)
        val result = mutableListOf<Pair<String, IElementType>>()
        while (lexer.tokenType != null) {
            val type = lexer.tokenType!!
            val text = source.substring(lexer.tokenStart, lexer.tokenEnd)
            result += text to type
            lexer.advance()
        }
        return result
    }
}

class VyxIndentTest {
    @Test
    fun `indents nested braces and outdents closers`() {
        val src = "fn main() -> i32 {\nlet x = 1;\nif (x > 0) {\nreturn x;\n}\nreturn 0;\n}\n"
        val got = VyxIndent.format(src)
        val expected = "fn main() -> i32 {\n    let x = 1;\n    if (x > 0) {\n        return x;\n    }\n    return 0;\n}\n"
        assertEquals(expected, got)
    }
}

class VyxPackageCompletionTest {
    @Test
    fun `use std dot is a package path`() {
        val line = "use std."
        val got = VyxPackageCompletion.usePathAt(line, line.length)
        assertEquals("std.", got?.path)
        assertEquals(false, got?.colonStyle)
    }

    @Test
    fun `prefix match is differential`() {
        val pkgs = listOf(
            "std.collections",
            "std.core",
            "std.fs",
            "std.testing.mock",
            "bootstrap.lexer",
        )
        val afterDot = VyxPackageCompletion.prefixMatches(pkgs, "std.")
        assertEquals(
            listOf("std.collections", "std.core", "std.fs", "std.testing.mock", "std.testing"),
            afterDot,
        )
        val afterC = VyxPackageCompletion.prefixMatches(pkgs, "std.c")
        assertEquals(listOf("std.collections", "std.core"), afterC)
        val afterFs = VyxPackageCompletion.prefixMatches(pkgs, "bootstrap.")
        assertEquals(listOf("bootstrap.lexer"), afterFs)
    }

    @Test
    fun `module line extracts dotted name`() {
        assertEquals(
            "std.collections",
            VyxPackageCompletion.moduleNameFromText("// header\nmodule std.collections;\nfn x() {}\n"),
        )
    }
}
