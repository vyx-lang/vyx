#include "ImportResolver.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace vyx {

namespace {

// Simple Levenshtein distance between two strings. Used to suggest
// similar module names when a `use X.Y.Z;` target has no match.
size_t levenshtein(const std::string& a, const std::string& b) {
    const size_t n = a.size();
    const size_t m = b.size();
    if (n == 0) return m;
    if (m == 0) return n;
    std::vector<size_t> prev(m + 1), curr(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({
                prev[j] + 1,          // deletion
                curr[j - 1] + 1,      // insertion
                prev[j - 1] + cost    // substitution
            });
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

// Pick up to `maxCount` module names from `candidates` that are most
// similar to `target` via Levenshtein distance. Only returns candidates
// whose distance is small enough (<= max(2, target.size()/2)) so we
// don't surface wildly unrelated names.
std::vector<std::string> similarModules(const std::string& target,
                                         const std::vector<std::string>& candidates,
                                         size_t maxCount = 3) {
    // Threshold: either <= 2 edits, or half the target length, whichever is larger.
    size_t threshold = std::max<size_t>(2, target.size() / 2);
    std::vector<std::pair<size_t, std::string>> scored;
    for (auto& c : candidates) {
        size_t d = levenshtein(target, c);
        if (d <= threshold) scored.emplace_back(d, c);
    }
    std::sort(scored.begin(), scored.end(),
              [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<std::string> result;
    for (auto& [_, n] : scored) {
        result.push_back(n);
        if (result.size() >= maxCount) break;
    }
    return result;
}

std::string lowerPathName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name;
}

bool shouldSkipRegistryDir(const fs::path& path) {
    std::string name = lowerPathName(path.filename().string());
    if (name.empty()) return false;
    if (name == ".git" || name == ".cache") return true;
    if (name == "target" || name == "out" || name == "build") return true;
    if (name.rfind("build_", 0) == 0) return true;
    if (name.rfind("cmake-build", 0) == 0) return true;
    return false;
}

} // namespace

std::expected<std::string, std::string> readSourceFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return std::unexpected("cannot open file '" + path + "'");
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    return content;
}

// ============================================================
//  ModuleRegistry
// ============================================================

void ModuleRegistry::scanDirectory(const fs::path& dir) {
    if (!fs::is_directory(dir)) return;
    std::string key = fs::canonical(dir).string();
    if (scannedDirs_.count(key)) return;
    scannedDirs_.insert(key);

    fs::recursive_directory_iterator it(
        dir, fs::directory_options::skip_permission_denied);
    fs::recursive_directory_iterator end;
    for (; it != end; ++it) {
        std::error_code ec;
        if (it->is_directory(ec)) {
            if (shouldSkipRegistryDir(it->path()))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        auto& entry = *it;
        auto ext = entry.path().extension().string();
        if (ext != ".vyx" && ext != ".vyi") continue;
        std::ifstream f(entry.path());
        if (!f.is_open()) continue;
        std::string firstLine;
        std::getline(f, firstLine);
        if (firstLine.starts_with("module ")) {
            auto semi = firstLine.find(';');
            if (semi != std::string::npos) {
                std::string modName = firstLine.substr(7, semi - 7);
                // De-duplicate by (moduleName, file stem): a project may contain
                // multiple copies of the same stdlib file (e.g. a vendored `std/`
                // copy plus the primary `std/`) both declaring `module std.collections`;
                // importing all of them produces ODR conflicts (duplicate class Vec).
                // We keep the first occurrence per (module, stem) pair; earlier scan
                // order reflects caller priority (exeDir/std > cwd/std > entry/std).
                std::string stem = entry.path().stem().string();
                auto& files = registry_[modName];
                bool exists = false;
                for (auto& existing : files) {
                    if (fs::path(existing).stem().string() == stem) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) files.push_back(entry.path().string());
            }
        }
    }
}

void ModuleRegistry::scanDirectories(const std::vector<fs::path>& dirs) {
    for (auto& dir : dirs) scanDirectory(dir);
}

std::vector<std::string> ModuleRegistry::filesForModule(const std::string& moduleName) const {
    auto it = registry_.find(moduleName);
    return it != registry_.end() ? it->second : std::vector<std::string>{};
}

std::vector<fs::path> moduleSearchRoots(const fs::path& entryPath,
                                        const fs::path& executableDir) {
    const fs::path absoluteEntry = fs::absolute(entryPath);
    const fs::path sourceDir = absoluteEntry.parent_path();
    const fs::path projectDir = sourceDir.parent_path();
    const fs::path workingDir = fs::current_path();

    // `project/src/main.vyx` conventionally consumes `project/std/`.
    // Keep source-local std/ first for the less common colocated layout.
    return {
        sourceDir / "std",
        projectDir / "std",
        workingDir / "std",
        executableDir.empty() ? fs::path{} : executableDir / "std",
        workingDir,
        sourceDir,
    };
}

// ============================================================
//  ImportResolver
// ============================================================

ImportResolver::ImportResolver(DiagnosticsEngine& diag, const ModuleRegistry& registry)
    : diag_(diag), registry_(registry) {}

void ImportResolver::resolve(TranslationUnit& unit, const fs::path& entryPath) {
    fs::path absEntry = fs::absolute(entryPath);
    std::string canonEntry = fs::canonical(absEntry).string();
    importedFiles_.insert(canonEntry);
    importChain_.push_back(canonEntry);

    resolveUseImports(unit);
    resolveFileImports(unit.declarations, absEntry);
    // Diagnose `use X.Y.Z;` statements at the entry TU whose target matched
    // nothing in either the module registry or the file-path fallback.
    // Nested imports (inside auto-loaded files) are already scoped by their
    // own module-cohesion logic; we only flag user-visible imports here.
    diagnoseUnknownImports(unit, absEntry);

    if (!allImported_.empty()) {
        unit.declarations.insert(unit.declarations.begin(),
            std::make_move_iterator(allImported_.begin()),
            std::make_move_iterator(allImported_.end()));
        allImported_.clear();
    }

    // C#-style fully-qualified paths: rewrite dotted chains only for modules
    // that were already imported explicitly. This pass never loads std or
    // any other module by itself.
    rewriteDottedModulePaths(unit);
    if (!allImported_.empty()) {
        unit.declarations.insert(unit.declarations.begin(),
            std::make_move_iterator(allImported_.begin()),
            std::make_move_iterator(allImported_.end()));
        allImported_.clear();
    }

    // P4-A.2 fix: auto-qualify every top-level type decl sitting inside
    // a file-scope `module std.X;` header. This happens AFTER every
    // import has been stitched in (so cross-file refs like `Vec` from
    // std/collections.vyx to std/vec.vyx's `class Vec<T>` see the same
    // rename table) and AFTER rewriteDottedModulePaths (whose
    // `std.collections.Vec` → `Vec` collapse emits bare short-name
    // Identifiers that this pass then re-qualifies to
    // `std.collections.Vec`). User-side files are left untouched except
    // for bare-short-name references that match a std-only type; a user
    // who declares their own `struct StringBuilder` keeps their local
    // decl and their local references intact (see the userTypeNames
    // filter in autoQualifyStdModuleGlobal).
    Parser::autoQualifyStdModuleGlobal(unit);
}

void ImportResolver::resolveUseImports(TranslationUnit& unit) {
    for (auto& decl : unit.declarations) {
        if (!decl || decl->kind != DeclKind::Import) continue;
        auto* imp = decl->as<ImportDecl>();
        if (!imp->importNames.empty() && imp->importNames[0] == "__module") continue;
        if (imp->importPath.empty()) continue;

        std::string modPath;
        for (size_t i = 0; i < imp->importPath.size(); ++i) {
            if (i > 0) modPath += ".";
            modPath += imp->importPath[i];
        }

        for (auto& f : registry_.filesForModule(modPath))
            importFile(f);
    }
}

void ImportResolver::resolveFileImports(const std::vector<DeclPtr>& decls,
                                         const fs::path& fromPath) {
    for (auto& d : decls) {
        if (!d || d->kind != DeclKind::Import) continue;
        auto* imp = d->as<ImportDecl>();
        if (!imp->importNames.empty() && imp->importNames[0] == "__module") continue;

        std::string ip;
        std::string modPath;
        for (size_t i = 0; i < imp->importPath.size(); ++i) {
            if (i > 0) ip += '/';
            ip += imp->importPath[i];
            if (i > 0) modPath += ".";
            modPath += imp->importPath[i];
        }
        ip += ".vyx";

        // Module resolution is authoritative. In particular, a bootstrap
        // source under `project/src/` must keep using `project/std/` after
        // the registry selected it; do not subsequently pull in an unrelated
        // `cwd/std/<module>.vyx` through the file-path fallback.
        if (!registry_.filesForModule(modPath).empty()) continue;

        fs::path impFile;
        fs::path c1 = fromPath.parent_path() / ip;
        fs::path c2 = fs::current_path() / ip;
        if (fs::exists(c1)) impFile = c1;
        else if (fs::exists(c2)) impFile = c2;

        if (impFile.empty()) {
            diag_.error(imp->location,
                "import '{}' could not be resolved; tried '{}' and '{}' "
                "(no module '{}' registered either)",
                ip, c1.string(), c2.string(), modPath);
            continue;
        }

        std::string cp = fs::canonical(impFile).string();

        if (std::ranges::find(importChain_, cp) != importChain_.end()) {
            std::cerr << "error: circular import detected:\n";
            for (auto& f : importChain_) std::cerr << "  -> " << f << '\n';
            std::cerr << "  -> " << cp << " (cycle)\n";
            continue;
        }

        if (importedFiles_.count(cp)) continue;
        importedFiles_.insert(cp);
        importChain_.push_back(cp);

        auto result = readSourceFile(impFile.string());
        if (!result) {
            std::cerr << "error: " << result.error() << '\n';
            importChain_.pop_back();
            continue;
        }
        sourceBuffers_.push_back(std::move(*result));
        auto& isrc = sourceBuffers_.back();
        if (!isrc.empty()) {
            Lexer il(isrc, impFile.string(), diag_);
            auto itk = il.tokenizeAll();
            Parser ip2(std::move(itk), diag_);
            auto iu = ip2.parseTranslationUnit(impFile.string());

            // Module-cohesion auto-load (same as importFile): pull all
            // siblings declaring the same module, so cross-file references
            // (`Vec<T>` in vec.vyx referenced by `public data: Vec<i64>`
            // in bitset.vyx) resolve.
            std::string ownModule;
            for (auto& d : iu.declarations) {
                if (!d || d->kind != DeclKind::Import) continue;
                auto* imp = d->as<ImportDecl>();
                if (!imp->importNames.empty() &&
                    imp->importNames[0] == "__module" &&
                    !imp->importPath.empty()) {
                    ownModule = imp->importPath[0];
                    break;
                }
            }
            if (!ownModule.empty()) {
                for (auto& f : registry_.filesForModule(ownModule))
                    importFile(f);
            }

            resolveUseImports(iu);
            resolveFileImports(iu.declarations, impFile);
            for (auto& id : iu.declarations) {
                if (id) { id->isImported = true; allImported_.push_back(std::move(id)); }
            }
            std::cerr << "  imported " << impFile.string() << '\n';
        }

        importChain_.pop_back();
    }
}

void ImportResolver::diagnoseUnknownImports(TranslationUnit& unit,
                                             const fs::path& fromPath) {
    for (auto& decl : unit.declarations) {
        if (!decl || decl->kind != DeclKind::Import) continue;
        auto* imp = decl->as<ImportDecl>();
        // Skip synthetic `module X.Y;` markers — these carry `__module` as
        // the first "import name" and are not user-visible imports.
        if (!imp->importNames.empty() && imp->importNames[0] == "__module") continue;
        if (imp->importPath.empty()) continue;

        // Skip if this import-decl was injected by a previously-imported
        // file (they already resolved against their own source's context).
        if (imp->isImported) continue;

        // Reconstruct the dotted path, e.g. "std.collections".
        std::string modPath;
        for (size_t i = 0; i < imp->importPath.size(); ++i) {
            if (i > 0) modPath += ".";
            modPath += imp->importPath[i];
        }

        // 1) Module-registry exact match?
        if (!registry_.filesForModule(modPath).empty()) continue;

        // 2) File-path fallback: matches `resolveFileImports` logic. A
        //    `use a.b.c;` also accepts `a/b/c.vyx` relative to the source
        //    file's directory OR the current working directory. This keeps
        //    e.g. `use std.iter;` working whenever the file exists on disk
        //    (even though its declared module is `std.collections`).
        std::string relPath;
        for (size_t i = 0; i < imp->importPath.size(); ++i) {
            if (i > 0) relPath += '/';
            relPath += imp->importPath[i];
        }
        relPath += ".vyx";
        fs::path c1 = fromPath.parent_path() / relPath;
        fs::path c2 = fs::current_path() / relPath;
        if (fs::exists(c1) || fs::exists(c2)) continue;

        // No registry match AND no file-path match → the import target is
        // unknown. Emit a clear diagnostic with a best-effort suggestion
        // drawn from registered module names.
        std::vector<std::string> allMods;
        allMods.reserve(registry_.modules().size());
        for (auto& [mod, _files] : registry_.modules()) allMods.push_back(mod);
        auto suggestions = similarModules(modPath, allMods);

        diag_.error(decl->location,
                    "unknown module '{}' (no file declares `module {};`)",
                    modPath, modPath);
        if (!suggestions.empty()) {
            std::string list;
            for (size_t i = 0; i < suggestions.size(); ++i) {
                if (i > 0) list += ", ";
                list += suggestions[i];
            }
            diag_.note(decl->location,
                       "similar available modules: {}", list);
        }
    }
}

void ImportResolver::importFile(const std::string& filePath) {
    std::string cp = fs::canonical(filePath).string();
    if (importedFiles_.count(cp)) return;
    importedFiles_.insert(cp);

    auto result = readSourceFile(filePath);
    if (!result) return;
    sourceBuffers_.push_back(std::move(*result));
    auto& isrc = sourceBuffers_.back();
    if (!isrc.empty()) {
        Lexer il(isrc, filePath, diag_);
        auto itk = il.tokenizeAll();
        Parser ip2(std::move(itk), diag_);
        auto iu = ip2.parseTranslationUnit(filePath);

        // Module-cohesion auto-load: when an imported file declares
        // `module X.Y`, pull in every other file declaring the same module
        // so that sibling definitions (e.g. `Vec<T>` in vec.vyx, referenced
        // by a field in bitset.vyx) are visible. Without this, `use std.bitset;`
        // loads bitset.vyx alone and its `public data: Vec<i64>` field
        // resolves to an undefined `Vec`.
        std::string ownModule;
        for (auto& d : iu.declarations) {
            if (!d || d->kind != DeclKind::Import) continue;
            auto* imp = d->as<ImportDecl>();
            if (!imp->importNames.empty() &&
                imp->importNames[0] == "__module" &&
                !imp->importPath.empty()) {
                ownModule = imp->importPath[0];
                break;
            }
        }
        if (!ownModule.empty()) {
            for (auto& f : registry_.filesForModule(ownModule))
                importFile(f);
        }

        resolveUseImports(iu);
        resolveFileImports(iu.declarations, fs::path(filePath));
        for (auto& id : iu.declarations) {
            if (id) { id->isImported = true; allImported_.push_back(std::move(id)); }
        }
    }
}

// ============================================================
//  Dotted module-path rewrite
// ============================================================
//
// Walks every expression in the TU. When it sees a MemberAccess chain
// rooted at an Identifier whose name matches an explicitly imported module
// prefix (e.g. "std" is the root of "std.collections"), the chain's module
// prefix is stripped and the expression is rewritten to reference just
// the final Vyx-visible symbol. The pass does not import modules itself.
//
// Example transforms:
//   std.collections.Vec::<i64>.new()
//     → Vec::<i64>.new()   [requires explicit `use std.collections;`]
//   std.core.panic("boom")
//     → panic("boom")      [requires explicit `use std.core;`]
//
// Only MemberAccess chains qualify; raw Identifier `std` (used as a
// value) still errors downstream. Chain traversal stops at the first
// segment that matches the longest registered module path.

namespace {

// Collect the dotted path of a pure Identifier+MemberAccess chain. Ignores
// callTypeArgs on MemberAccess (the chain identity isn't affected by them).
// Returns "" if any link is not a MemberAccess / Identifier.
std::string collectDottedChain(const Expr* e) {
    if (!e) return "";
    if (e->kind == ExprKind::Identifier) {
        auto* id = e->as<const IdentifierExpr>();
        return id->name;
    }
    if (e->kind == ExprKind::MemberAccess) {
        auto* ma = e->as<const MemberAccessExpr>();
        std::string inner = collectDottedChain(ma->object.get());
        if (inner.empty()) return "";
        return inner + "." + ma->member;
    }
    return "";
}

// Depth of the dotted chain (count of `.` + 1 = segment count).
size_t dotSegments(const std::string& s) {
    if (s.empty()) return 0;
    return static_cast<size_t>(std::count(s.begin(), s.end(), '.')) + 1;
}

} // namespace

void ImportResolver::rewriteDottedModulePaths(TranslationUnit& unit) {
    // Collect registered modules from the filesystem-backed registry, PLUS
    // any namespaces declared inline by the user (either file-wide
    // `module Foo.Bar;` or the C#-style block form `module Foo.Bar { ... }`).
    // Both forms emit a `__module Foo.Bar` sentinel via ImportDecl, which
    // we harvest below so `Foo.Bar.Point.new()` rewrites even when nothing
    // in the filesystem registers `Foo.Bar` as a module.
    std::map<std::string, std::vector<std::string>> localModules;
    std::set<std::string> explicitModules;
    for (auto& decl : unit.declarations) {
        if (!decl || decl->kind != DeclKind::Import) continue;
        auto* imp = decl->as<ImportDecl>();
        if (imp->importNames.empty()) continue;
        if (imp->importNames[0] == "__module" && !imp->importPath.empty()) {
            localModules.emplace(imp->importPath.back(), std::vector<std::string>{});
            // Also register each dotted prefix so `Foo.Bar.Point.new()` has
            // "Foo" and "Foo.Bar" both resolvable. This matches the behaviour
            // of filesystem-registered module hierarchies.
            const auto& full = imp->importPath.back();
            for (size_t pos = full.find('.'); pos != std::string::npos;
                 pos = full.find('.', pos + 1)) {
                localModules.emplace(full.substr(0, pos), std::vector<std::string>{});
            }
        } else if (!imp->importPath.empty()) {
            std::string modPath;
            for (size_t i = 0; i < imp->importPath.size(); ++i) {
                if (i > 0) modPath += ".";
                modPath += imp->importPath[i];
            }
            if (!modPath.empty()) explicitModules.insert(modPath);
        }
    }

    if (registry_.modules().empty() && localModules.empty()) return;

    // Collect root names that begin any registered module path. If the
    // user writes `foo.bar.Baz` but no registered module starts with
    // "foo", the chain is left alone (Sema errors as before).
    std::set<std::string> moduleRoots;
    for (auto& [mod, _files] : registry_.modules()) {
        if (!explicitModules.count(mod)) continue;
        auto dot = mod.find('.');
        if (dot == std::string::npos) moduleRoots.insert(mod);
        else                          moduleRoots.insert(mod.substr(0, dot));
    }
    for (auto& [mod, _] : localModules) {
        auto dot = mod.find('.');
        if (dot == std::string::npos) moduleRoots.insert(mod);
        else                          moduleRoots.insert(mod.substr(0, dot));
    }
    if (moduleRoots.empty()) return;

    // Recursive helper that attempts to rewrite `slot` in-place when it
    // contains a module-prefixed chain. Returns true if any rewrite happened
    // somewhere in the subtree (for debug visibility only — callers don't use
    // the return value here).
    std::function<void(ExprPtr&)> rewriteExpr;

    auto tryCollapseChain = [&](ExprPtr& slot) {
        // Collapse at any level: if `slot`'s object's dotted chain equals a
        // registered module path, drop the module prefix. Iterative so we
        // handle deeper wrappers like `.new` after the module path.
        while (slot && slot->kind == ExprKind::MemberAccess) {
            auto* ma = slot->as<MemberAccessExpr>();
            if (!ma->object) break;
            std::string objChain = collectDottedChain(ma->object.get());
            if (objChain.empty()) break;
            // Root must be a known module root to avoid misfiring on user
            // expressions like `obj.field.subfield`.
            auto firstDot = objChain.find('.');
            std::string root = firstDot == std::string::npos ? objChain : objChain.substr(0, firstDot);
            if (!moduleRoots.count(root)) break;

            // Find longest registered module prefix that the object chain
            // fully spans. e.g. for object chain "std.collections" against
            // modules {"std", "std.collections"}, pick "std.collections".
            // Both filesystem-registered modules and user-declared inline
            // namespaces (harvested above as `localModules`) participate.
            std::string bestMod;
            size_t bestDepth = 0;
            bool bestIsLocal = false;
            auto consider = [&](const std::string& mod, bool isLocal) {
                if (!isLocal && !explicitModules.count(mod)) return;
                if (objChain == mod) {
                    size_t d = dotSegments(mod);
                    if (d > bestDepth) {
                        bestDepth = d;
                        bestMod = mod;
                        bestIsLocal = isLocal;
                    }
                }
            };
            for (auto& [mod, _files] : registry_.modules()) consider(mod, false);
            for (auto& [mod, _] : localModules) consider(mod, true);
            if (bestMod.empty()) break;

            // Collapse the chain into a single Identifier. For filesystem-
            // registered modules the member is addressable by its bare
            // short name (the parser never renamed `class Vec<T>` to
            // `collections.Vec<T>`), so we drop the module prefix. For
            // user-declared block-form namespaces the parser DID rename
            // the decl to `Foo.Bar.Point`, so we must keep the full
            // qualified name as the identifier for Sema's symbol lookup
            // to hit the right entry.
            auto ident = std::make_unique<IdentifierExpr>();
            ident->location = slot->location;
            if (bestIsLocal) {
                ident->name = bestMod + "." + ma->member;
            } else {
                ident->name = ma->member;
            }
            ident->callTypeArgs = std::move(ma->callTypeArgs);
            slot = std::move(ident);
            break;
        }
    };

    rewriteExpr = [&](ExprPtr& slot) {
        if (!slot) return;
        tryCollapseChain(slot);
        if (!slot) return;
        switch (slot->kind) {
            case ExprKind::MemberAccess: {
                auto* ma = slot->as<MemberAccessExpr>();
                rewriteExpr(ma->object);
                // After rewriting the inner object (which may have been
                // collapsed from `MA(MA, Solid)` into `Ident("Shapes.Solid")`
                // or similar), retry collapsing at this level: e.g.
                // `MA(Ident("Shapes.Solid"), "Cube")` now matches the
                // registered `Shapes.Solid` namespace and should fold
                // into `Ident("Shapes.Solid.Cube")`. Without this pass
                // the outer `.Cube` stays as a dangling MemberAccess and
                // Sema sees `Shapes.Solid.Cube` as a field access on the
                // non-existent symbol `Shapes.Solid`.
                tryCollapseChain(slot);
                break;
            }
            case ExprKind::Call: {
                auto* c = slot->as<CallExpr>();
                rewriteExpr(c->callee);
                for (auto& a : c->args) rewriteExpr(a);
                break;
            }
            case ExprKind::BinaryOp: {
                auto* b = slot->as<BinaryOpExpr>();
                rewriteExpr(b->lhs);
                rewriteExpr(b->rhs);
                break;
            }
            case ExprKind::UnaryOp: {
                auto* u = slot->as<UnaryOpExpr>();
                rewriteExpr(u->operand);
                break;
            }
            case ExprKind::Index: {
                auto* ix = slot->as<IndexExpr>();
                rewriteExpr(ix->object);
                rewriteExpr(ix->indexExpr);
                break;
            }
            case ExprKind::Assignment: {
                auto* a = slot->as<AssignmentExpr>();
                rewriteExpr(a->lhs);
                rewriteExpr(a->rhs);
                break;
            }
            case ExprKind::CompoundAssignment: {
                auto* a = slot->as<CompoundAssignmentExpr>();
                rewriteExpr(a->target);
                rewriteExpr(a->value);
                break;
            }
            case ExprKind::Cast: {
                auto* c = slot->as<CastExpr>();
                rewriteExpr(c->operand);
                break;
            }
            case ExprKind::Ternary: {
                auto* t = slot->as<TernaryExpr>();
                rewriteExpr(t->condition);
                rewriteExpr(t->trueExpr);
                rewriteExpr(t->falseExpr);
                break;
            }
            case ExprKind::TryExpr: {
                auto* t = slot->as<TryExpr>();
                rewriteExpr(t->inner);
                break;
            }
            case ExprKind::ArrayInit: {
                auto* a = slot->as<ArrayInitExpr>();
                for (auto& el : a->elements) rewriteExpr(el);
                rewriteExpr(a->repeatCount);
                break;
            }
            case ExprKind::TupleInit: {
                auto* t = slot->as<TupleInitExpr>();
                for (auto& el : t->elements) rewriteExpr(el);
                break;
            }
            case ExprKind::StructInit: {
                auto* s = slot->as<StructInitExpr>();
                for (auto& [_, v] : s->fieldInits) rewriteExpr(v);
                rewriteExpr(s->spreadBase);
                break;
            }
            case ExprKind::StringInterpolation: {
                auto* si = slot->as<StringInterpExpr>();
                for (auto& p : si->parts) rewriteExpr(p.expr);
                break;
            }
            default: break;
        }
    };

    // Walk statements recursively to find every expression site.
    std::function<void(Stmt&)> walkStmt;
    walkStmt = [&](Stmt& s) {
        switch (s.kind) {
            case StmtKind::Block: {
                auto& b = static_cast<BlockStmt&>(s);
                for (auto& sub : b.statements) if (sub) walkStmt(*sub);
                break;
            }
            case StmtKind::VarDecl: {
                auto& v = static_cast<VarDeclStmt&>(s);
                if (v.initExpr) rewriteExpr(v.initExpr);
                break;
            }
            case StmtKind::ExprStmt: {
                auto& e = static_cast<ExprStmt&>(s);
                if (e.expr) rewriteExpr(e.expr);
                break;
            }
            case StmtKind::Return: {
                auto& r = static_cast<ReturnStmt&>(s);
                if (r.expr) rewriteExpr(r.expr);
                break;
            }
            case StmtKind::If: {
                auto& i = static_cast<IfStmt&>(s);
                if (i.condition) rewriteExpr(i.condition);
                if (i.thenBranch) walkStmt(*i.thenBranch);
                for (auto& [cond, body] : i.elifBranches) {
                    if (cond) rewriteExpr(cond);
                    if (body) walkStmt(*body);
                }
                if (i.elseBranch) walkStmt(*i.elseBranch);
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<WhileStmt&>(s);
                if (w.condition) rewriteExpr(w.condition);
                if (w.body) walkStmt(*w.body);
                break;
            }
            case StmtKind::For: {
                auto& f = static_cast<ForStmt&>(s);
                if (f.init) walkStmt(*f.init);
                if (f.condition) rewriteExpr(f.condition);
                if (f.step) rewriteExpr(f.step);
                if (f.body) walkStmt(*f.body);
                break;
            }
            case StmtKind::ForEach: {
                auto& fe = static_cast<ForEachStmt&>(s);
                if (fe.collection) rewriteExpr(fe.collection);
                if (fe.body) walkStmt(*fe.body);
                break;
            }
            case StmtKind::Match: {
                auto& m = static_cast<MatchStmt&>(s);
                if (m.expr) rewriteExpr(m.expr);
                for (auto& arm : m.arms) {
                    if (arm.valuePattern) rewriteExpr(arm.valuePattern);
                    if (arm.guardExpr) rewriteExpr(arm.guardExpr);
                    if (arm.body) walkStmt(*arm.body);
                }
                break;
            }
            case StmtKind::Assignment: {
                auto& a = static_cast<AssignStmt&>(s);
                if (a.target) rewriteExpr(a.target);
                if (a.value) rewriteExpr(a.value);
                break;
            }
            case StmtKind::Defer: {
                auto& d = static_cast<DeferStmt&>(s);
                if (d.body) walkStmt(*d.body);
                break;
            }
            default: break;
        }
    };

    auto walkMethod = [&](MethodDecl& m) {
        if (m.body) walkStmt(*m.body);
        for (auto& p : m.params) if (p.defaultValue) rewriteExpr(p.defaultValue);
    };

    for (auto& decl : unit.declarations) {
        if (!decl) continue;
        switch (decl->kind) {
            case DeclKind::Function: {
                auto& fd = *decl->as<FunctionDecl>();
                if (fd.body) walkStmt(*fd.body);
                for (auto& p : fd.params) if (p.defaultValue) rewriteExpr(p.defaultValue);
                break;
            }
            case DeclKind::Class: {
                auto& cd = *decl->as<ClassDecl>();
                for (auto& m : cd.methods) walkMethod(m);
                for (auto& f : cd.fields) if (f.defaultValue) rewriteExpr(f.defaultValue);
                break;
            }
            case DeclKind::Struct: {
                auto& sd = *decl->as<StructDecl>();
                for (auto& f : sd.fields) if (f.defaultValue) rewriteExpr(f.defaultValue);
                break;
            }
            case DeclKind::GlobalVar: {
                auto& gv = *decl->as<GlobalVarDecl>();
                if (gv.initBody) walkStmt(*gv.initBody);
                break;
            }
            default: break;
        }
    }

}

} // namespace vyx
