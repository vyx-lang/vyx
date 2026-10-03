#pragma once
#include "../Parser/AST.h"
#include "TemplateResolver.h"
#include <functional>

namespace vyx {

// ── Pure structural clones (no substitution) ─────────────────────────────
ExprPtr cloneExpr(const Expr& expr);
StmtPtr cloneStatement(const Stmt& stmt);
TypePtr cloneTypeAnnotation(const TypeAnnotation& type);

// ── Type-substituting clones ──────────────────────────────────────────────
//
// These variants walk every TypeAnnotation encountered in the subtree and
// replace it via:
//   1. TemplateResolver::substituteType(*ann, subst)  →  VyxTypePtr
//   2. reifier(vyxTypePtr)                             →  TypePtr (AST node)
//
// The `reifier` callback is typically `Sema::convertTypeToAnnotation`.
// It is threaded as a std::function so these helpers do not need to depend
// on the full Sema header (which would create a circular include).
//
// All non-type-bearing sub-nodes are cloned with identical structural shape
// to their pure-clone counterparts above.

using TypeReifier = std::function<TypePtr(const VyxTypePtr&)>;

TypePtr  cloneTypeAnnotationWithSubst(const TypeAnnotation& type,
                                      TemplateResolver& tr,
                                      const GenericSubstitution& subst,
                                      const TypeReifier& reify);

ExprPtr  cloneExprWithSubst(const Expr& expr,
                             TemplateResolver& tr,
                             const GenericSubstitution& subst,
                             const TypeReifier& reify);

StmtPtr  cloneStatementWithSubst(const Stmt& stmt,
                                  TemplateResolver& tr,
                                  const GenericSubstitution& subst,
                                  const TypeReifier& reify);

} // namespace vyx
