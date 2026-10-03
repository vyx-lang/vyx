#include "LSPServer.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <map>

namespace vyx {
namespace lsp {

LSPServer::LSPServer() {}

// ============================================================
//  JSON-RPC Transport (stdio)
// ============================================================

std::string LSPServer::readMessage() {
    std::string header;
    int contentLength = 0;

    while (std::getline(std::cin, header)) {
        if (header.empty() || header == "\r") break;
        if (header.back() == '\r') header.pop_back();
        if (header.substr(0, 16) == "Content-Length: ") {
            contentLength = std::stoi(header.substr(16));
        }
    }

    if (contentLength == 0) return "";
    std::string body(contentLength, '\0');
    std::cin.read(body.data(), contentLength);
    return body;
}

void LSPServer::sendMessage(const std::string& json) {
    std::cout << "Content-Length: " << json.size() << "\r\n\r\n" << json;
    std::cout.flush();
}

std::string LSPServer::jsonString(const std::string& s) {
    std::string escaped;
    escaped += '"';
    for (char c : s) {
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:   escaped += c; break;
        }
    }
    escaped += '"';
    return escaped;
}

std::string LSPServer::jsonInt(int n) {
    return std::to_string(n);
}

static std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    auto pos = json.find(searchKey);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + searchKey.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static int extractJsonInt(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    auto pos = json.find(searchKey);
    if (pos == std::string::npos) return -1;
    pos = json.find(':', pos + searchKey.size());
    if (pos == std::string::npos) return -1;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    std::string num;
    while (pos < json.size() && (std::isdigit(json[pos]) || json[pos] == '-')) {
        num += json[pos++];
    }
    return num.empty() ? -1 : std::stoi(num);
}

// ============================================================
//  Main loop
// ============================================================

void LSPServer::run() {
    [[maybe_unused]] bool initialized = false;

    while (true) {
        std::string msg = readMessage();
        if (msg.empty()) break;

        std::string method = extractJsonString(msg, "method");
        int id = extractJsonInt(msg, "id");

        if (method == "initialize") {
            std::string response = R"({
                "jsonrpc": "2.0",
                "id": )" + jsonInt(id) + R"(,
                "result": {
                    "capabilities": {
                        "textDocumentSync": 1,
                        "completionProvider": {"triggerCharacters": [".", ":"]},
                        "signatureHelpProvider": {"triggerCharacters": ["(", ","]},
                        "hoverProvider": true,
                        "definitionProvider": true,
                        "referencesProvider": true,
                        "renameProvider": true,
                        "documentSymbolProvider": true,
                        "foldingRangeProvider": true,
                        "documentFormattingProvider": true
                    },
                    "serverInfo": {"name": "vyxc-lsp", "version": "0.6.0"}
                }
            })";
            sendMessage(response);
            initialized = true;
        } else if (method == "initialized") {
            // no-op
        } else if (method == "shutdown") {
            sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) + ",\"result\":null}");
        } else if (method == "exit") {
            break;
        } else if (method == "textDocument/didOpen") {
            std::string uri = extractJsonString(msg, "uri");
            auto textPos = msg.find("\"text\"");
            if (textPos != std::string::npos) {
                auto start = msg.find('"', textPos + 6);
                if (start != std::string::npos) {
                    std::string text;
                    for (size_t i = start + 1; i < msg.size(); ++i) {
                        if (msg[i] == '"' && msg[i - 1] != '\\') break;
                        if (msg[i] == '\\' && i + 1 < msg.size()) {
                            switch (msg[i + 1]) {
                                case 'n': text += '\n'; ++i; break;
                                case 'r': text += '\r'; ++i; break;
                                case 't': text += '\t'; ++i; break;
                                case '"': text += '"'; ++i; break;
                                case '\\': text += '\\'; ++i; break;
                                default: text += msg[i]; break;
                            }
                        } else {
                            text += msg[i];
                        }
                    }
                    openDocument(uri, text);
                }
            }
        } else if (method == "textDocument/didChange") {
            std::string uri = extractJsonString(msg, "uri");
            auto textPos = msg.find("\"text\"");
            if (textPos != std::string::npos) {
                auto start = msg.find('"', textPos + 6);
                if (start != std::string::npos) {
                    std::string text;
                    for (size_t i = start + 1; i < msg.size(); ++i) {
                        if (msg[i] == '"' && msg[i - 1] != '\\') break;
                        if (msg[i] == '\\' && i + 1 < msg.size()) {
                            switch (msg[i + 1]) {
                                case 'n': text += '\n'; ++i; break;
                                case 't': text += '\t'; ++i; break;
                                case '"': text += '"'; ++i; break;
                                case '\\': text += '\\'; ++i; break;
                                default: text += msg[i]; break;
                            }
                        } else {
                            text += msg[i];
                        }
                    }
                    updateDocument(uri, text);
                }
            }
        } else if (method == "textDocument/didClose") {
            std::string uri = extractJsonString(msg, "uri");
            closeDocument(uri);
        } else if (method == "textDocument/completion") {
            std::string uri = extractJsonString(msg, "uri");
            int line = extractJsonInt(msg, "line");
            int character = extractJsonInt(msg, "character");
            auto items = completion(uri, {line, character});

            std::string itemsJson = "[";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i > 0) itemsJson += ",";
                itemsJson += "{\"label\":" + jsonString(items[i].label) +
                    ",\"kind\":" + jsonInt(items[i].kind) +
                    ",\"detail\":" + jsonString(items[i].detail) + "}";
            }
            itemsJson += "]";
            sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) +
                ",\"result\":{\"isIncomplete\":false,\"items\":" + itemsJson + "}}");

        } else if (method == "textDocument/hover") {
            std::string uri = extractJsonString(msg, "uri");
            int line = extractJsonInt(msg, "line");
            int character = extractJsonInt(msg, "character");
            auto result = hover(uri, {line, character});

            if (!result.contents.empty()) {
                sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) +
                    ",\"result\":{\"contents\":{\"kind\":\"markdown\",\"value\":" +
                    jsonString(result.contents) + "}}}");
            } else {
                sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) + ",\"result\":null}");
            }

        } else if (method == "textDocument/definition") {
            std::string uri = extractJsonString(msg, "uri");
            int line = extractJsonInt(msg, "line");
            int character = extractJsonInt(msg, "character");
            auto locs = gotoDefinition(uri, {line, character});

            if (!locs.empty()) {
                auto& loc = locs[0];
                sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) +
                    ",\"result\":{\"uri\":" + jsonString(loc.uri) +
                    ",\"range\":{\"start\":{\"line\":" + jsonInt(loc.range.start.line) +
                    ",\"character\":" + jsonInt(loc.range.start.character) +
                    "},\"end\":{\"line\":" + jsonInt(loc.range.end.line) +
                    ",\"character\":" + jsonInt(loc.range.end.character) + "}}}}");
            } else {
                sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) + ",\"result\":null}");
            }
        } else if (method == "textDocument/documentSymbol") {
            std::string uri = extractJsonString(msg, "uri");
            auto syms = documentSymbols(uri);
            std::string symsJson = "[";
            for (size_t i = 0; i < syms.size(); ++i) {
                if (i > 0) symsJson += ",";
                auto& s = syms[i];
                symsJson += "{\"name\":" + jsonString(s.name) +
                    ",\"kind\":" + jsonInt(s.kind) +
                    ",\"range\":{\"start\":{\"line\":" + jsonInt(s.range.start.line) +
                    ",\"character\":" + jsonInt(s.range.start.character) +
                    "},\"end\":{\"line\":" + jsonInt(s.range.end.line) +
                    ",\"character\":" + jsonInt(s.range.end.character) +
                    "}},\"selectionRange\":{\"start\":{\"line\":" + jsonInt(s.selectionRange.start.line) +
                    ",\"character\":" + jsonInt(s.selectionRange.start.character) +
                    "},\"end\":{\"line\":" + jsonInt(s.selectionRange.end.line) +
                    ",\"character\":" + jsonInt(s.selectionRange.end.character) + "}}";
                if (!s.children.empty()) {
                    symsJson += ",\"children\":[";
                    for (size_t j = 0; j < s.children.size(); ++j) {
                        if (j > 0) symsJson += ",";
                        auto& c = s.children[j];
                        symsJson += "{\"name\":" + jsonString(c.name) +
                            ",\"kind\":" + jsonInt(c.kind) +
                            ",\"range\":{\"start\":{\"line\":" + jsonInt(c.range.start.line) +
                            ",\"character\":" + jsonInt(c.range.start.character) +
                            "},\"end\":{\"line\":" + jsonInt(c.range.end.line) +
                            ",\"character\":" + jsonInt(c.range.end.character) +
                            "}},\"selectionRange\":{\"start\":{\"line\":" + jsonInt(c.range.start.line) +
                            ",\"character\":" + jsonInt(c.range.start.character) +
                            "},\"end\":{\"line\":" + jsonInt(c.range.end.line) +
                            ",\"character\":" + jsonInt(c.range.end.character) + "}}}";
                    }
                    symsJson += "]";
                }
                symsJson += "}";
            }
            symsJson += "]";
            sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) + ",\"result\":" + symsJson + "}");

        } else if (method == "textDocument/signatureHelp") {
            std::string uri = extractJsonString(msg, "uri");
            int line = extractJsonInt(msg, "line");
            int character = extractJsonInt(msg, "character");
            auto info = signatureHelp(uri, {line, character});
            if (!info.label.empty()) {
                std::string paramsJson = "[";
                for (size_t i = 0; i < info.paramRanges.size(); ++i) {
                    if (i > 0) paramsJson += ",";
                    paramsJson += "{\"label\":[" + jsonInt(info.paramRanges[i].first) +
                        "," + jsonInt(info.paramRanges[i].second) + "]}";
                }
                paramsJson += "]";
                sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) +
                    ",\"result\":{\"signatures\":[{\"label\":" + jsonString(info.label) +
                    ",\"parameters\":" + paramsJson + "}],\"activeSignature\":0,\"activeParameter\":" +
                    jsonInt(info.activeParam) + "}}");
            } else {
                sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) + ",\"result\":null}");
            }

        } else if (method == "textDocument/foldingRange") {
            std::string uri = extractJsonString(msg, "uri");
            auto ranges = foldingRanges(uri);
            std::string rangesJson = "[";
            for (size_t i = 0; i < ranges.size(); ++i) {
                if (i > 0) rangesJson += ",";
                rangesJson += "{\"startLine\":" + jsonInt(ranges[i].startLine) +
                    ",\"endLine\":" + jsonInt(ranges[i].endLine) +
                    ",\"kind\":" + jsonString(ranges[i].kind) + "}";
            }
            rangesJson += "]";
            sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) + ",\"result\":" + rangesJson + "}");

        } else if (method == "textDocument/references") {
            std::string uri = extractJsonString(msg, "uri");
            int line = extractJsonInt(msg, "line");
            int character = extractJsonInt(msg, "character");
            auto refs = references(uri, {line, character});
            std::string refsJson = "[";
            for (size_t i = 0; i < refs.size(); ++i) {
                if (i > 0) refsJson += ",";
                refsJson += "{\"uri\":" + jsonString(refs[i].uri) +
                    ",\"range\":{\"start\":{\"line\":" + jsonInt(refs[i].range.start.line) +
                    ",\"character\":" + jsonInt(refs[i].range.start.character) +
                    "},\"end\":{\"line\":" + jsonInt(refs[i].range.end.line) +
                    ",\"character\":" + jsonInt(refs[i].range.end.character) + "}}}";
            }
            refsJson += "]";
            sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) + ",\"result\":" + refsJson + "}");

        } else if (id >= 0) {
            sendMessage("{\"jsonrpc\":\"2.0\",\"id\":" + jsonInt(id) + ",\"result\":null}");
        }
    }
}

// ============================================================
//  Document management
// ============================================================

void LSPServer::openDocument(const std::string& uri, const std::string& content) {
    documents_[uri].content = content;
    analyzeDocument(uri);

    auto& state = documents_[uri];
    if (!state.cachedDiags.empty()) {
        std::string diagsJson = "[";
        for (size_t i = 0; i < state.cachedDiags.size(); ++i) {
            if (i > 0) diagsJson += ",";
            auto& d = state.cachedDiags[i];
            diagsJson += "{\"range\":{\"start\":{\"line\":" + jsonInt(d.range.start.line) +
                ",\"character\":" + jsonInt(d.range.start.character) +
                "},\"end\":{\"line\":" + jsonInt(d.range.end.line) +
                ",\"character\":" + jsonInt(d.range.end.character) +
                "}},\"severity\":" + jsonInt(d.severity) +
                ",\"message\":" + jsonString(d.message) + "}";
        }
        diagsJson += "]";
        sendMessage("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
            "\"params\":{\"uri\":" + jsonString(uri) + ",\"diagnostics\":" + diagsJson + "}}");
    }
}

void LSPServer::updateDocument(const std::string& uri, const std::string& content) {
    openDocument(uri, content);
}

void LSPServer::closeDocument(const std::string& uri) {
    documents_.erase(uri);
    sendMessage("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
        "\"params\":{\"uri\":" + jsonString(uri) + ",\"diagnostics\":[]}}");
}

void LSPServer::analyzeDocument(const std::string& uri) {
    auto& state = documents_[uri];
    state.symbols.clear();
    state.cachedDiags.clear();

    DiagnosticsEngine diag;
    diag.setSource(state.content);

    Lexer lexer(state.content, uri, diag);
    state.tokens = lexer.tokenizeAll();

    if (!diag.hasErrors()) {
        Parser parser(std::vector<Token>(state.tokens), diag);
        auto unit = parser.parseTranslationUnit(uri);

        if (!diag.hasErrors()) {
            state.symbols = collectSymbols(unit);

            Sema sema(diag);
            sema.analyze(unit);

            state.unit = std::make_unique<TranslationUnit>(std::move(unit));
        }
    }

    state.cachedDiags = diagnostics(uri);
}

std::vector<LSPSymbolInfo> LSPServer::collectSymbols(const TranslationUnit& unit) {
    std::vector<LSPSymbolInfo> syms;
    for (auto& decl : unit.declarations) {
        if (!decl) continue;
        LSPSymbolInfo info;
        info.name = decl->name;
        info.declLocation = decl->location;

        switch (decl->kind) {
            case DeclKind::Function:
                info.completionKind = 3;
                info.typeName = "fn " + decl->name + "(";
                for (size_t i = 0; i < decl->params.size(); ++i) {
                    if (i > 0) info.typeName += ", ";
                    info.typeName += decl->params[i].name;
                    if (decl->params[i].type) info.typeName += ": " + decl->params[i].type->name;
                }
                info.typeName += ")";
                if (decl->returnType) info.typeName += " -> " + decl->returnType->name;
                break;
            case DeclKind::Struct:
                info.completionKind = 22;
                info.typeName = "struct " + decl->name;
                break;
            case DeclKind::Class:
                info.completionKind = 7;
                info.typeName = "class " + decl->name;
                break;
            case DeclKind::Interface:
                info.completionKind = 8;
                info.typeName = "interface " + decl->name;
                break;
            case DeclKind::ErrorDef:
                info.completionKind = 13;
                info.typeName = "enum " + decl->name;
                break;
            default:
                info.completionKind = 6;
                info.typeName = decl->name;
                break;
        }
        syms.push_back(std::move(info));

        for (auto& method : decl->methods) {
            LSPSymbolInfo mInfo;
            mInfo.name = method.name;
            mInfo.declLocation = method.location;
            mInfo.completionKind = 2;
            mInfo.typeName = "fn " + decl->name + "." + method.name + "(";
            for (size_t i = 0; i < method.params.size(); ++i) {
                if (i > 0) mInfo.typeName += ", ";
                mInfo.typeName += method.params[i].name;
                if (method.params[i].type) mInfo.typeName += ": " + method.params[i].type->name;
            }
            mInfo.typeName += ")";
            if (method.returnType) mInfo.typeName += " -> " + method.returnType->name;
            syms.push_back(std::move(mInfo));
        }

        for (auto& field : decl->fields) {
            LSPSymbolInfo fInfo;
            fInfo.name = field.name;
            fInfo.declLocation = field.location;
            fInfo.completionKind = 5;
            fInfo.typeName = field.type ? field.type->name : "unknown";
            syms.push_back(std::move(fInfo));
        }
    }
    return syms;
}

std::string LSPServer::findWordAtPosition(const std::string& content, Position pos) {
    std::istringstream stream(content);
    std::string line;
    int lineNo = 0;
    while (std::getline(stream, line)) {
        if (lineNo == pos.line) {
            int start = pos.character;
            int end = pos.character;
            while (start > 0 && (std::isalnum(line[start - 1]) || line[start - 1] == '_')) start--;
            while (end < (int)line.size() && (std::isalnum(line[end]) || line[end] == '_')) end++;
            if (start < end) return line.substr(start, end - start);
            return "";
        }
        lineNo++;
    }
    return "";
}

// ============================================================
//  Completion
// ============================================================

std::vector<CompletionItem> LSPServer::completion(const std::string& uri, Position pos) {
    std::vector<CompletionItem> items;
    auto it = documents_.find(uri);
    if (it == documents_.end()) return items;

    std::string prefix = findWordAtPosition(it->second.content, pos);

    for (auto& sym : it->second.symbols) {
        if (prefix.empty() || sym.name.find(prefix) == 0) {
            CompletionItem item;
            item.label = sym.name;
            item.kind = sym.completionKind;
            item.detail = sym.typeName;
            items.push_back(std::move(item));
        }
    }

    static const char* keywords[] = {
        "fn", "let", "var", "struct", "class", "interface", "enum", "import", "export",
        "if", "elif", "else", "while", "for", "foreach", "match", "case", "default",
        "return", "break", "continue", "defer", "async", "await", "static_assert",
        "comptime", "bench", "true", "false", "null", "self", "mut", "public", "private"
    };
    for (auto* kw : keywords) {
        std::string kwStr(kw);
        if (prefix.empty() || kwStr.find(prefix) == 0) {
            CompletionItem item;
            item.label = kwStr;
            item.kind = 14; // Keyword
            item.detail = "keyword";
            items.push_back(std::move(item));
        }
    }

    return items;
}

// ============================================================
//  Go to Definition
// ============================================================

std::vector<Location> LSPServer::gotoDefinition(const std::string& uri, Position pos) {
    std::vector<Location> result;
    auto it = documents_.find(uri);
    if (it == documents_.end()) return result;

    std::string word = findWordAtPosition(it->second.content, pos);
    if (word.empty()) return result;

    for (auto& sym : it->second.symbols) {
        if (sym.name == word) {
            Location loc;
            loc.uri = uri;
            loc.range.start.line = static_cast<int>(sym.declLocation.line) - 1;
            loc.range.start.character = static_cast<int>(sym.declLocation.column) - 1;
            loc.range.end.line = loc.range.start.line;
            loc.range.end.character = loc.range.start.character + static_cast<int>(word.size());
            result.push_back(loc);
            break;
        }
    }

    return result;
}

// ============================================================
//  Hover
// ============================================================

HoverResult LSPServer::hover(const std::string& uri, Position pos) {
    HoverResult result;
    auto it = documents_.find(uri);
    if (it == documents_.end()) return result;

    std::string word = findWordAtPosition(it->second.content, pos);
    if (word.empty()) return result;

    for (auto& sym : it->second.symbols) {
        if (sym.name == word) {
            result.contents = "```vyx\n" + sym.typeName + "\n```";
            result.range.start.line = pos.line;
            result.range.start.character = pos.character;
            result.range.end.line = pos.line;
            result.range.end.character = pos.character + static_cast<int>(word.size());
            break;
        }
    }

    return result;
}

// ============================================================
//  Diagnostics
// ============================================================

std::vector<Diagnostic> LSPServer::diagnostics(const std::string& uri) {
    std::vector<Diagnostic> diags;
    auto it = documents_.find(uri);
    if (it == documents_.end()) return diags;

    DiagnosticsEngine diagEngine;
    diagEngine.setSource(it->second.content);

    Lexer lexer(it->second.content, uri, diagEngine);
    auto tokens = lexer.tokenizeAll();

    if (!diagEngine.hasErrors()) {
        Parser parser(std::move(tokens), diagEngine);
        auto unit = parser.parseTranslationUnit(uri);

        if (!diagEngine.hasErrors()) {
            Sema sema(diagEngine);
            sema.analyze(unit);
        }
    }

    for (auto& msg : diagEngine.diagnostics()) {
        Diagnostic d;
        d.range.start.line = static_cast<int>(msg.location.line) - 1;
        d.range.start.character = static_cast<int>(msg.location.column) - 1;
        d.range.end.line = d.range.start.line;
        d.range.end.character = d.range.start.character + 1;
        d.severity = msg.isError ? 1 : 2;
        d.message = msg.message;
        diags.push_back(std::move(d));
    }

    return diags;
}

// ============================================================
//  Document Symbols (outline)
// ============================================================

std::vector<LSPServer::DocumentSymbol> LSPServer::documentSymbols(const std::string& uri) {
    std::vector<DocumentSymbol> result;
    auto it = documents_.find(uri);
    if (it == documents_.end() || !it->second.unit) return result;

    for (auto& decl : it->second.unit->declarations) {
        if (!decl) continue;
        DocumentSymbol sym;
        sym.name = decl->name;
        sym.range.start.line = static_cast<int>(decl->location.line) - 1;
        sym.range.start.character = 0;
        sym.range.end.line = sym.range.start.line;
        sym.range.end.character = 80;
        sym.selectionRange = sym.range;
        sym.selectionRange.end.character = sym.range.start.character + static_cast<int>(decl->name.size());

        switch (decl->kind) {
            case DeclKind::Function: sym.kind = 12; break;
            case DeclKind::Struct:   sym.kind = 23; break;
            case DeclKind::Class:    sym.kind = 5; break;
            case DeclKind::Interface: sym.kind = 11; break;
            case DeclKind::ErrorDef: sym.kind = 10; break;
            default: sym.kind = 13; break;
        }

        for (auto& method : decl->methods) {
            DocumentSymbol child;
            child.name = method.name;
            child.kind = 6;
            child.range.start.line = static_cast<int>(method.location.line) - 1;
            child.range.start.character = 4;
            child.range.end.line = child.range.start.line;
            child.range.end.character = 80;
            child.selectionRange = child.range;
            sym.children.push_back(std::move(child));
        }

        for (auto& field : decl->fields) {
            DocumentSymbol child;
            child.name = field.name;
            child.kind = 8;
            child.range.start.line = static_cast<int>(field.location.line) - 1;
            child.range.start.character = 4;
            child.range.end.line = child.range.start.line;
            child.range.end.character = 40;
            child.selectionRange = child.range;
            sym.children.push_back(std::move(child));
        }

        result.push_back(std::move(sym));
    }
    return result;
}

// ============================================================
//  Signature Help
// ============================================================

LSPServer::SignatureInfo LSPServer::signatureHelp(const std::string& uri, Position pos) {
    SignatureInfo info;
    auto it = documents_.find(uri);
    if (it == documents_.end()) return info;

    std::string word = findWordAtPosition(it->second.content, {pos.line, pos.character - 1});
    for (auto& sym : it->second.symbols) {
        if (sym.name == word && (sym.completionKind == 3 || sym.completionKind == 2)) {
            info.label = sym.typeName;
            int parenStart = static_cast<int>(info.label.find('('));
            int parenEnd = static_cast<int>(info.label.rfind(')'));
            if (parenStart >= 0 && parenEnd > parenStart) {
                std::string params = info.label.substr(parenStart + 1, parenEnd - parenStart - 1);
                int offset = parenStart + 1;
                int start = 0;
                for (int i = 0; i <= static_cast<int>(params.size()); ++i) {
                    if (i == static_cast<int>(params.size()) || params[i] == ',') {
                        info.paramRanges.push_back({offset + start, offset + i});
                        start = i + 2;
                    }
                }
            }
            break;
        }
    }
    return info;
}

// ============================================================
//  Folding Ranges
// ============================================================

std::vector<LSPServer::FoldingRange> LSPServer::foldingRanges(const std::string& uri) {
    std::vector<FoldingRange> result;
    auto it = documents_.find(uri);
    if (it == documents_.end()) return result;

    std::vector<int> braceStack;
    std::istringstream stream(it->second.content);
    std::string line;
    int lineNo = 0;

    while (std::getline(stream, line)) {
        for (char c : line) {
            if (c == '{') braceStack.push_back(lineNo);
            if (c == '}' && !braceStack.empty()) {
                FoldingRange fr;
                fr.startLine = braceStack.back();
                fr.endLine = lineNo;
                fr.kind = "region";
                if (fr.endLine > fr.startLine) result.push_back(fr);
                braceStack.pop_back();
            }
        }
        lineNo++;
    }
    return result;
}

// ============================================================
//  Document Formatting
// ============================================================

std::string LSPServer::formatDocument(const std::string& uri) {
    auto it = documents_.find(uri);
    if (it == documents_.end()) return "";
    return it->second.content;
}

// ============================================================
//  References
// ============================================================

std::vector<Location> LSPServer::references(const std::string& uri, Position pos) {
    std::vector<Location> result;
    auto it = documents_.find(uri);
    if (it == documents_.end()) return result;

    std::string word = findWordAtPosition(it->second.content, pos);
    if (word.empty()) return result;

    std::istringstream stream(it->second.content);
    std::string line;
    int lineNo = 0;
    while (std::getline(stream, line)) {
        size_t searchPos = 0;
        while ((searchPos = line.find(word, searchPos)) != std::string::npos) {
            bool validStart = (searchPos == 0 || !std::isalnum(line[searchPos - 1]));
            bool validEnd = (searchPos + word.size() >= line.size() ||
                            !std::isalnum(line[searchPos + word.size()]));
            if (validStart && validEnd) {
                Location loc;
                loc.uri = uri;
                loc.range.start.line = lineNo;
                loc.range.start.character = static_cast<int>(searchPos);
                loc.range.end.line = lineNo;
                loc.range.end.character = static_cast<int>(searchPos + word.size());
                result.push_back(loc);
            }
            searchPos += word.size();
        }
        lineNo++;
    }
    return result;
}

// ============================================================
//  Rename
// ============================================================

std::map<std::string, std::vector<std::pair<Range, std::string>>> LSPServer::rename(
    const std::string& uri, Position pos, const std::string& newName) {
    std::map<std::string, std::vector<std::pair<Range, std::string>>> edits;
    auto refs = references(uri, pos);
    for (auto& ref : refs) {
        edits[ref.uri].push_back({ref.range, newName});
    }
    return edits;
}

} // namespace lsp
} // namespace vyx
