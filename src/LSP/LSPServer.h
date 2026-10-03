#pragma once
#include "../Lexer/Lexer.h"
#include "../Parser/Parser.h"
#include "../Sema/Sema.h"
#include "../Common/Diagnostics.h"
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace vyx {
namespace lsp {

struct Position {
    int line = 0;
    int character = 0;
};

struct Range {
    Position start;
    Position end;
};

struct Location {
    std::string uri;
    Range range;
};

struct CompletionItem {
    std::string label;
    int kind = 1; // 1=Text, 2=Method, 3=Function, 6=Variable, 7=Class, 8=Interface, 22=Struct, 13=Enum
    std::string detail;
    std::string documentation;
};

struct Diagnostic {
    Range range;
    int severity = 1; // 1=Error, 2=Warning, 3=Info, 4=Hint
    std::string message;
};

struct HoverResult {
    std::string contents;
    Range range;
};

struct LSPSymbolInfo {
    std::string name;
    std::string typeName;
    SourceLocation declLocation;
    int completionKind = 1;
};

class LSPServer {
public:
    LSPServer();

    void run();

    // Document management
    void openDocument(const std::string& uri, const std::string& content);
    void updateDocument(const std::string& uri, const std::string& content);
    void closeDocument(const std::string& uri);

    // LSP features
    std::vector<CompletionItem> completion(const std::string& uri, Position pos);
    std::vector<Location> gotoDefinition(const std::string& uri, Position pos);
    HoverResult hover(const std::string& uri, Position pos);
    std::vector<Diagnostic> diagnostics(const std::string& uri);
    std::vector<Location> references(const std::string& uri, Position pos);
    std::map<std::string, std::vector<std::pair<Range, std::string>>> rename(
        const std::string& uri, Position pos, const std::string& newName);

    // New LSP features
    struct DocumentSymbol {
        std::string name;
        int kind; // 5=Class, 6=Method, 12=Function, 13=Variable, 23=Struct, 10=Enum
        Range range;
        Range selectionRange;
        std::vector<DocumentSymbol> children;
    };
    std::vector<DocumentSymbol> documentSymbols(const std::string& uri);

    struct SignatureInfo {
        std::string label;
        std::vector<std::pair<int, int>> paramRanges;
        int activeParam = 0;
    };
    SignatureInfo signatureHelp(const std::string& uri, Position pos);

    struct FoldingRange {
        int startLine, endLine;
        std::string kind; // "region", "comment", "imports"
    };
    std::vector<FoldingRange> foldingRanges(const std::string& uri);

    std::string formatDocument(const std::string& uri);

private:
    struct DocumentState {
        std::string content;
        std::vector<Token> tokens;
        std::unique_ptr<TranslationUnit> unit;
        std::vector<LSPSymbolInfo> symbols;
        std::vector<Diagnostic> cachedDiags;
    };

    void analyzeDocument(const std::string& uri);
    std::string findWordAtPosition(const std::string& content, Position pos);
    std::vector<LSPSymbolInfo> collectSymbols(const TranslationUnit& unit);

    // JSON-RPC handling
    std::string readMessage();
    void sendMessage(const std::string& json);
    std::string handleRequest(const std::string& method, const std::string& params, int id);
    void handleNotification(const std::string& method, const std::string& params);

    // Simple JSON helpers
    static std::string jsonString(const std::string& s);
    static std::string jsonInt(int n);

    std::map<std::string, DocumentState> documents_;
};

} // namespace lsp
} // namespace vyx
