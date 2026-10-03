#pragma once
#include "../Common/SourceLocation.h"
#include <string>
#include <string_view>
#include <map>

namespace vyx {

enum class TokenKind {
    // Literals
    IntLiteral,         // 42, 0xFF
    FloatLiteral,       // 3.14
    StringLiteral,      // "hello"
    CharLiteral,        // 'a'
    BoolLiteral,        // true, false

    // Identifier
    Identifier,

    // Keywords - declarations
    KW_fn,
    KW_let,
    KW_var,
    KW_struct,
    KW_class,
    KW_interface,
    KW_enum,
    KW_error,
    KW_impl,
    KW_import,
    KW_module,
    KW_use,
    KW_export,
    KW_extern,

    // Keywords - control flow
    KW_if,
    KW_elif,
    KW_else,
    KW_while,
    KW_for,
    KW_foreach,
    KW_match,
    KW_case,
    KW_default,
    KW_return,
    KW_break,
    KW_continue,
    KW_yield,

    // Keywords - types & memory
    KW_true,
    KW_false,
    KW_null,
    KW_self,
    KW_mut,
    KW_public,
    KW_private,
    KW_internal,
    KW_protected,
    KW_override,
    KW_async,
    KW_await,
    KW_task,
    KW_fail,
    KW_as,
    KW_static,
    KW_const_kw,
    KW_type,
    KW_newtype,
    KW_defer,
    KW_static_assert,
    KW_comptime,
    KW_bench,
    KW_concept,
    KW_where,
    KW_requires,
    KW_unsafe,
    KW_asm,

    // Keywords - type names
    KW_i8, KW_i16, KW_i32, KW_i64,
    KW_u8, KW_u16, KW_u32, KW_u64,
    KW_f32, KW_f64,
    KW_bool,
    KW_char,
    // `string` is NOT a keyword.  R5 "lean compiler, fat stdlib" step: the
    // identifier `string` resolves to whatever is bound in the symbol table /
    // lang-item registry — currently the built-in primitive type, eventually
    // the stdlib `@[lang_item("string")] class String`.  Previously we had a
    // `KW_string` token for it, but keeping it as an ordinary Identifier
    // matches how every other named type (class, struct, enum) lives in the
    // grammar and lets the lang-item registry take over resolution cleanly.
    KW_isize, KW_usize,
    KW_rawptr,
    KW_void,
    KW_when,
    KW_macro,
    KW_is,
    KW_in,

    // Operators - arithmetic
    Plus,           // +
    Minus,          // -
    Star,           // *
    Slash,          // /
    Percent,        // %

    // Operators - comparison
    EqualEqual,     // ==
    BangEqual,      // !=
    Less,           // <
    LessEqual,      // <=
    Greater,        // >
    GreaterEqual,   // >=

    // Operators - logical
    AmpAmp,         // &&
    PipePipe,       // ||
    Bang,           // !

    // Operators - bitwise
    Amp,            // &
    Pipe,           // |
    Caret,          // ^
    Tilde,          // ~
    LessLess,       // <<
    GreaterGreater, // >>

    // Operators - assignment
    Equal,          // =
    PlusEqual,      // +=
    MinusEqual,     // -=
    StarEqual,      // *=
    SlashEqual,     // /=
    PercentEqual,   // %=
    LessLessEqual,  // <<=
    GreaterGreaterEqual, // >>=
    AmpEqual,       // &=
    PipeEqual,      // |=
    CaretEqual,     // ^=

    // Operators - special
    Arrow,          // ->
    FatArrow,       // =>
    DotDot,         // ..
    DotDotEqual,    // ..=
    Ellipsis,       // ...
    ColonColon,     // ::
    Question,       // ?
    QuestionQuestion, // ??
    MatchOp,        // =~
    PlusPlus,       // ++
    MinusMinus,     // --
    PipeArrow,      // |>

    // Delimiters
    LParen,         // (
    RParen,         // )
    LBrace,         // {
    RBrace,         // }
    LBracket,       // [
    RBracket,       // ]

    // Punctuation
    Comma,          // ,
    Semicolon,      // ;
    Colon,          // :
    Dot,            // .
    At,             // @

    // Special
    Eof,
    Invalid,
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    std::string_view text;
    SourceLocation location;

    union {
        int64_t intValue;
        double floatValue;
        bool boolValue;
    };
    std::string stringValue;
    std::string docComment;

    bool is(TokenKind k) const { return kind == k; }
    bool isNot(TokenKind k) const { return kind != k; }
    bool isOneOf(TokenKind k1, TokenKind k2) const { return is(k1) || is(k2); }

    template <typename... Ts>
    bool isOneOf(TokenKind k1, TokenKind k2, Ts... ks) const {
        return is(k1) || isOneOf(k2, ks...);
    }

    bool isTypeName() const {
        switch (kind) {
            case TokenKind::KW_i8:  case TokenKind::KW_i16: case TokenKind::KW_i32: case TokenKind::KW_i64:
            case TokenKind::KW_u8:  case TokenKind::KW_u16: case TokenKind::KW_u32: case TokenKind::KW_u64:
            case TokenKind::KW_f32: case TokenKind::KW_f64:
            case TokenKind::KW_bool:   case TokenKind::KW_char:
            // `string` is no longer a keyword — it's an Identifier whose
            // resolution goes through the symbol table + lang-item registry.
            case TokenKind::KW_isize:  case TokenKind::KW_usize:
            case TokenKind::KW_rawptr: case TokenKind::KW_void:
                return true;
            default:
                return false;
        }
    }
};

inline const std::map<std::string_view, TokenKind>& getKeywordMap() {
    static const std::map<std::string_view, TokenKind> map = {
        {"fn", TokenKind::KW_fn}, {"let", TokenKind::KW_let}, {"var", TokenKind::KW_var},
        {"struct", TokenKind::KW_struct}, {"class", TokenKind::KW_class},
        {"interface", TokenKind::KW_interface}, {"enum", TokenKind::KW_enum},
        {"error", TokenKind::KW_error}, {"impl", TokenKind::KW_impl},
        {"import", TokenKind::KW_import},
        {"module", TokenKind::KW_module}, {"use", TokenKind::KW_use},
        {"export", TokenKind::KW_export}, {"extern", TokenKind::KW_extern},
        {"if", TokenKind::KW_if}, {"elif", TokenKind::KW_elif}, {"else", TokenKind::KW_else},
        {"while", TokenKind::KW_while}, {"for", TokenKind::KW_for},
        {"foreach", TokenKind::KW_foreach}, {"match", TokenKind::KW_match},
        {"case", TokenKind::KW_case}, {"default", TokenKind::KW_default},
        {"return", TokenKind::KW_return}, {"break", TokenKind::KW_break},
        {"continue", TokenKind::KW_continue}, {"yield", TokenKind::KW_yield},
        {"true", TokenKind::KW_true}, {"false", TokenKind::KW_false},
        {"null", TokenKind::KW_null}, {"self", TokenKind::KW_self},
        {"mut", TokenKind::KW_mut}, {"public", TokenKind::KW_public},
        {"private", TokenKind::KW_private}, {"internal", TokenKind::KW_internal},
        {"protected", TokenKind::KW_protected}, {"override", TokenKind::KW_override},
        {"async", TokenKind::KW_async}, {"await", TokenKind::KW_await},
        {"task", TokenKind::KW_task}, {"fail", TokenKind::KW_fail}, {"as", TokenKind::KW_as},
        {"static", TokenKind::KW_static}, {"const", TokenKind::KW_const_kw},
        {"type", TokenKind::KW_type},
        {"newtype", TokenKind::KW_newtype},
        {"defer", TokenKind::KW_defer},
        {"static_assert", TokenKind::KW_static_assert},
        {"comptime", TokenKind::KW_comptime},
        {"bench", TokenKind::KW_bench},
        {"concept", TokenKind::KW_concept},
        {"trait", TokenKind::KW_interface},
        {"protocol", TokenKind::KW_interface},
        {"where", TokenKind::KW_where},
        {"requires", TokenKind::KW_requires},
        {"asm", TokenKind::KW_asm},
        {"unsafe", TokenKind::KW_unsafe},
        {"i8", TokenKind::KW_i8}, {"i16", TokenKind::KW_i16},
        {"i32", TokenKind::KW_i32}, {"i64", TokenKind::KW_i64},
        {"u8", TokenKind::KW_u8}, {"u16", TokenKind::KW_u16},
        {"u32", TokenKind::KW_u32}, {"u64", TokenKind::KW_u64},
        {"f32", TokenKind::KW_f32}, {"f64", TokenKind::KW_f64},
        {"bool", TokenKind::KW_bool}, {"char", TokenKind::KW_char},
        // `string` is intentionally absent — it tokenises as Identifier so
        // resolution can route through the symbol table + lang-item registry.
        {"isize", TokenKind::KW_isize}, {"usize", TokenKind::KW_usize},
        {"rawptr", TokenKind::KW_rawptr}, {"void", TokenKind::KW_void},
        {"when", TokenKind::KW_when},
        {"macro", TokenKind::KW_macro},
        {"is", TokenKind::KW_is},
        {"in", TokenKind::KW_in},
    };
    return map;
}

inline std::string_view tokenKindToString(TokenKind kind) {
    switch (kind) {
        case TokenKind::IntLiteral:    return "integer literal";
        case TokenKind::FloatLiteral:  return "float literal";
        case TokenKind::StringLiteral: return "string literal";
        case TokenKind::CharLiteral:   return "char literal";
        case TokenKind::BoolLiteral:   return "bool literal";
        case TokenKind::Identifier:    return "identifier";

        case TokenKind::KW_fn:        return "'fn'";
        case TokenKind::KW_let:       return "'let'";
        case TokenKind::KW_var:       return "'var'";
        case TokenKind::KW_struct:    return "'struct'";
        case TokenKind::KW_class:     return "'class'";
        case TokenKind::KW_interface: return "'interface'";
        case TokenKind::KW_enum:      return "'enum'";
        case TokenKind::KW_error:     return "'error'";
        case TokenKind::KW_impl:      return "'impl'";
        case TokenKind::KW_import:    return "'import'";
        case TokenKind::KW_module:    return "'module'";
        case TokenKind::KW_use:       return "'use'";
        case TokenKind::KW_export:    return "'export'";
        case TokenKind::KW_extern:    return "'extern'";

        case TokenKind::KW_if:        return "'if'";
        case TokenKind::KW_elif:      return "'elif'";
        case TokenKind::KW_else:      return "'else'";
        case TokenKind::KW_while:     return "'while'";
        case TokenKind::KW_for:       return "'for'";
        case TokenKind::KW_foreach:   return "'foreach'";
        case TokenKind::KW_match:     return "'match'";
        case TokenKind::KW_case:      return "'case'";
        case TokenKind::KW_default:   return "'default'";
        case TokenKind::KW_return:    return "'return'";
        case TokenKind::KW_break:     return "'break'";
        case TokenKind::KW_continue:  return "'continue'";
        case TokenKind::KW_yield:     return "'yield'";

        case TokenKind::KW_true:      return "'true'";
        case TokenKind::KW_false:     return "'false'";
        case TokenKind::KW_null:      return "'null'";
        case TokenKind::KW_self:      return "'self'";
        case TokenKind::KW_mut:       return "'mut'";
        case TokenKind::KW_public:    return "'public'";
        case TokenKind::KW_private:   return "'private'";
        case TokenKind::KW_internal:  return "'internal'";
        case TokenKind::KW_protected: return "'protected'";
        case TokenKind::KW_override:  return "'override'";
        case TokenKind::KW_async:     return "'async'";
        case TokenKind::KW_await:     return "'await'";
        case TokenKind::KW_task:      return "'task'";
        case TokenKind::KW_fail:      return "'fail'";
        case TokenKind::KW_as:        return "'as'";
        case TokenKind::KW_static:    return "'static'";
        case TokenKind::KW_const_kw:  return "'const'";
        case TokenKind::KW_type:      return "'type'";
        case TokenKind::KW_newtype:   return "'newtype'";
        case TokenKind::KW_defer:     return "'defer'";
        case TokenKind::KW_static_assert: return "'static_assert'";
        case TokenKind::KW_comptime:  return "'comptime'";
        case TokenKind::KW_bench:     return "'bench'";
        case TokenKind::KW_concept:   return "'concept'";
        case TokenKind::KW_where:     return "'where'";
        case TokenKind::KW_requires:  return "'requires'";
        case TokenKind::KW_unsafe:    return "'unsafe'";
        case TokenKind::KW_asm:       return "'asm'";

        case TokenKind::KW_i8:     return "'i8'";
        case TokenKind::KW_i16:    return "'i16'";
        case TokenKind::KW_i32:    return "'i32'";
        case TokenKind::KW_i64:    return "'i64'";
        case TokenKind::KW_u8:     return "'u8'";
        case TokenKind::KW_u16:    return "'u16'";
        case TokenKind::KW_u32:    return "'u32'";
        case TokenKind::KW_u64:    return "'u64'";
        case TokenKind::KW_f32:    return "'f32'";
        case TokenKind::KW_f64:    return "'f64'";
        case TokenKind::KW_bool:   return "'bool'";
        case TokenKind::KW_char:   return "'char'";
        case TokenKind::KW_isize:  return "'isize'";
        case TokenKind::KW_usize:  return "'usize'";
        case TokenKind::KW_rawptr: return "'rawptr'";
        case TokenKind::KW_void:   return "'void'";
        case TokenKind::KW_when:   return "'when'";
        case TokenKind::KW_macro:  return "'macro'";
        case TokenKind::KW_is:     return "'is'";
        case TokenKind::KW_in:     return "'in'";

        case TokenKind::Plus:           return "'+'";
        case TokenKind::Minus:          return "'-'";
        case TokenKind::Star:           return "'*'";
        case TokenKind::Slash:          return "'/'";
        case TokenKind::Percent:        return "'%'";
        case TokenKind::EqualEqual:     return "'=='";
        case TokenKind::BangEqual:      return "'!='";
        case TokenKind::Less:           return "'<'";
        case TokenKind::LessEqual:      return "'<='";
        case TokenKind::Greater:        return "'>'";
        case TokenKind::GreaterEqual:   return "'>='";
        case TokenKind::AmpAmp:         return "'&&'";
        case TokenKind::PipePipe:       return "'||'";
        case TokenKind::Bang:           return "'!'";
        case TokenKind::Amp:            return "'&'";
        case TokenKind::Pipe:           return "'|'";
        case TokenKind::Caret:          return "'^'";
        case TokenKind::Tilde:          return "'~'";
        case TokenKind::LessLess:       return "'<<'";
        case TokenKind::GreaterGreater: return "'>>'";
        case TokenKind::Equal:          return "'='";
        case TokenKind::PlusEqual:      return "'+='";
        case TokenKind::MinusEqual:     return "'-='";
        case TokenKind::StarEqual:      return "'*='";
        case TokenKind::SlashEqual:     return "'/='";
        case TokenKind::PercentEqual:   return "'%='";
        case TokenKind::LessLessEqual:  return "'<<='";
        case TokenKind::GreaterGreaterEqual: return "'>>='";
        case TokenKind::AmpEqual:       return "'&='";
        case TokenKind::PipeEqual:      return "'|='";
        case TokenKind::CaretEqual:     return "'^='";
        case TokenKind::Arrow:          return "'->'";
        case TokenKind::FatArrow:       return "'=>'";
        case TokenKind::DotDot:         return "'..'";
        case TokenKind::DotDotEqual:    return "'..='";
        case TokenKind::Ellipsis:       return "'...'";
        case TokenKind::ColonColon:     return "'::'";
        case TokenKind::Question:       return "'?'";
        case TokenKind::QuestionQuestion: return "'?\?'";
        case TokenKind::MatchOp:        return "'=~'";
        case TokenKind::PlusPlus:       return "'++'";
        case TokenKind::MinusMinus:     return "'--'";
        case TokenKind::PipeArrow:      return "'|>'";

        case TokenKind::LParen:    return "'('";
        case TokenKind::RParen:    return "')'";
        case TokenKind::LBrace:    return "'{'";
        case TokenKind::RBrace:    return "'}'";
        case TokenKind::LBracket:  return "'['";
        case TokenKind::RBracket:  return "']'";

        case TokenKind::Comma:     return "','";
        case TokenKind::Semicolon: return "';'";
        case TokenKind::Colon:     return "':'";
        case TokenKind::Dot:       return "'.'";
        case TokenKind::At:        return "'@'";

        case TokenKind::Eof:     return "end of file";
        case TokenKind::Invalid: return "invalid token";
    }
    return "token";
}

} // namespace vyx
