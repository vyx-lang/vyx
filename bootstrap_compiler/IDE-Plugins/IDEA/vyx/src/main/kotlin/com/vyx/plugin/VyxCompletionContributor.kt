package com.vyx.plugin

import com.intellij.codeInsight.completion.*
import com.intellij.codeInsight.lookup.LookupElementBuilder
import com.intellij.patterns.PlatformPatterns
import com.intellij.patterns.StandardPatterns
import com.intellij.util.ProcessingContext

/**
 * Offline completion so Ctrl+Space is never empty when LSP is offline.
 * `use std.` / `import` paths prefix-match the registered package list.
 */
class VyxCompletionContributor : CompletionContributor() {
    init {
        extend(
            CompletionType.BASIC,
            PlatformPatterns.psiElement().withLanguage(VyxLanguage.INSTANCE),
            object : CompletionProvider<CompletionParameters>() {
                override fun addCompletions(
                    parameters: CompletionParameters,
                    context: ProcessingContext,
                    result: CompletionResultSet
                ) {
                    val document = parameters.editor.document
                    val offset = parameters.offset
                    val lineNo = document.getLineNumber(offset)
                    val lineStart = document.getLineStartOffset(lineNo)
                    val line = document.charsSequence.subSequence(
                        lineStart,
                        document.getLineEndOffset(lineNo),
                    ).toString()
                    val col = offset - lineStart
                    val usePath = VyxPackageCompletion.usePathAt(line, col)
                    if (usePath != null) {
                        val matches = VyxPackageCompletion.prefixMatches(
                            VyxPackageCompletion.packages(parameters.editor.project),
                            usePath.path,
                        )
                        val scoped = result.withPrefixMatcher(usePath.path)
                        for (pkg in matches) {
                            val insert = VyxPackageCompletion.rewriteSeps(pkg, usePath.colonStyle)
                            val priority = if (pkg == "std" || pkg.startsWith("std.")) 1200.0 else 1100.0
                            scoped.addElement(
                                PrioritizedLookupElement.withPriority(
                                    LookupElementBuilder.create(insert)
                                        .withLookupString(pkg)
                                        .withPresentableText(pkg)
                                        .withTypeText("package", true)
                                        .withBoldness(pkg.startsWith("std.")),
                                    priority,
                                )
                            )
                        }
                        scoped.restartCompletionOnPrefixChange(StandardPatterns.string())
                        return
                    }

                    for (kw in VyxTokenTypes.KEYWORDS) {
                        result.addElement(
                            PrioritizedLookupElement.withPriority(
                                LookupElementBuilder.create(kw)
                                .withBoldness(true)
                                .withTypeText("keyword", true),
                                -1000.0,
                            )
                        )
                    }
                    for (ty in VyxTokenTypes.TYPES) {
                        result.addElement(
                            PrioritizedLookupElement.withPriority(
                                LookupElementBuilder.create(ty)
                                    .withTypeText("type", true),
                                -900.0,
                            )
                        )
                    }
                    for (fn in listOf(
                        "print", "assert", "assert_eq", "panic", "typeinfo",
                        "makeVec", "makeDict", "makeSet", "makeRef", "makeScope",
                        "makeEvent", "makeChannel", "makeMutex", "spawn", "sleep_ms"
                    )) {
                        result.addElement(
                            PrioritizedLookupElement.withPriority(
                                LookupElementBuilder.create(fn)
                                .withTypeText("builtin", true)
                                .withInsertHandler { ctx, _ ->
                                    val doc = ctx.document
                                    val tail = ctx.tailOffset
                                    if (tail < doc.textLength && doc.charsSequence[tail] == '(') return@withInsertHandler
                                    doc.insertString(tail, "()")
                                    ctx.editor.caretModel.moveToOffset(tail + 1)
                                },
                                -800.0,
                            )
                        )
                    }
                }
            }
        )
    }
}
