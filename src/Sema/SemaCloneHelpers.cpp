#include "SemaCloneHelpers.h"
#include "TemplateResolver.h"

namespace vyx {

TypePtr cloneTypeAnnotation(const TypeAnnotation& type) {
    switch (type.kind) {
        case TypeAnnotationKind::Named: {
            auto cloned = std::make_unique<NamedType>();
            cloned->location = type.location;
            cloned->name = type.name;
            return cloned;
        }
        case TypeAnnotationKind::Pointer: {
            auto* src = type.as<PointerType>();
            auto cloned = std::make_unique<PointerType>();
            cloned->location = type.location;
            cloned->name = type.name;
            if (src->innerType) cloned->innerType = cloneTypeAnnotation(*src->innerType);
            return cloned;
        }
        case TypeAnnotationKind::Reference: {
            auto* src = type.as<ReferenceType>();
            auto cloned = std::make_unique<ReferenceType>();
            cloned->location = type.location;
            cloned->name = type.name;
            cloned->isMutable = src->isMutable;
            if (src->innerType) cloned->innerType = cloneTypeAnnotation(*src->innerType);
            return cloned;
        }
        case TypeAnnotationKind::Array: {
            auto* src = type.as<ArrayType>();
            auto cloned = std::make_unique<ArrayType>();
            cloned->location = type.location;
            cloned->name = type.name;
            if (src->elementType) cloned->elementType = cloneTypeAnnotation(*src->elementType);
            if (src->size) cloned->size = cloneExpr(*src->size);
            return cloned;
        }
        case TypeAnnotationKind::Tuple: {
            auto* src = type.as<TupleType>();
            auto cloned = std::make_unique<TupleType>();
            cloned->location = type.location;
            cloned->name = type.name;
            for (const auto& elem : src->elements) {
                if (elem) cloned->elements.push_back(cloneTypeAnnotation(*elem));
            }
            return cloned;
        }
        case TypeAnnotationKind::Function: {
            auto* src = type.as<FunctionType>();
            auto cloned = std::make_unique<FunctionType>();
            cloned->location = type.location;
            cloned->name = type.name;
            for (const auto& pt : src->paramTypes) {
                if (pt) cloned->paramTypes.push_back(cloneTypeAnnotation(*pt));
            }
            if (src->returnType) cloned->returnType = cloneTypeAnnotation(*src->returnType);
            return cloned;
        }
        case TypeAnnotationKind::Generic: {
            auto* src = type.as<GenericType>();
            auto cloned = std::make_unique<GenericType>();
            cloned->location = type.location;
            cloned->name = type.name;
            // Preserve slot alignment with argExprs: const-value slots have
            // typeArgs[i]==nullptr and argExprs[i]==<IntLiteral>, so we must
            // keep null entries in typeArgs rather than dropping them.
            cloned->typeArgs.reserve(src->typeArgs.size());
            for (const auto& arg : src->typeArgs) {
                cloned->typeArgs.push_back(arg ? cloneTypeAnnotation(*arg) : nullptr);
            }
            // Clone the parallel const-value argument expressions; required so
            // downstream Mono can evaluate non-type turbofish slots (e.g.
            // Array<i32, N>) after a generic body is cloned.
            cloned->argExprs.reserve(src->argExprs.size());
            for (const auto& ae : src->argExprs) {
                cloned->argExprs.push_back(ae ? cloneExpr(*ae) : nullptr);
            }
            return cloned;
        }
        case TypeAnnotationKind::Union: {
            auto* src = type.as<UnionType>();
            auto cloned = std::make_unique<UnionType>();
            cloned->location = type.location;
            cloned->name = type.name;
            for (const auto& member : src->members) {
                if (member) cloned->members.push_back(cloneTypeAnnotation(*member));
            }
            return cloned;
        }
        case TypeAnnotationKind::PackIndex: {
            auto* src = type.as<PackIndexType>();
            auto cloned = std::make_unique<PackIndexType>();
            cloned->location = type.location;
            cloned->name = type.name;
            cloned->packName = src->packName;
            if (src->indexExpr) cloned->indexExpr = cloneExpr(*src->indexExpr);
            return cloned;
        }
        case TypeAnnotationKind::Dependent: {
            // P2-generics C5: clone a dependent associated-type reference
            // (`T::Item`). Both fields are plain strings so no deep copy is
            // needed beyond filling the new node.
            auto* src = type.as<DependentType>();
            auto cloned = std::make_unique<DependentType>();
            cloned->location = type.location;
            cloned->name = type.name;
            cloned->baseName = src->baseName;
            cloned->memberName = src->memberName;
            return cloned;
        }
    }
    auto cloned = std::make_unique<NamedType>();
    cloned->location = type.location;
    cloned->name = type.name;
    return cloned;
}

ExprPtr cloneExpr(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::IntLiteral: {
            auto* src = expr.as<IntLiteralExpr>();
            auto cloned = std::make_unique<IntLiteralExpr>();
            cloned->location = expr.location;
            cloned->value = src->value;
            return cloned;
        }
        case ExprKind::FloatLiteral: {
            auto* src = expr.as<FloatLiteralExpr>();
            auto cloned = std::make_unique<FloatLiteralExpr>();
            cloned->location = expr.location;
            cloned->value = src->value;
            return cloned;
        }
        case ExprKind::StringLiteral: {
            auto* src = expr.as<StringLiteralExpr>();
            auto cloned = std::make_unique<StringLiteralExpr>();
            cloned->location = expr.location;
            cloned->value = src->value;
            return cloned;
        }
        case ExprKind::BoolLiteral: {
            auto* src = expr.as<BoolLiteralExpr>();
            auto cloned = std::make_unique<BoolLiteralExpr>();
            cloned->location = expr.location;
            cloned->value = src->value;
            return cloned;
        }
        case ExprKind::CharLiteral: {
            auto* src = expr.as<CharLiteralExpr>();
            auto cloned = std::make_unique<CharLiteralExpr>();
            cloned->location = expr.location;
            cloned->value = src->value;
            return cloned;
        }
        case ExprKind::NullLiteral: {
            auto cloned = std::make_unique<NullLiteralExpr>();
            cloned->location = expr.location;
            return cloned;
        }
        case ExprKind::Identifier: {
            auto* src = expr.as<IdentifierExpr>();
            auto cloned = std::make_unique<IdentifierExpr>();
            cloned->location = expr.location;
            cloned->name = src->name;
            for (const auto& ta : src->callTypeArgs) {
                cloned->callTypeArgs.push_back(ta ? cloneTypeAnnotation(*ta) : nullptr);
            }
            // Const-arg expressions (non-type turbofish slots like `Array::<i32, 16>`).
            for (const auto& ae : src->callArgExprs) {
                if (ae) cloned->callArgExprs.push_back(cloneExpr(*ae));
                else cloned->callArgExprs.push_back(nullptr);
            }
            if (src->typeAnnotation) cloned->typeAnnotation = cloneTypeAnnotation(*src->typeAnnotation);
            return cloned;
        }
        case ExprKind::BinaryOp: {
            auto* src = expr.as<BinaryOpExpr>();
            auto cloned = std::make_unique<BinaryOpExpr>();
            cloned->location = expr.location;
            cloned->op = src->op;
            if (src->lhs) cloned->lhs = cloneExpr(*src->lhs);
            if (src->rhs) cloned->rhs = cloneExpr(*src->rhs);
            return cloned;
        }
        case ExprKind::UnaryOp: {
            auto* src = expr.as<UnaryOpExpr>();
            auto cloned = std::make_unique<UnaryOpExpr>();
            cloned->location = expr.location;
            cloned->op = src->op;
            if (src->operand) cloned->operand = cloneExpr(*src->operand);
            return cloned;
        }
        case ExprKind::Call: {
            auto* src = expr.as<CallExpr>();
            auto cloned = std::make_unique<CallExpr>();
            cloned->location = expr.location;
            if (src->callee) cloned->callee = cloneExpr(*src->callee);
            for (const auto& arg : src->args) {
                if (arg) cloned->args.push_back(cloneExpr(*arg));
            }
            cloned->argNames = src->argNames;
            return cloned;
        }
        case ExprKind::MemberAccess: {
            auto* src = expr.as<MemberAccessExpr>();
            auto cloned = std::make_unique<MemberAccessExpr>();
            cloned->location = expr.location;
            if (src->object) cloned->object = cloneExpr(*src->object);
            cloned->member = src->member;
            for (const auto& ta : src->callTypeArgs) {
                if (ta) cloned->callTypeArgs.push_back(cloneTypeAnnotation(*ta));
            }
            return cloned;
        }
        case ExprKind::Index: {
            auto* src = expr.as<IndexExpr>();
            auto cloned = std::make_unique<IndexExpr>();
            cloned->location = expr.location;
            if (src->object) cloned->object = cloneExpr(*src->object);
            if (src->indexExpr) cloned->indexExpr = cloneExpr(*src->indexExpr);
            return cloned;
        }
        case ExprKind::Assignment: {
            auto* src = expr.as<AssignmentExpr>();
            auto cloned = std::make_unique<AssignmentExpr>();
            cloned->location = expr.location;
            if (src->lhs) cloned->lhs = cloneExpr(*src->lhs);
            if (src->rhs) cloned->rhs = cloneExpr(*src->rhs);
            return cloned;
        }
        case ExprKind::CompoundAssignment: {
            auto* src = expr.as<CompoundAssignmentExpr>();
            auto cloned = std::make_unique<CompoundAssignmentExpr>();
            cloned->location = expr.location;
            cloned->op = src->op;
            if (src->target) cloned->target = cloneExpr(*src->target);
            if (src->value) cloned->value = cloneExpr(*src->value);
            return cloned;
        }
        case ExprKind::Cast: {
            auto* src = expr.as<CastExpr>();
            auto cloned = std::make_unique<CastExpr>();
            cloned->location = expr.location;
            if (src->operand) cloned->operand = cloneExpr(*src->operand);
            if (src->targetType) cloned->targetType = cloneTypeAnnotation(*src->targetType);
            return cloned;
        }
        case ExprKind::StructInit: {
            auto* src = expr.as<StructInitExpr>();
            auto cloned = std::make_unique<StructInitExpr>();
            cloned->location = expr.location;
            cloned->structName = src->structName;
            for (const auto& [name, fieldExpr] : src->fieldInits) {
                if (fieldExpr) cloned->fieldInits.emplace_back(name, cloneExpr(*fieldExpr));
            }
            if (src->spreadBase) cloned->spreadBase = cloneExpr(*src->spreadBase);
            if (src->typeAnnotation) cloned->typeAnnotation = cloneTypeAnnotation(*src->typeAnnotation);
            return cloned;
        }
        case ExprKind::ArrayInit: {
            auto* src = expr.as<ArrayInitExpr>();
            auto cloned = std::make_unique<ArrayInitExpr>();
            cloned->location = expr.location;
            for (const auto& elem : src->elements) {
                if (elem) cloned->elements.push_back(cloneExpr(*elem));
            }
            cloned->elementIsSpread = src->elementIsSpread;
            if (src->repeatCount) cloned->repeatCount = cloneExpr(*src->repeatCount);
            return cloned;
        }
        case ExprKind::TupleInit: {
            auto* src = expr.as<TupleInitExpr>();
            auto cloned = std::make_unique<TupleInitExpr>();
            cloned->location = expr.location;
            for (const auto& elem : src->elements) {
                if (elem) cloned->elements.push_back(cloneExpr(*elem));
            }
            return cloned;
        }
        case ExprKind::StringInterpolation: {
            auto* src = expr.as<StringInterpExpr>();
            auto cloned = std::make_unique<StringInterpExpr>();
            cloned->location = expr.location;
            for (const auto& part : src->parts) {
                InterpPart clonedPart;
                clonedPart.isExpr = part.isExpr;
                clonedPart.text = part.text;
                if (part.expr) clonedPart.expr = cloneExpr(*part.expr);
                cloned->parts.push_back(std::move(clonedPart));
            }
            return cloned;
        }
        case ExprKind::SelfExpr: {
            auto cloned = std::make_unique<SelfExpr>();
            cloned->location = expr.location;
            return cloned;
        }
        case ExprKind::FailExpr: {
            auto* src = expr.as<FailExpr>();
            auto cloned = std::make_unique<FailExpr>();
            cloned->location = expr.location;
            cloned->typeName = src->typeName;
            cloned->variant = src->variant;
            cloned->isPayload = src->isPayload;
            if (src->message) cloned->message = cloneExpr(*src->message);
            return cloned;
        }
        case ExprKind::TryExpr: {
            auto* src = expr.as<TryExpr>();
            auto cloned = std::make_unique<TryExpr>();
            cloned->location = expr.location;
            if (src->inner) cloned->inner = cloneExpr(*src->inner);
            return cloned;
        }
        case ExprKind::Ternary: {
            auto* src = expr.as<TernaryExpr>();
            auto cloned = std::make_unique<TernaryExpr>();
            cloned->location = expr.location;
            if (src->condition) cloned->condition = cloneExpr(*src->condition);
            if (src->trueExpr) cloned->trueExpr = cloneExpr(*src->trueExpr);
            if (src->falseExpr) cloned->falseExpr = cloneExpr(*src->falseExpr);
            return cloned;
        }
        case ExprKind::Closure: {
            auto* src = expr.as<ClosureExpr>();
            auto cloned = std::make_unique<ClosureExpr>();
            cloned->location = expr.location;
            for (const auto& cap : src->captures) {
                CaptureItem clonedCap;
                clonedCap.name = cap.name;
                clonedCap.byRef = cap.byRef;
                clonedCap.move = cap.move;
                if (cap.moveExpr) clonedCap.moveExpr = cloneExpr(*cap.moveExpr);
                cloned->captures.push_back(std::move(clonedCap));
            }
            for (const auto& param : src->params) {
                ClosureParam clonedParam;
                clonedParam.name = param.name;
                if (param.type) clonedParam.type = cloneTypeAnnotation(*param.type);
                cloned->params.push_back(std::move(clonedParam));
            }
            if (src->returnType) cloned->returnType = cloneTypeAnnotation(*src->returnType);
            if (src->body) cloned->body = cloneStatement(*src->body);
            if (src->singleExpr) cloned->singleExpr = cloneExpr(*src->singleExpr);
            return cloned;
        }
        case ExprKind::AwaitExpr: {
            auto* src = expr.as<AwaitExpr>();
            auto cloned = std::make_unique<AwaitExpr>();
            cloned->location = expr.location;
            if (src->inner) cloned->inner = cloneExpr(*src->inner);
            return cloned;
        }
        case ExprKind::InlineAsm: {
            auto* src = expr.as<InlineAsmExpr>();
            auto cloned = std::make_unique<InlineAsmExpr>();
            cloned->location = expr.location;
            cloned->asmTemplate = src->asmTemplate;
            cloned->constraints = src->constraints;
            cloned->hasSideEffects = src->hasSideEffects;
            for (const auto& op : src->operands) {
                if (op) cloned->operands.push_back(cloneExpr(*op));
            }
            return cloned;
        }
        // P5-pack: pack fold expression — clone preserves packName + op verbatim.
        // Mono will expand the fold when it has concrete pack members.
        case ExprKind::PackFold: {
            auto* src = expr.as<PackFoldExpr>();
            auto cloned = std::make_unique<PackFoldExpr>();
            cloned->location = expr.location;
            cloned->packName = src->packName;
            cloned->op       = src->op;
            return cloned;
        }
        // P3-Q: type reflection expression — typeParam and member are plain strings.
        case ExprKind::TypeReflect: {
            auto* src = expr.as<TypeReflectExpr>();
            auto cloned = std::make_unique<TypeReflectExpr>();
            cloned->location  = expr.location;
            cloned->typeParam = src->typeParam;
            cloned->member    = src->member;
            return cloned;
        }
        // F: sizeof...(pack) — packName is a plain string; clone verbatim.
        case ExprKind::SizeofPack: {
            auto* src = expr.as<SizeofPackExpr>();
            auto cloned = std::make_unique<SizeofPackExpr>();
            cloned->location = expr.location;
            cloned->packName = src->packName;
            return cloned;
        }
    }
    auto cloned = std::make_unique<NullLiteralExpr>();
    cloned->location = expr.location;
    return cloned;
}

StmtPtr cloneStatement(const Stmt& stmt) {
    switch (stmt.kind) {
        case StmtKind::VarDecl: {
            auto* src = stmt.as<VarDeclStmt>();
            auto cloned = std::make_unique<VarDeclStmt>();
            cloned->location = stmt.location;
            cloned->isConst = src->isConst;
            cloned->isComptime = src->isComptime;
            cloned->varName = src->varName;
            if (src->varType) cloned->varType = cloneTypeAnnotation(*src->varType);
            if (src->initExpr) cloned->initExpr = cloneExpr(*src->initExpr);
            if (src->elseBranch) cloned->elseBranch = cloneStatement(*src->elseBranch);
            return cloned;
        }
        case StmtKind::ExprStmt: {
            auto* src = stmt.as<ExprStmt>();
            auto cloned = std::make_unique<ExprStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = cloneExpr(*src->expr);
            
            return cloned;
        }
        case StmtKind::Return: {
            auto* src = stmt.as<ReturnStmt>();
            auto cloned = std::make_unique<ReturnStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = cloneExpr(*src->expr);
            return cloned;
        }
        case StmtKind::If: {
            auto* src = stmt.as<IfStmt>();
            auto cloned = std::make_unique<IfStmt>();
            cloned->location = stmt.location;
            if (src->condition) cloned->condition = cloneExpr(*src->condition);
            if (src->thenBranch) cloned->thenBranch = cloneStatement(*src->thenBranch);
            for (const auto& [cond, branch] : src->elifBranches) {
                if (cond && branch) {
                    cloned->elifBranches.emplace_back(cloneExpr(*cond), cloneStatement(*branch));
                }
            }
            if (src->elseBranch) cloned->elseBranch = cloneStatement(*src->elseBranch);
            return cloned;
        }
        case StmtKind::While: {
            auto* src = stmt.as<WhileStmt>();
            auto cloned = std::make_unique<WhileStmt>();
            cloned->location = stmt.location;
            if (src->condition) cloned->condition = cloneExpr(*src->condition);
            if (src->body) cloned->body = cloneStatement(*src->body);
            return cloned;
        }
        case StmtKind::For: {
            auto* src = stmt.as<ForStmt>();
            auto cloned = std::make_unique<ForStmt>();
            cloned->location = stmt.location;
            if (src->init) cloned->init = cloneStatement(*src->init);
            if (src->condition) cloned->condition = cloneExpr(*src->condition);
            if (src->step) cloned->step = cloneExpr(*src->step);
            if (src->body) cloned->body = cloneStatement(*src->body);
            return cloned;
        }
        case StmtKind::ForEach: {
            auto* src = stmt.as<ForEachStmt>();
            auto cloned = std::make_unique<ForEachStmt>();
            cloned->location = stmt.location;
            cloned->varName = src->varName;
            cloned->destructure = src->destructure;
            if (src->collection) cloned->collection = cloneExpr(*src->collection);
            if (src->body) cloned->body = cloneStatement(*src->body);
            return cloned;
        }
        case StmtKind::Block: {
            auto* src = stmt.as<BlockStmt>();
            auto cloned = std::make_unique<BlockStmt>();
            cloned->location = stmt.location;
            for (const auto& s : src->statements) {
                if (s) cloned->statements.push_back(cloneStatement(*s));
            }
            return cloned;
        }
        case StmtKind::Break: {
            auto cloned = std::make_unique<BreakStmt>();
            cloned->location = stmt.location;
            return cloned;
        }
        case StmtKind::Continue: {
            auto cloned = std::make_unique<ContinueStmt>();
            cloned->location = stmt.location;
            return cloned;
        }
        case StmtKind::Match: {
            auto* src = stmt.as<MatchStmt>();
            auto cloned = std::make_unique<MatchStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = cloneExpr(*src->expr);
            for (const auto& arm : src->arms) {
                MatchArm clonedArm;
                clonedArm.location = arm.location;
                clonedArm.label = arm.label;
                clonedArm.bindingName = arm.bindingName;
                clonedArm.isDefault = arm.isDefault;
                clonedArm.tupleBindings = arm.tupleBindings;
                clonedArm.nestedPatterns = arm.nestedPatterns;
                if (arm.typePattern) clonedArm.typePattern = cloneTypeAnnotation(*arm.typePattern);
                if (arm.valuePattern) clonedArm.valuePattern = cloneExpr(*arm.valuePattern);
                if (arm.guardExpr) clonedArm.guardExpr = cloneExpr(*arm.guardExpr);
                if (arm.body) clonedArm.body = cloneStatement(*arm.body);
                cloned->arms.push_back(std::move(clonedArm));
            }
            return cloned;
        }
        case StmtKind::Assignment: {
            auto* src = stmt.as<AssignStmt>();
            auto cloned = std::make_unique<AssignStmt>();
            cloned->location = stmt.location;
            if (src->target) cloned->target = cloneExpr(*src->target);
            if (src->value) cloned->value = cloneExpr(*src->value);
            return cloned;
        }
        case StmtKind::Defer: {
            auto* src = stmt.as<DeferStmt>();
            auto cloned = std::make_unique<DeferStmt>();
            cloned->location = stmt.location;
            if (src->body) cloned->body = cloneStatement(*src->body);
            return cloned;
        }
        case StmtKind::StaticAssert: {
            auto* src = stmt.as<StaticAssertStmt>();
            auto cloned = std::make_unique<StaticAssertStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = cloneExpr(*src->expr);
            cloned->message = src->message;
            return cloned;
        }
        case StmtKind::Unsafe: {
            auto* src = stmt.as<UnsafeStmt>();
            auto cloned = std::make_unique<UnsafeStmt>();
            cloned->location = stmt.location;
            if (src->body) cloned->body = cloneStatement(*src->body);
            return cloned;
        }
    }
    auto cloned = std::make_unique<BlockStmt>();
    cloned->location = stmt.location;
    return cloned;
}

// ============================================================
//  Type-substituting clone variants
// ============================================================
//
// Each function mirrors the structural shape of its pure-clone counterpart
// above.  The only difference is that every TypeAnnotation leaf is routed
// through:
//   reify( tr.substituteType(*ann, subst) )
// instead of cloneTypeAnnotation(*ann).
//
// `reify` is typically Sema::convertTypeToAnnotation, threaded in as a
// std::function so this file does not need to include Sema.h.

TypePtr cloneTypeAnnotationWithSubst(const TypeAnnotation& type,
                                     TemplateResolver& tr,
                                     const GenericSubstitution& subst,
                                     const TypeReifier& reify)
{
    // Substitute through the resolver and reify back to an AST annotation.
    // substituteType handles Named, Generic, Pointer, Reference, Array,
    // Tuple, Function, Union — every kind that can carry a generic param.
    VyxTypePtr substituted = tr.substituteType(type, subst);
    TypePtr result = reify(substituted);
    if (result) {
        result->location = type.location;
        return result;
    }
    // Fallback: pure structural clone so the tree stays well-formed even if
    // the reifier returns nullptr (e.g. for an unknown nominal type that
    // the substitution left unchanged).
    return cloneTypeAnnotation(type);
}

ExprPtr cloneExprWithSubst(const Expr& expr,
                            TemplateResolver& tr,
                            const GenericSubstitution& subst,
                            const TypeReifier& reify)
{
    // Convenience shorthands that capture the context.
    auto subType = [&](const TypeAnnotation& t) -> TypePtr {
        return cloneTypeAnnotationWithSubst(t, tr, subst, reify);
    };
    auto subExpr = [&](const Expr& e) -> ExprPtr {
        return cloneExprWithSubst(e, tr, subst, reify);
    };
    auto subStmt = [&](const Stmt& s) -> StmtPtr {
        return cloneStatementWithSubst(s, tr, subst, reify);
    };

    switch (expr.kind) {
        case ExprKind::IntLiteral: {
            auto* src = expr.as<IntLiteralExpr>();
            auto cloned = std::make_unique<IntLiteralExpr>();
            cloned->location = expr.location;
            cloned->value = src->value;
            return cloned;
        }
        case ExprKind::FloatLiteral: {
            auto* src = expr.as<FloatLiteralExpr>();
            auto cloned = std::make_unique<FloatLiteralExpr>();
            cloned->location = expr.location;
            cloned->value = src->value;
            return cloned;
        }
        case ExprKind::StringLiteral: {
            auto* src = expr.as<StringLiteralExpr>();
            auto cloned = std::make_unique<StringLiteralExpr>();
            cloned->location = expr.location;
            cloned->value = src->value;
            return cloned;
        }
        case ExprKind::BoolLiteral: {
            auto* src = expr.as<BoolLiteralExpr>();
            auto cloned = std::make_unique<BoolLiteralExpr>();
            cloned->location = expr.location;
            cloned->value = src->value;
            return cloned;
        }
        case ExprKind::CharLiteral: {
            auto* src = expr.as<CharLiteralExpr>();
            auto cloned = std::make_unique<CharLiteralExpr>();
            cloned->location = expr.location;
            cloned->value = src->value;
            return cloned;
        }
        case ExprKind::NullLiteral: {
            auto cloned = std::make_unique<NullLiteralExpr>();
            cloned->location = expr.location;
            return cloned;
        }
        case ExprKind::Identifier: {
            auto* src = expr.as<IdentifierExpr>();
            auto cloned = std::make_unique<IdentifierExpr>();
            cloned->location = expr.location;
            cloned->name = src->name;
            // Turbofish call type args: e.g. foo::<T>() → foo::<i32>()
            for (const auto& ta : src->callTypeArgs) {
                if (ta) cloned->callTypeArgs.push_back(subType(*ta));
                else cloned->callTypeArgs.push_back(nullptr);
            }
            // Const-arg expressions (non-type turbofish slots like `inner::<N>()`).
            // Without this, callArgExprs on callees like `inner::<N>` are lost when
            // cloning outer's body, and substConstIdentsInStmt cannot substitute N.
            for (const auto& ae : src->callArgExprs) {
                if (ae) cloned->callArgExprs.push_back(subExpr(*ae));
                else cloned->callArgExprs.push_back(nullptr);
            }
            if (src->typeAnnotation) cloned->typeAnnotation = subType(*src->typeAnnotation);
            return cloned;
        }
        case ExprKind::BinaryOp: {
            auto* src = expr.as<BinaryOpExpr>();
            auto cloned = std::make_unique<BinaryOpExpr>();
            cloned->location = expr.location;
            cloned->op = src->op;
            if (src->lhs) cloned->lhs = subExpr(*src->lhs);
            if (src->rhs) cloned->rhs = subExpr(*src->rhs);
            return cloned;
        }
        case ExprKind::UnaryOp: {
            auto* src = expr.as<UnaryOpExpr>();
            auto cloned = std::make_unique<UnaryOpExpr>();
            cloned->location = expr.location;
            cloned->op = src->op;
            if (src->operand) cloned->operand = subExpr(*src->operand);
            return cloned;
        }
        case ExprKind::Call: {
            auto* src = expr.as<CallExpr>();
            auto cloned = std::make_unique<CallExpr>();
            cloned->location = expr.location;
            if (src->callee) cloned->callee = subExpr(*src->callee);
            for (const auto& arg : src->args) {
                if (arg) cloned->args.push_back(subExpr(*arg));
            }
            cloned->argNames = src->argNames;
            return cloned;
        }
        case ExprKind::MemberAccess: {
            auto* src = expr.as<MemberAccessExpr>();
            auto cloned = std::make_unique<MemberAccessExpr>();
            cloned->location = expr.location;
            if (src->object) cloned->object = subExpr(*src->object);
            cloned->member = src->member;
            // Turbofish on member: obj.method::<T>() → obj.method::<i32>()
            for (const auto& ta : src->callTypeArgs) {
                if (ta) cloned->callTypeArgs.push_back(subType(*ta));
            }
            return cloned;
        }
        case ExprKind::Index: {
            auto* src = expr.as<IndexExpr>();
            auto cloned = std::make_unique<IndexExpr>();
            cloned->location = expr.location;
            if (src->object) cloned->object = subExpr(*src->object);
            if (src->indexExpr) cloned->indexExpr = subExpr(*src->indexExpr);
            return cloned;
        }
        case ExprKind::Assignment: {
            auto* src = expr.as<AssignmentExpr>();
            auto cloned = std::make_unique<AssignmentExpr>();
            cloned->location = expr.location;
            if (src->lhs) cloned->lhs = subExpr(*src->lhs);
            if (src->rhs) cloned->rhs = subExpr(*src->rhs);
            return cloned;
        }
        case ExprKind::CompoundAssignment: {
            auto* src = expr.as<CompoundAssignmentExpr>();
            auto cloned = std::make_unique<CompoundAssignmentExpr>();
            cloned->location = expr.location;
            cloned->op = src->op;
            if (src->target) cloned->target = subExpr(*src->target);
            if (src->value)  cloned->value  = subExpr(*src->value);
            return cloned;
        }
        case ExprKind::Cast: {
            // Cast target type: (val as T) → (val as i32)
            auto* src = expr.as<CastExpr>();
            auto cloned = std::make_unique<CastExpr>();
            cloned->location = expr.location;
            if (src->operand) cloned->operand = subExpr(*src->operand);
            if (src->targetType) cloned->targetType = subType(*src->targetType);
            return cloned;
        }
        case ExprKind::StructInit: {
            auto* src = expr.as<StructInitExpr>();
            auto cloned = std::make_unique<StructInitExpr>();
            cloned->location = expr.location;
            cloned->structName = src->structName;
            // The parser bakes the turbofish args into `structName` as a
            // mangled string like "Array<T,N>". When substituting T → i32,
            // rewrite matching tokens inside that string so the downstream
            // symbol lookup resolves to the concrete instance's name.
            // (Const-param tokens are filled in by a separate pass run
            // after this clone via substConstIdentsInStmt.)
            if (!cloned->structName.empty() &&
                cloned->structName.find('<') != std::string::npos) {
                std::string& s = cloned->structName;
                std::string out;
                out.reserve(s.size());
                size_t i = 0;
                bool changed = false;
                while (i < s.size()) {
                    char c = s[i];
                    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
                        size_t j = i;
                        while (j < s.size() &&
                               ((s[j] >= 'A' && s[j] <= 'Z') ||
                                (s[j] >= 'a' && s[j] <= 'z') ||
                                (s[j] >= '0' && s[j] <= '9') ||
                                s[j] == '_')) ++j;
                        std::string token(s, i, j - i);
                        if (subst.isGenericParam(token)) {
                            auto ct = subst.getConcreteType(token);
                            if (ct) {
                                out += ct->toString();
                                changed = true;
                            } else {
                                out += token;
                            }
                        } else {
                            out += token;
                        }
                        i = j;
                    } else {
                        out.push_back(c);
                        ++i;
                    }
                }
                if (changed) cloned->structName = std::move(out);
            }
            for (const auto& [name, fieldExpr] : src->fieldInits) {
                if (fieldExpr) cloned->fieldInits.emplace_back(name, subExpr(*fieldExpr));
            }
            if (src->spreadBase) cloned->spreadBase = subExpr(*src->spreadBase);
            // Generic struct init type annotation: Foo::<T> { ... } → Foo::<i32> { ... }
            if (src->typeAnnotation) cloned->typeAnnotation = subType(*src->typeAnnotation);
            return cloned;
        }
        case ExprKind::ArrayInit: {
            auto* src = expr.as<ArrayInitExpr>();
            auto cloned = std::make_unique<ArrayInitExpr>();
            cloned->location = expr.location;
            for (const auto& elem : src->elements) {
                if (elem) cloned->elements.push_back(subExpr(*elem));
            }
            cloned->elementIsSpread = src->elementIsSpread;
            // Repeat form `[v; N]`: carry the repeat expression over so
            // downstream const-param substitution can fold N to its bound
            // integer and analysis produces the correct [T; N] size.
            if (src->repeatCount) cloned->repeatCount = subExpr(*src->repeatCount);
            return cloned;
        }
        case ExprKind::TupleInit: {
            auto* src = expr.as<TupleInitExpr>();
            auto cloned = std::make_unique<TupleInitExpr>();
            cloned->location = expr.location;
            for (const auto& elem : src->elements) {
                if (elem) cloned->elements.push_back(subExpr(*elem));
            }
            return cloned;
        }
        case ExprKind::StringInterpolation: {
            auto* src = expr.as<StringInterpExpr>();
            auto cloned = std::make_unique<StringInterpExpr>();
            cloned->location = expr.location;
            for (const auto& part : src->parts) {
                InterpPart clonedPart;
                clonedPart.isExpr = part.isExpr;
                clonedPart.text = part.text;
                if (part.expr) clonedPart.expr = subExpr(*part.expr);
                cloned->parts.push_back(std::move(clonedPart));
            }
            return cloned;
        }
        case ExprKind::SelfExpr: {
            auto cloned = std::make_unique<SelfExpr>();
            cloned->location = expr.location;
            return cloned;
        }
        case ExprKind::FailExpr: {
            auto* src = expr.as<FailExpr>();
            auto cloned = std::make_unique<FailExpr>();
            cloned->location = expr.location;
            cloned->typeName = src->typeName;
            cloned->variant = src->variant;
            cloned->isPayload = src->isPayload;
            if (src->message) cloned->message = subExpr(*src->message);
            return cloned;
        }
        case ExprKind::TryExpr: {
            auto* src = expr.as<TryExpr>();
            auto cloned = std::make_unique<TryExpr>();
            cloned->location = expr.location;
            if (src->inner) cloned->inner = subExpr(*src->inner);
            return cloned;
        }
        case ExprKind::Ternary: {
            auto* src = expr.as<TernaryExpr>();
            auto cloned = std::make_unique<TernaryExpr>();
            cloned->location = expr.location;
            if (src->condition) cloned->condition = subExpr(*src->condition);
            if (src->trueExpr)  cloned->trueExpr  = subExpr(*src->trueExpr);
            if (src->falseExpr) cloned->falseExpr = subExpr(*src->falseExpr);
            return cloned;
        }
        case ExprKind::Closure: {
            // Closure: substitute param types, return type annotation, and recurse
            // into the body so inner var-decl annotations also get substituted.
            // Capture items: byRef/move captures are value-level; the moveExpr
            // sub-expression may reference generic types in casts.
            auto* src = expr.as<ClosureExpr>();
            auto cloned = std::make_unique<ClosureExpr>();
            cloned->location = expr.location;
            for (const auto& cap : src->captures) {
                CaptureItem clonedCap;
                clonedCap.name = cap.name;
                clonedCap.byRef = cap.byRef;
                clonedCap.move = cap.move;
                if (cap.moveExpr) clonedCap.moveExpr = subExpr(*cap.moveExpr);
                cloned->captures.push_back(std::move(clonedCap));
            }
            for (const auto& param : src->params) {
                ClosureParam clonedParam;
                clonedParam.name = param.name;
                if (param.type) clonedParam.type = subType(*param.type);
                cloned->params.push_back(std::move(clonedParam));
            }
            if (src->returnType) cloned->returnType = subType(*src->returnType);
            if (src->body)       cloned->body       = subStmt(*src->body);
            if (src->singleExpr) cloned->singleExpr = subExpr(*src->singleExpr);
            return cloned;
        }
        case ExprKind::AwaitExpr: {
            auto* src = expr.as<AwaitExpr>();
            auto cloned = std::make_unique<AwaitExpr>();
            cloned->location = expr.location;
            if (src->inner) cloned->inner = subExpr(*src->inner);
            return cloned;
        }
        case ExprKind::InlineAsm: {
            auto* src = expr.as<InlineAsmExpr>();
            auto cloned = std::make_unique<InlineAsmExpr>();
            cloned->location = expr.location;
            cloned->asmTemplate = src->asmTemplate;
            cloned->constraints = src->constraints;
            cloned->hasSideEffects = src->hasSideEffects;
            for (const auto& op : src->operands) {
                if (op) cloned->operands.push_back(subExpr(*op));
            }
            return cloned;
        }
        // P5-pack: pack fold — packName and op are not type-bearing; plain clone.
        case ExprKind::PackFold: {
            auto* src = expr.as<PackFoldExpr>();
            auto cloned = std::make_unique<PackFoldExpr>();
            cloned->location = expr.location;
            cloned->packName = src->packName;
            cloned->op       = src->op;
            return cloned;
        }
        // P3-Q: type reflection — typeParam / member are plain strings, no type-bearing fields.
        case ExprKind::TypeReflect: {
            auto* src = expr.as<TypeReflectExpr>();
            auto cloned = std::make_unique<TypeReflectExpr>();
            cloned->location  = expr.location;
            cloned->typeParam = src->typeParam;
            cloned->member    = src->member;
            return cloned;
        }
        // F: sizeof...(pack) — no type-bearing fields; plain clone.
        case ExprKind::SizeofPack: {
            auto* src = expr.as<SizeofPackExpr>();
            auto cloned = std::make_unique<SizeofPackExpr>();
            cloned->location = expr.location;
            cloned->packName = src->packName;
            return cloned;
        }
    }
    // Unhandled leaf: structural clone.
    return cloneExpr(expr);
}

StmtPtr cloneStatementWithSubst(const Stmt& stmt,
                                 TemplateResolver& tr,
                                 const GenericSubstitution& subst,
                                 const TypeReifier& reify)
{
    auto subType = [&](const TypeAnnotation& t) -> TypePtr {
        return cloneTypeAnnotationWithSubst(t, tr, subst, reify);
    };
    auto subExpr = [&](const Expr& e) -> ExprPtr {
        return cloneExprWithSubst(e, tr, subst, reify);
    };
    auto subStmt = [&](const Stmt& s) -> StmtPtr {
        return cloneStatementWithSubst(s, tr, subst, reify);
    };

    switch (stmt.kind) {
        case StmtKind::VarDecl: {
            auto* src = stmt.as<VarDeclStmt>();
            auto cloned = std::make_unique<VarDeclStmt>();
            cloned->location   = stmt.location;
            cloned->isConst    = src->isConst;
            cloned->isComptime = src->isComptime;
            cloned->varName    = src->varName;
            // var x: T = ... → var x: i32 = ...
            if (src->varType)    cloned->varType    = subType(*src->varType);
            if (src->initExpr)   cloned->initExpr   = subExpr(*src->initExpr);
            if (src->elseBranch) cloned->elseBranch = subStmt(*src->elseBranch);
            return cloned;
        }
        case StmtKind::ExprStmt: {
            auto* src = stmt.as<ExprStmt>();
            auto cloned = std::make_unique<ExprStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = subExpr(*src->expr);
            return cloned;
        }
        case StmtKind::Return: {
            auto* src = stmt.as<ReturnStmt>();
            auto cloned = std::make_unique<ReturnStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = subExpr(*src->expr);
            return cloned;
        }
        case StmtKind::If: {
            auto* src = stmt.as<IfStmt>();
            auto cloned = std::make_unique<IfStmt>();
            cloned->location = stmt.location;
            if (src->condition)  cloned->condition  = subExpr(*src->condition);
            if (src->thenBranch) cloned->thenBranch = subStmt(*src->thenBranch);
            for (const auto& [cond, branch] : src->elifBranches) {
                if (cond && branch)
                    cloned->elifBranches.emplace_back(subExpr(*cond), subStmt(*branch));
            }
            if (src->elseBranch) cloned->elseBranch = subStmt(*src->elseBranch);
            return cloned;
        }
        case StmtKind::While: {
            auto* src = stmt.as<WhileStmt>();
            auto cloned = std::make_unique<WhileStmt>();
            cloned->location = stmt.location;
            if (src->condition) cloned->condition = subExpr(*src->condition);
            if (src->body)      cloned->body      = subStmt(*src->body);
            return cloned;
        }
        case StmtKind::For: {
            auto* src = stmt.as<ForStmt>();
            auto cloned = std::make_unique<ForStmt>();
            cloned->location = stmt.location;
            if (src->init)      cloned->init      = subStmt(*src->init);
            if (src->condition) cloned->condition = subExpr(*src->condition);
            if (src->step)      cloned->step      = subExpr(*src->step);
            if (src->body)      cloned->body      = subStmt(*src->body);
            return cloned;
        }
        case StmtKind::ForEach: {
            auto* src = stmt.as<ForEachStmt>();
            auto cloned = std::make_unique<ForEachStmt>();
            cloned->location    = stmt.location;
            cloned->varName     = src->varName;
            cloned->destructure = src->destructure;
            if (src->collection) cloned->collection = subExpr(*src->collection);
            if (src->body)       cloned->body       = subStmt(*src->body);
            return cloned;
        }
        case StmtKind::Block: {
            auto* src = stmt.as<BlockStmt>();
            auto cloned = std::make_unique<BlockStmt>();
            cloned->location = stmt.location;
            for (const auto& s : src->statements) {
                if (s) cloned->statements.push_back(subStmt(*s));
            }
            return cloned;
        }
        case StmtKind::Break: {
            auto cloned = std::make_unique<BreakStmt>();
            cloned->location = stmt.location;
            return cloned;
        }
        case StmtKind::Continue: {
            auto cloned = std::make_unique<ContinueStmt>();
            cloned->location = stmt.location;
            return cloned;
        }
        case StmtKind::Match: {
            auto* src = stmt.as<MatchStmt>();
            auto cloned = std::make_unique<MatchStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = subExpr(*src->expr);
            for (const auto& arm : src->arms) {
                MatchArm clonedArm;
                clonedArm.location       = arm.location;
                clonedArm.label          = arm.label;
                clonedArm.bindingName    = arm.bindingName;
                clonedArm.isDefault      = arm.isDefault;
                clonedArm.tupleBindings  = arm.tupleBindings;
                clonedArm.nestedPatterns = arm.nestedPatterns;
                // Match arm type pattern: `is T` → `is i32`
                if (arm.typePattern)  clonedArm.typePattern  = subType(*arm.typePattern);
                if (arm.valuePattern) clonedArm.valuePattern = subExpr(*arm.valuePattern);
                if (arm.guardExpr)    clonedArm.guardExpr    = subExpr(*arm.guardExpr);
                if (arm.body)         clonedArm.body         = subStmt(*arm.body);
                cloned->arms.push_back(std::move(clonedArm));
            }
            return cloned;
        }
        case StmtKind::Assignment: {
            auto* src = stmt.as<AssignStmt>();
            auto cloned = std::make_unique<AssignStmt>();
            cloned->location = stmt.location;
            if (src->target) cloned->target = subExpr(*src->target);
            if (src->value)  cloned->value  = subExpr(*src->value);
            return cloned;
        }
        case StmtKind::Defer: {
            auto* src = stmt.as<DeferStmt>();
            auto cloned = std::make_unique<DeferStmt>();
            cloned->location = stmt.location;
            if (src->body) cloned->body = subStmt(*src->body);
            return cloned;
        }
        case StmtKind::StaticAssert: {
            auto* src = stmt.as<StaticAssertStmt>();
            auto cloned = std::make_unique<StaticAssertStmt>();
            cloned->location = stmt.location;
            if (src->expr) cloned->expr = subExpr(*src->expr);
            cloned->message = src->message;
            return cloned;
        }
        case StmtKind::Unsafe: {
            auto* src = stmt.as<UnsafeStmt>();
            auto cloned = std::make_unique<UnsafeStmt>();
            cloned->location = stmt.location;
            if (src->body) cloned->body = subStmt(*src->body);
            return cloned;
        }
    }
    // Unhandled: structural clone.
    return cloneStatement(stmt);
}

} // namespace vyx
