package com.vyx.plugin

import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighter
import com.intellij.openapi.options.colors.AttributesDescriptor
import com.intellij.openapi.options.colors.ColorDescriptor
import com.intellij.openapi.options.colors.ColorSettingsPage
import javax.swing.Icon

class VyxColorSettingsPage : ColorSettingsPage {
    private val descriptors = arrayOf(
        AttributesDescriptor("Keywords", VyxSyntaxHighlighter.KEYWORD),
        AttributesDescriptor("Literals//String", VyxSyntaxHighlighter.STRING),
        AttributesDescriptor("Literals//Number", VyxSyntaxHighlighter.NUMBER),
        AttributesDescriptor("Comments", VyxSyntaxHighlighter.COMMENT),
        AttributesDescriptor("Identifiers//Default", VyxSyntaxHighlighter.IDENTIFIER),
        AttributesDescriptor("Identifiers//Function declaration", VyxSyntaxHighlighter.FUNCTION_DECLARATION),
        AttributesDescriptor("Identifiers//Function call or reference", VyxSyntaxHighlighter.FUNCTION_CALL),
        AttributesDescriptor("Identifiers//Method", VyxSyntaxHighlighter.METHOD),
        AttributesDescriptor("Identifiers//Field or property", VyxSyntaxHighlighter.FIELD),
        AttributesDescriptor("Identifiers//Parameter", VyxSyntaxHighlighter.PARAMETER),
        AttributesDescriptor("Identifiers//Local variable", VyxSyntaxHighlighter.LOCAL_VARIABLE),
        AttributesDescriptor("Identifiers//Constant", VyxSyntaxHighlighter.CONSTANT),
        AttributesDescriptor("Types//Built-in type", VyxSyntaxHighlighter.TYPE),
        AttributesDescriptor("Types//Type declaration", VyxSyntaxHighlighter.TYPE_DECLARATION),
        AttributesDescriptor("Types//Type reference", VyxSyntaxHighlighter.TYPE_REFERENCE),
        AttributesDescriptor("Modules and namespaces", VyxSyntaxHighlighter.MODULE),
        AttributesDescriptor("Metadata and attributes", VyxSyntaxHighlighter.ATTRIBUTE),
        AttributesDescriptor("Punctuation//Operators and colon", VyxSyntaxHighlighter.OPERATOR),
        AttributesDescriptor("Punctuation//Braces", VyxSyntaxHighlighter.BRACES),
        AttributesDescriptor("Punctuation//Parentheses", VyxSyntaxHighlighter.PARENTHESES),
        AttributesDescriptor("Punctuation//Brackets", VyxSyntaxHighlighter.BRACKETS),
        AttributesDescriptor("Punctuation//Semicolon", VyxSyntaxHighlighter.SEMICOLON),
        AttributesDescriptor("Punctuation//Comma", VyxSyntaxHighlighter.COMMA),
        AttributesDescriptor("Punctuation//Dot", VyxSyntaxHighlighter.DOT),
    )

    private val tags = mapOf(
        "functionDeclaration" to VyxSyntaxHighlighter.FUNCTION_DECLARATION,
        "functionReference" to VyxSyntaxHighlighter.FUNCTION_CALL,
        "method" to VyxSyntaxHighlighter.METHOD,
        "field" to VyxSyntaxHighlighter.FIELD,
        "parameter" to VyxSyntaxHighlighter.PARAMETER,
        "local" to VyxSyntaxHighlighter.LOCAL_VARIABLE,
        "constant" to VyxSyntaxHighlighter.CONSTANT,
        "typeDeclaration" to VyxSyntaxHighlighter.TYPE_DECLARATION,
        "typeReference" to VyxSyntaxHighlighter.TYPE_REFERENCE,
        "module" to VyxSyntaxHighlighter.MODULE,
        "attribute" to VyxSyntaxHighlighter.ATTRIBUTE,
    )

    override fun getIcon(): Icon = VyxIcons.Vyx

    override fun getHighlighter(): SyntaxHighlighter = VyxSyntaxHighlighter()

    override fun getDemoText(): String = """
        @[<attribute>version</attribute>("1.0.0")]
        module <module>demo</module>;

        class <typeDeclaration>Counter</typeDeclaration> {
            var <field>value</field>: i32;

            fn <functionDeclaration>increment</functionDeclaration>(self, <parameter>amount</parameter>: i32) -> i32 {
                let <local>next</local>: i32 = self.<field>value</field> + <parameter>amount</parameter>;
                return <local>next</local>;
            }
        }

        const <constant>DEFAULT_STEP</constant>: i32 = 1;
        fn <functionDeclaration>func</functionDeclaration>(a: i32) -> i32 { return a + 1; }

        fn <functionDeclaration>main</functionDeclaration>() {
            let f: fn(i32) -> i32 = <functionReference>func</functionReference>;
            let counter: <typeReference>Counter</typeReference>;
            print(<functionReference>add</functionReference>@1.0.0["_1"](1, 2));
            counter.<method>increment</method>(DEFAULT_STEP);
        }
    """.trimIndent()

    override fun getAdditionalHighlightingTagToDescriptorMap(): Map<String, TextAttributesKey> = tags

    override fun getAttributeDescriptors(): Array<AttributesDescriptor> = descriptors

    override fun getColorDescriptors(): Array<ColorDescriptor> = ColorDescriptor.EMPTY_ARRAY

    override fun getDisplayName(): String = "Vyx"
}
