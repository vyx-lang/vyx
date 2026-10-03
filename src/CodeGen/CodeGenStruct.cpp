#include "CodeGenIncludes.h"

namespace vyx {

void CodeGen::emitStructDecl(const Decl& decl) {
    if (structTypes_.count(decl.name)) return;

    // BUG-LV-04 root cause — primitive-target impl blocks reuse the
    // primitive's spelling in their `decl.name` (`impl Hashable for i64`
    // arrives with `name == "i64"`).  Without this guard, the line below
    // creates `structTypes_["i64"] = StructType(name = "i64", body = {})`
    // and shadows the i64 primitive everywhere downstream — match-arm
    // binders for `Box<i64>` payloads (CodeGenMatch.cpp) then resolve T
    // to that empty struct and emit `load %i64, ptr %box.ptr`, which
    // mismatches the function's `i64` return type and trips the LLVM
    // verifier.  emitClassDecl already routes impl blocks through the
    // primitive-impl path (no struct layout needed); skip them here too.
    if (decl.kind == DeclKind::Class && decl.as<const ClassDecl>()->isImplBlock)
        return;

    // Uninstantiated generic templates: their struct body is never used for
    // concrete codegen (every real use routes through the mangled Mono
    // instantiation), but walking their fields forces `toLLVMType` to resolve
    // inner template references like `Dict<T, bool>`. When the user's program
    // never instantiates those inner concretes (HashSet<T> → Dict<T,bool>
    // cascade), the lookup would fire a spurious "not found in structTypes_"
    // diagnostic. Set `unreachableTemplateBodyWalk_` so unresolved field
    // types fall back to pointers silently — this is the single legitimate
    // carve-out from the strict "报错优于回退" policy: the laid-out placeholder
    // is never emitted, no object code ever references these fields.
    bool prevSoft = unreachableTemplateBodyWalk_;
    if (!decl.genericParams.empty()) unreachableTemplateBodyWalk_ = true;
    struct SoftGuard {
        bool& flag;
        bool prev;
        ~SoftGuard() { flag = prev; }
    } softGuard{unreachableTemplateBodyWalk_, prevSoft};

    const std::string& structParentName = (decl.kind == DeclKind::Class)
        ? decl.as<const ClassDecl>()->parentName
        : decl.as<const StructDecl>()->parentName;
    const std::vector<FieldDecl>& structFields = (decl.kind == DeclKind::Class)
        ? decl.as<const ClassDecl>()->fields
        : decl.as<const StructDecl>()->fields;

    bool isPacked = false;
    bool isUnion = false;
    bool isBitfield = false;
    for (auto& [an, av] : decl.attributes) {
        if (an == "packed") isPacked = true;
        if (an == "union") isUnion = true;
        if (an == "bitfield") isBitfield = true;
    }

    auto* structTy = llvm::StructType::create(*context_, decl.name);
    structTypes_[decl.name] = structTy;

    std::vector<llvm::Type*> fieldTypes;
    std::vector<std::string> fieldNames;
    std::vector<std::string> fieldTypeNames;

    if (!structParentName.empty()) {
        auto parentIt = structTypes_.find(structParentName);
        if (parentIt != structTypes_.end()) {
            auto parentFieldsIt = structFieldNames_.find(structParentName);
            if (parentFieldsIt != structFieldNames_.end()) {
                auto* parentTy = parentIt->second;
                for (size_t i = 0; i < parentFieldsIt->second.size(); ++i) {
                    fieldTypes.push_back(parentTy->getElementType(i));
                    fieldNames.push_back(parentFieldsIt->second[i]);
                }
                auto parentTypeNamesIt = structFieldTypeNames_.find(structParentName);
                if (parentTypeNamesIt != structFieldTypeNames_.end()) {
                    for (auto& tn : parentTypeNamesIt->second)
                        fieldTypeNames.push_back(tn);
                } else {
                    fieldTypeNames.resize(fieldNames.size(), "");
                }
            }
        }
    }

    for (auto& field : structFields) {
        if (!field.type) continue;
        // Static fields: emit as module-scope globals, not in per-instance struct layout
        if (field.isStatic) {
            std::string globalName = decl.name + "." + field.name;
            if (!module_->getGlobalVariable(globalName, true)) {
                auto* ft = toLLVMType(*field.type);
                // Initializer: prefer a compile-time constant from the
                // field's default-value expression. Without this the
                // global was always zero-initialized and reads of
                // `ClassName.STATIC_FIELD` returned 0/"" regardless of
                // what the declaration said.
                llvm::Constant* initC = llvm::Constant::getNullValue(ft);
                if (field.defaultValue) {
                    auto* init = field.defaultValue.get();
                    if (init->kind == ExprKind::IntLiteral && ft->isIntegerTy()) {
                        initC = llvm::ConstantInt::get(
                            ft, static_cast<uint64_t>(init->as<IntLiteralExpr>()->value),
                            /*signed=*/true);
                    } else if (init->kind == ExprKind::UnaryOp) {
                        auto* un = init->as<UnaryOpExpr>();
                        if (un->op == UnaryOp::Neg && un->operand &&
                            un->operand->kind == ExprKind::IntLiteral && ft->isIntegerTy()) {
                            initC = llvm::ConstantInt::get(
                                ft, static_cast<uint64_t>(-un->operand->as<IntLiteralExpr>()->value),
                                /*signed=*/true);
                        }
                    } else if (init->kind == ExprKind::FloatLiteral && ft->isFloatingPointTy()) {
                        initC = llvm::ConstantFP::get(
                            ft, init->as<FloatLiteralExpr>()->value);
                    } else if (init->kind == ExprKind::BoolLiteral && ft->isIntegerTy()) {
                        initC = llvm::ConstantInt::get(
                            ft, init->as<BoolLiteralExpr>()->value ? 1 : 0);
                    } else if (init->kind == ExprKind::StringLiteral) {
                        // __String is { ptr, len, cap, owned }. Build a
                        // constant with ptr → interned literal bytes,
                        // len = literal size, cap = 0 (non-owned), owned
                        // = 0 so runtime code doesn't try to free it.
                        auto* strTy = getOrCreateStringType();
                        if (ft == strTy) {
                            const std::string& s = init->as<StringLiteralExpr>()->value;
                            auto* bytes = llvm::ConstantDataArray::getString(*context_, s, /*AddNull=*/true);
                            auto* bytesGV = new llvm::GlobalVariable(
                                *module_, bytes->getType(), /*isConstant=*/true,
                                llvm::GlobalValue::PrivateLinkage, bytes,
                                ".staticstr." + decl.name + "." + field.name);
                            bytesGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
                            initC = llvm::ConstantStruct::get(strTy, {
                                bytesGV,
                                llvm::ConstantInt::get(i64Ty, s.size()),
                                llvm::ConstantInt::get(i64Ty, 0),
                                llvm::ConstantInt::get(i64Ty, 0),
                            });
                        }
                    }
                }
                auto* gv = new llvm::GlobalVariable(
                    *module_, ft, /*isConstant=*/false,
                    llvm::GlobalValue::ExternalLinkage,
                    initC,
                    globalName);
                (void)gv;
            }
            continue;
        }
        {
            auto* ft = toLLVMType(*field.type);
            fieldTypes.push_back(ft);
            fieldNames.push_back(field.name);
            std::string ftName;
            if (field.type->kind == TypeAnnotationKind::Reference) {
                auto* ref = field.type->as<const ReferenceType>();
                if (ref && ref->innerType) ftName = resolveAndMangleTypeAnnotation(*ref->innerType);
            } else if (field.type->kind == TypeAnnotationKind::Pointer) {
                auto* ptr = field.type->as<const PointerType>();
                if (ptr && ptr->innerType) ftName = resolveAndMangleTypeAnnotation(*ptr->innerType);
            } else {
                ftName = resolveAndMangleTypeAnnotation(*field.type);
            }
            fieldTypeNames.push_back(ftName);
            for (auto& extra : field.extraNames) {
                fieldTypes.push_back(ft);
                fieldNames.push_back(extra);
                fieldTypeNames.push_back(ftName);
            }
        }
    }

    if (isBitfield && !fieldTypes.empty()) {
        int totalBits = 0;
        for (auto* ft : fieldTypes) {
            totalBits += static_cast<int>(module_->getDataLayout().getTypeAllocSize(ft) * 8);
        }
        llvm::Type* backingTy;
        if (totalBits <= 8) backingTy = llvm::Type::getInt8Ty(*context_);
        else if (totalBits <= 16) backingTy = llvm::Type::getInt16Ty(*context_);
        else if (totalBits <= 32) backingTy = llvm::Type::getInt32Ty(*context_);
        else backingTy = llvm::Type::getInt64Ty(*context_);

        structTy->setBody({backingTy}, true);
        structFieldNames_[decl.name] = std::move(fieldNames);

        int bitOffset = 0;
        for (size_t i = 0; i < fieldTypes.size() && i < structFields.size(); ++i) {
            int width = static_cast<int>(module_->getDataLayout().getTypeAllocSize(fieldTypes[i]) * 8);
            bitfieldLayouts_[decl.name + "." + structFields[i].name] = {bitOffset, width};
            bitOffset += width;
        }
        return;
    }

    if (isUnion && !fieldTypes.empty()) {
        uint64_t maxSize = 0;
        for (auto* ft : fieldTypes) {
            uint64_t sz = module_->getDataLayout().getTypeAllocSize(ft);
            if (sz > maxSize) maxSize = sz;
        }
        structTy->setBody({llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), maxSize)});
        structFieldNames_[decl.name] = std::move(fieldNames);
        return;
    }

    structTy->setBody(fieldTypes, isPacked);
    structFieldNames_[decl.name] = std::move(fieldNames);
    structFieldTypeNames_[decl.name] = std::move(fieldTypeNames);
}

// getOrCreateGenericStructType has been removed. Post-Mono, every concrete
// generic struct type is emitted by Monomorphize::instantiate() and pre-
// registered in structTypes_ before CodeGen runs. Call sites now do a direct
// structTypes_.find() lookup and emit a diagnostic on miss (Mono scan gap).
} // namespace vyx
