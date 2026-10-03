#pragma once
#include "Type.h"
#include "../Common/SourceLocation.h"
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <optional>

namespace vyx {

/// Function-specific symbol fields (only meaningful when isFunction == true)
struct FunctionInfo {
    std::vector<VyxTypePtr> paramTypes;
    VyxTypePtr returnType;
    size_t paramCount = 0;
    bool isGeneric = false;
    size_t genericArity = 0;
    bool isUnsafe = false;
    bool borrowsArgs = false;
};

/// Ownership/borrow tracking fields (only meaningful for local variables)
struct BorrowInfo {
    bool isMoved = false;
    bool isBorrowed = false;
    int borrowCount = 0;
    int mutBorrowCount = 0;
    bool escapes = false;
    int scopeDepth = 0;
    int borrowScopeDepth = 0;
    // P4-B: location of the statement that performed the move (var-decl RHS,
    // assignment RHS, call-by-value arg, return expr, match scrutinee). Set
    // when `isMoved` flips to true so the V-MOVE-001 diagnostic can point
    // back at the move site with a `note:` line. Default-constructed
    // SourceLocation (line==0) is treated as "unknown".
    SourceLocation moveLocation;
};

/// Symbol table entry. Inherits FunctionInfo and BorrowInfo as mixins
/// so all fields remain directly accessible for backward compatibility.
struct Symbol : FunctionInfo, BorrowInfo {
    std::string name;
    VyxTypePtr type;
    bool isConst = false;
    bool isMutable = false;
    SourceLocation declLocation;

    bool isFunction = false;
    bool isImported = false;
    bool isExported = false;

    bool isDeprecated = false;
    std::string deprecationMsg;

    bool isSend = true;
    bool isSync = true;
};

class Scope {
public:
    explicit Scope(std::shared_ptr<Scope> parent = nullptr)
        : parent_(std::move(parent)) {}

    bool declare(const std::string& name, Symbol sym) {
        if (symbols_.count(name)) return false;
        symbols_[name] = std::move(sym);
        return true;
    }

    Symbol* lookup(const std::string& name) {
        auto it = symbols_.find(name);
        if (it != symbols_.end()) return &it->second;
        if (parent_) return parent_->lookup(name);
        return nullptr;
    }

    Symbol* lookupLocal(const std::string& name) {
        auto it = symbols_.find(name);
        if (it != symbols_.end()) return &it->second;
        return nullptr;
    }

    template <typename F>
    void forEachLocal(F&& fn) {
        for (auto& [name, sym] : symbols_) fn(name, sym);
    }

    std::shared_ptr<Scope> parent() const { return parent_; }

private:
    std::shared_ptr<Scope> parent_;
    std::map<std::string, Symbol> symbols_;
};

class SymbolTable {
public:
    SymbolTable() : current_(std::make_shared<Scope>()) {}

    void pushScope() {
        current_ = std::make_shared<Scope>(current_);
        scopeDepth_++;
    }

    void popScope() {
        if (current_->parent()) {
            releaseBorrowsInScope(scopeDepth_);
            current_ = current_->parent();
            scopeDepth_--;
        }
    }

    int currentScopeDepth() const { return scopeDepth_; }

    bool declare(const std::string& name, Symbol sym) {
        return current_->declare(name, std::move(sym));
    }

    Symbol* lookup(const std::string& name) {
        return current_->lookup(name);
    }

    // P4-C scope-aware auto-drop: iterate symbols declared in the CURRENT
    // scope only (does not descend into parent scopes). Used by Sema's
    // analyzeBlock to collect heap-owning unmoved/unescaped locals into
    // BlockStmt::autoDropLocals right before popScope.
    template <typename F>
    void forEachLocalSymbol(F&& fn) {
        if (!current_) return;
        current_->forEachLocal(std::forward<F>(fn));
    }

    // Type registry
    void registerType(const std::string& name, VyxTypePtr type) {
        typeRegistry_[name] = std::move(type);
    }

    void unregisterType(const std::string& name) {
        typeRegistry_.erase(name);
    }

    VyxTypePtr lookupType(const std::string& name) const {
        auto it = typeRegistry_.find(name);
        if (it != typeRegistry_.end()) return it->second;
        return nullptr;
    }

    void trackBorrow(const std::string& symbolName, int depth, bool isMut = false) {
        activeBorrows_.push_back({symbolName, depth, isMut});
    }

private:
    void releaseBorrowsInScope(int depth) {
        std::erase_if(activeBorrows_, [&](const BorrowRecord& br) {
            if (br.scopeDepth >= depth) {
                auto* sym = lookup(br.symbolName);
                if (sym) {
                    if (sym->borrowCount > 0) {
                        sym->borrowCount--;
                        if (sym->borrowCount == 0) sym->isBorrowed = false;
                    }
                    if (br.isMutable && sym->mutBorrowCount > 0) {
                        sym->mutBorrowCount--;
                    }
                }
                return true;
            }
            return false;
        });
    }

    struct BorrowRecord {
        std::string symbolName;
        int scopeDepth;
        bool isMutable = false;
    };

    std::shared_ptr<Scope> current_;
    std::map<std::string, VyxTypePtr> typeRegistry_;
    std::vector<BorrowRecord> activeBorrows_;
    int scopeDepth_ = 0;
};

} // namespace vyx
