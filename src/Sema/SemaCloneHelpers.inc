// Shared clone helpers for Sema translation units

static ExprPtr cloneExpr(const Expr& expr);
static StmtPtr cloneStatement(const Stmt& stmt);
static TypePtr cloneTypeAnnotation(const TypeAnnotation& type);

/// Deep clone a type annotation
static TypePtr cloneTypeAnnotation(const TypeAnnotation& type) {
    auto cloned = std::make_unique<TypeAnnotation>();
    cloned->kind = type.kind;
    cloned->location = type.location;
    cloned->name = type.name;
    cloned->isMutable = type.isMutable;
    
    if (type.innerType) cloned->innerType = cloneTypeAnnotation(*type.innerType);
    if (type.arraySize) cloned->arraySize = cloneExpr(*type.arraySize);
    if (type.returnType) cloned->returnType = cloneTypeAnnotation(*type.returnType);
    
    for (const auto& subType : type.subTypes) {
        if (subType) cloned->subTypes.push_back(cloneTypeAnnotation(*subType));
    }
    
    return cloned;
}

/// Deep clone an expression tree
static ExprPtr cloneExpr(const Expr& expr) {
    auto cloned = std::make_unique<Expr>();
    cloned->kind = expr.kind;
    cloned->location = expr.location;
    
    // Copy primitive fields
    cloned->intValue = expr.intValue;
    cloned->floatValue = expr.floatValue;
    cloned->stringValue = expr.stringValue;
    cloned->boolValue = expr.boolValue;
    cloned->binaryOp = expr.binaryOp;
    cloned->unaryOp = expr.unaryOp;
    cloned->compoundOp = expr.compoundOp;
    cloned->structName = expr.structName;
    cloned->member = expr.member;
    cloned->failTypeName = expr.failTypeName;
    cloned->failVariant = expr.failVariant;
    
    // Deep copy children
    if (expr.lhs) cloned->lhs = cloneExpr(*expr.lhs);
    if (expr.rhs) cloned->rhs = cloneExpr(*expr.rhs);
    if (expr.operand) cloned->operand = cloneExpr(*expr.operand);
    if (expr.callee) cloned->callee = cloneExpr(*expr.callee);
    if (expr.object) cloned->object = cloneExpr(*expr.object);
    if (expr.indexExpr) cloned->indexExpr = cloneExpr(*expr.indexExpr);
    if (expr.compoundTarget) cloned->compoundTarget = cloneExpr(*expr.compoundTarget);
    if (expr.compoundValue) cloned->compoundValue = cloneExpr(*expr.compoundValue);
    if (expr.tryExpr) cloned->tryExpr = cloneExpr(*expr.tryExpr);
    
    // Copy argument lists
    for (const auto& arg : expr.args) {
        if (arg) cloned->args.push_back(cloneExpr(*arg));
    }
    
    // Copy field initializers
    for (const auto& [name, fieldExpr] : expr.fieldInits) {
        if (fieldExpr) {
            cloned->fieldInits.emplace_back(name, cloneExpr(*fieldExpr));
        }
    }
    
    // Copy elements
    for (const auto& elem : expr.elements) {
        if (elem) cloned->elements.push_back(cloneExpr(*elem));
    }
    
    // Copy interpolation parts
    for (const auto& part : expr.interpParts) {
        Expr::InterpPart clonedPart;
        clonedPart.isExpr = part.isExpr;
        clonedPart.text = part.text;
        if (part.expr) clonedPart.expr = cloneExpr(*part.expr);
        cloned->interpParts.push_back(std::move(clonedPart));
    }
    
    // Copy captures
    for (const auto& cap : expr.captures) {
        Expr::CaptureItem clonedCap;
        clonedCap.name = cap.name;
        clonedCap.byRef = cap.byRef;
        clonedCap.move = cap.move;
        if (cap.moveExpr) clonedCap.moveExpr = cloneExpr(*cap.moveExpr);
        cloned->captures.push_back(std::move(clonedCap));
    }
    
    // Copy closure params
    for (const auto& param : expr.closureParams) {
        Expr::ClosureParam clonedParam;
        clonedParam.name = param.name;
        if (param.type) {
            clonedParam.type = cloneTypeAnnotation(*param.type);
        }
        cloned->closureParams.push_back(std::move(clonedParam));
    }
    
    // Copy closure body/expr
    if (expr.closureBody) cloned->closureBody = cloneStatement(*expr.closureBody);
    if (expr.closureExpr) cloned->closureExpr = cloneExpr(*expr.closureExpr);
    
    // Copy type annotation
    if (expr.typeAnnotation) {
        cloned->typeAnnotation = cloneTypeAnnotation(*expr.typeAnnotation);
    }
    
    return cloned;
}

/// Deep clone a statement tree
static StmtPtr cloneStatement(const Stmt& stmt) {
    auto cloned = std::make_unique<Stmt>();
    cloned->kind = stmt.kind;
    cloned->location = stmt.location;
    
    // Copy primitive fields
    cloned->isConst = stmt.isConst;
    cloned->varName = stmt.varName;
    cloned->foreachVar = stmt.foreachVar;
    cloned->foreachDestructure = stmt.foreachDestructure;

    // Deep copy children
    if (stmt.initExpr) cloned->initExpr = cloneExpr(*stmt.initExpr);
    if (stmt.expr) cloned->expr = cloneExpr(*stmt.expr);
    if (stmt.condition) cloned->condition = cloneExpr(*stmt.condition);
    if (stmt.thenBranch) cloned->thenBranch = cloneStatement(*stmt.thenBranch);
    if (stmt.whileCond) cloned->whileCond = cloneExpr(*stmt.whileCond);
    if (stmt.whileBody) cloned->whileBody = cloneStatement(*stmt.whileBody);
    if (stmt.forInit) cloned->forInit = cloneStatement(*stmt.forInit);
    if (stmt.forCond) cloned->forCond = cloneExpr(*stmt.forCond);
    if (stmt.forStep) cloned->forStep = cloneExpr(*stmt.forStep);
    if (stmt.forBody) cloned->forBody = cloneStatement(*stmt.forBody);
    if (stmt.foreachCollection) cloned->foreachCollection = cloneExpr(*stmt.foreachCollection);
    if (stmt.foreachBody) cloned->foreachBody = cloneStatement(*stmt.foreachBody);
    if (stmt.matchExpr) cloned->matchExpr = cloneExpr(*stmt.matchExpr);
    if (stmt.assignTarget) cloned->assignTarget = cloneExpr(*stmt.assignTarget);
    if (stmt.assignValue) cloned->assignValue = cloneExpr(*stmt.assignValue);
    
    // Copy elif branches
    for (const auto& [cond, branch] : stmt.elifBranches) {
        if (cond && branch) {
            cloned->elifBranches.emplace_back(cloneExpr(*cond), cloneStatement(*branch));
        }
    }
    
    // Copy else branch
    if (stmt.elseBranch) cloned->elseBranch = cloneStatement(*stmt.elseBranch);
    
    // Copy statements in block
    for (const auto& s : stmt.statements) {
        if (s) cloned->statements.push_back(cloneStatement(*s));
    }
    
    // Copy match arms
    for (const auto& arm : stmt.matchArms) {
        MatchArm clonedArm;
        clonedArm.location = arm.location;
        clonedArm.label = arm.label;
        clonedArm.bindingName = arm.bindingName;
        clonedArm.isDefault = arm.isDefault;
        if (arm.typePattern) {
            clonedArm.typePattern = cloneTypeAnnotation(*arm.typePattern);
        }
        if (arm.valuePattern) {
            clonedArm.valuePattern = cloneExpr(*arm.valuePattern);
        }
        clonedArm.tupleBindings = arm.tupleBindings;
        if (arm.guardExpr) {
            clonedArm.guardExpr = cloneExpr(*arm.guardExpr);
        }
        if (arm.body) {
            clonedArm.body = cloneStatement(*arm.body);
        }
        cloned->matchArms.push_back(std::move(clonedArm));
    }
    
    // Copy var type
    if (stmt.varType) {
        cloned->varType = cloneTypeAnnotation(*stmt.varType);
    }
    
    return cloned;
}

static int levenshteinDistance(const std::string& a, const std::string& b) {
    int m = static_cast<int>(a.size()), n = static_cast<int>(b.size());
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1));
    for (int i = 0; i <= m; ++i) dp[i][0] = i;
    for (int j = 0; j <= n; ++j) dp[0][j] = j;
    for (int i = 1; i <= m; ++i)
        for (int j = 1; j <= n; ++j)
            dp[i][j] = std::min({dp[i-1][j]+1, dp[i][j-1]+1, dp[i-1][j-1]+(a[i-1]!=b[j-1])});
    return dp[m][n];
}