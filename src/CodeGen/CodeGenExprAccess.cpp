#include "CodeGenIncludes.h"

namespace vyx {

llvm::Value* CodeGen::emitMemberAccess(const Expr& expr) {
    auto* maE = expr.as<const MemberAccessExpr>();
    if (!maE->object) {
        diag_.error(maE->location, "member access: missing object expression");
        return nullptr;
    }

    // typeinfo(T).name / .size / .fields — compile-time reflection
    if (maE->object->kind == ExprKind::Call) {
        auto* objCall = maE->object->as<const CallExpr>();
        if (objCall->callee && objCall->callee->kind == ExprKind::Identifier &&
            objCall->callee->as<const IdentifierExpr>()->name == "typeinfo") {
        if (objCall->args.empty() || !objCall->args[0]) {
            diag_.error(maE->location, "typeinfo() requires an identifier argument");
            return nullptr;
        }
        std::string typeName;
        int typeSize = 0;
        int fieldCount = 0;
        std::string kindLabel;
        if (objCall->args[0]->kind == ExprKind::Identifier) {
            auto* argIdent = objCall->args[0]->as<const IdentifierExpr>();
            if (argIdent->typeAnnotation) typeName = argIdent->typeAnnotation->name;
            else typeName = argIdent->name;
        }
        if (typeName.empty()) {
            diag_.error(maE->location, "typeinfo: could not resolve a type name from the argument");
            return nullptr;
        }

        // Primitive-type reflection: answer .name / .size / .kind without
        // requiring a registered struct. Lets `typeinfo(i64)` /
        // `typeinfo(string)` etc. work uniformly with user-defined types.
        struct PrimInfo { const char* name; int size; const char* kind; };
        static const PrimInfo kPrims[] = {
            {"i8",   1, "int"},   {"i16",  2, "int"},
            {"i32",  4, "int"},   {"i64",  8, "int"},
            {"u8",   1, "uint"},  {"u16",  2, "uint"},
            {"u32",  4, "uint"},  {"u64",  8, "uint"},
            {"f32",  4, "float"}, {"f64",  8, "float"},
            {"bool", 1, "bool"},  {"char", 4, "char"},
            {"isize", 8, "int"},  {"usize", 8, "uint"},
            {"rawptr", 8, "pointer"},
            {"void",  0, "void"},
            {"str", 32, "string"},
            {"string", 32, "string"},
        };
        bool isPrim = false;
        for (auto& p : kPrims) {
            if (typeName == p.name) {
                typeSize = p.size;
                kindLabel = p.kind;
                fieldCount = 0;
                isPrim = true;
                break;
            }
        }

        if (!isPrim) {
            auto sit = structTypes_.find(typeName);
            if (sit != structTypes_.end()) {
                typeSize = static_cast<int>(module_->getDataLayout().getTypeAllocSize(sit->second).getFixedValue());
                fieldCount = sit->second->getNumElements();
                // Classify: enum ADT (tagged union with __tag/__data fields),
                // class (uppercase-starting name), or plain struct.
                auto nmIt = structFieldNames_.find(typeName);
                if (nmIt != structFieldNames_.end() && nmIt->second.size() >= 2 &&
                    nmIt->second[0] == "__tag" && nmIt->second[1] == "__data") {
                    kindLabel = "enum";
                } else if (!typeName.empty() &&
                           std::isupper(static_cast<unsigned char>(typeName[0]))) {
                    kindLabel = "class";
                } else {
                    kindLabel = "struct";
                }
            } else {
                TypeAnnotation ann;
                ann.kind = TypeAnnotationKind::Named;
                ann.name = typeName;
                auto* llvmTy = toLLVMType(ann);
                if (llvmTy && !llvmTy->isVoidTy())
                    typeSize = static_cast<int>(module_->getDataLayout().getTypeAllocSize(llvmTy).getFixedValue());
                kindLabel = "unknown";
            }
        }
        if (maE->member == "name") return createStringValue(typeName);
        if (maE->member == "size") return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), typeSize);
        if (maE->member == "fields") return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), fieldCount);
        if (maE->member == "kind") return createStringValue(kindLabel);
        return createStringValue(typeName + " (size=" + std::to_string(typeSize) +
            ", fields=" + std::to_string(fieldCount) + ", kind=" + kindLabel + ")");
        }
    }

    // self.field inside class methods
    if (maE->object->kind == ExprKind::SelfExpr && !currentClassName_.empty()) {
        auto selfIt = namedValues_.find("self");
        auto stIt = structTypes_.find(currentClassName_);
        if (selfIt != namedValues_.end() && stIt != structTypes_.end()) {
            auto* selfPtr = builder_->CreateLoad(getValuePtrType(selfIt->second), selfIt->second, "self.ptr");
            auto* structTy = stIt->second;
            auto fnIt = structFieldNames_.find(currentClassName_);
            if (fnIt != structFieldNames_.end()) {
                auto& fieldNames = fnIt->second;
                for (size_t i = 0; i < fieldNames.size(); ++i) {
                    if (fieldNames[i] == maE->member) {
                        auto* gep = builder_->CreateStructGEP(structTy, selfPtr, i, "self." + maE->member);
                        return builder_->CreateLoad(structTy->getElementType(i), gep, maE->member + ".val");
                    }
                }
            }
        }
    }

    // Regular member access: obj.field
    if (maE->object->kind == ExprKind::Identifier) {
        auto it = namedValues_.find(maE->object->as<const IdentifierExpr>()->name);
        if (it != namedValues_.end()) {
            auto* allocaTy = getValuePtrType(it->second);

            // Direct struct value
            if (auto* structTy = llvm::dyn_cast<llvm::StructType>(allocaTy)) {
                // `.len` fast-path: only apply when this type's index-1
                // slot is actually named `len`. Previously the path fired
                // unconditionally for any struct with ≥3 fields, which
                // broke classes whose `len` field lives at a different
                // offset (e.g. Deque with layout {data, cap, head, tail,
                // len, elem_size}: .len used to return cap instead).
                if (maE->member == "len") {
                    std::string typeName = structTy->getName().str();
                    auto fnIt = structFieldNames_.find(typeName);
                    bool lenAtIndex1 = false;
                    if (fnIt != structFieldNames_.end() && fnIt->second.size() > 1
                        && fnIt->second[1] == "len") {
                        lenAtIndex1 = true;
                    } else if (fnIt == structFieldNames_.end() && structTy->getNumElements() >= 3) {
                        // Unknown type without registered names (opaque builtin like
                        // Vec/string before registration): keep the legacy assumption.
                        lenAtIndex1 = true;
                    }
                    if (lenAtIndex1) {
                        auto* lenPtr = builder_->CreateStructGEP(structTy, it->second, 1, "vec.len.ptr");
                        return builder_->CreateLoad(llvm::Type::getInt64Ty(*context_), lenPtr, "vec.len");
                    }
                    // Fall through to the name-based field lookup below.
                }

                // Tuple element access: _0, _1, _2, ...
                if (maE->member.size() >= 2 && maE->member[0] == '_' &&
                    std::all_of(maE->member.begin() + 1, maE->member.end(), ::isdigit)) {
                    unsigned idx = std::stoul(maE->member.substr(1));
                    if (idx < structTy->getNumElements()) {
                        auto* gep = builder_->CreateStructGEP(structTy, it->second, idx, maE->member);
                        return builder_->CreateLoad(structTy->getElementType(idx), gep, maE->member + ".val");
                    }
                }

                std::string typeName = structTy->getName().str();

                // Bitfield access: extract bits using shift + mask
                auto bfIt = bitfieldLayouts_.find(typeName + "." + maE->member);
                if (bfIt != bitfieldLayouts_.end()) {
                    auto [bitOffset, bitWidth] = bfIt->second;
                    auto* backingPtr = builder_->CreateStructGEP(structTy, it->second, 0, "bf.ptr");
                    auto* backing = builder_->CreateLoad(structTy->getElementType(0), backingPtr, "bf.val");
                    auto* shifted = (bitOffset > 0)
                        ? builder_->CreateLShr(backing, bitOffset, "bf.shifted")
                        : backing;
                    auto* mask = llvm::ConstantInt::get(shifted->getType(), (1ULL << bitWidth) - 1);
                    return builder_->CreateAnd(shifted, mask, maE->member + ".bf");
                }

                auto fnIt = structFieldNames_.find(typeName);
                if (fnIt != structFieldNames_.end()) {
                    auto& fieldNames = fnIt->second;
                    for (size_t i = 0; i < fieldNames.size(); ++i) {
                        if (fieldNames[i] == maE->member) {
                            auto* gep = builder_->CreateStructGEP(structTy, it->second, i, maE->member);
                            return builder_->CreateLoad(structTy->getElementType(i), gep, maE->member + ".val");
                        }
                    }
                }

                // Ref<T> auto-deref: .kind on Ref<ADT> → load ADT, return __tag
                if (typeName == "__RefCounted" && maE->member == "kind") {
                    std::string innerType2 = resolveRefInner(*maE->object,
                        maE->object->as<const IdentifierExpr>()->name);
                    // Search all ADT types as fallback
                    if (innerType2.empty()) {
                        for (auto& [sn, stTy2] : structTypes_) {
                            auto sfIt2 = structFieldNames_.find(sn);
                            if (sfIt2 == structFieldNames_.end()) continue;
                            if (sfIt2->second.size() >= 2 && sfIt2->second[0] == "__tag" && sfIt2->second[1] == "__data") {
                                innerType2 = sn;
                                break;
                            }
                        }
                    }
                    if (!innerType2.empty()) {
                        auto innerStIt = structTypes_.find(innerType2);
                        if (innerStIt != structTypes_.end()) {
                            auto innerFnIt = structFieldNames_.find(innerType2);
                            if (innerFnIt != structFieldNames_.end() && innerFnIt->second.size() >= 2 &&
                                innerFnIt->second[0] == "__tag" && innerFnIt->second[1] == "__data") {
                                auto* dataPtrGEP = builder_->CreateStructGEP(structTy, it->second, 0, "ref.data.ptr");
                                auto* rawPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), dataPtrGEP, "ref.raw.ptr");
                                auto* adtVal = builder_->CreateLoad(innerStIt->second, rawPtr, "ref.adt");
                                auto* fn5 = builder_->GetInsertBlock()->getParent();
                                auto* tmpA2 = createEntryBlockAlloca(fn5, innerStIt->second, "ref.kind.tmp");
                                builder_->CreateStore(adtVal, tmpA2);
                                auto* tagPtr = builder_->CreateStructGEP(innerStIt->second, tmpA2, 0, "ref.adt.kind");
                                return builder_->CreateLoad(llvm::Type::getInt32Ty(*context_), tagPtr, "kind.val");
                            }
                        }
                    }
                }

                // Ref<T> auto-deref: load inner T through data pointer, then access field
                if (typeName == "__RefCounted") {
                    std::string innerTypeName = resolveRefInner(*maE->object,
                        maE->object->as<const IdentifierExpr>()->name);
                    // Fallback: when inner type is unknown, search class/struct types for the member
                    // Only use fallback if the match is unique (no ambiguity)
                    if (innerTypeName.empty()) {
                        int matchCount = 0;
                        std::string candidateName;
                        for (auto& [stName, stTy] : structTypes_) {
                            // Skip compiler-internal layout aliases and well-known generic
                            // container/ADT families.  Option / Result are pulled through
                            // the lang-item helper so stdlib renames are honoured.
                            if (stName.starts_with("__") || stName.starts_with("Box<") ||
                                stName.starts_with("Vec<") || stName.starts_with("Dict<") ||
                                isOptionOrResultSlotName(stName)) continue;
                            auto sfIt = structFieldNames_.find(stName);
                            if (sfIt == structFieldNames_.end()) continue;
                            if (sfIt->second.size() >= 2 && sfIt->second[0] == "__tag" &&
                                sfIt->second[1] == "__data") continue;
                            for (auto& fn : sfIt->second) {
                                if (fn == maE->member) {
                                    candidateName = stName;
                                    matchCount++;
                                    break;
                                }
                            }
                        }
                        if (matchCount == 1) innerTypeName = candidateName;
                    }
                    if (!innerTypeName.empty()) {
                        auto innerStIt = structTypes_.find(innerTypeName);
                        if (innerStIt != structTypes_.end()) {
                            auto* innerTy = innerStIt->second;
                            auto innerFnIt = structFieldNames_.find(innerTypeName);
                            if (innerFnIt != structFieldNames_.end()) {
                                auto& innerFields = innerFnIt->second;
                                for (size_t i = 0; i < innerFields.size(); ++i) {
                                    if (innerFields[i] == maE->member) {
                                        auto* dataPtrGEP = builder_->CreateStructGEP(structTy, it->second, 0, "ref.data.ptr");
                                        auto* rawPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), dataPtrGEP, "ref.raw.ptr");
                                        auto* fieldGEP = builder_->CreateStructGEP(innerTy, rawPtr, i, "ref." + maE->member);
                                        auto* fieldVal = builder_->CreateLoad(innerTy->getElementType(i), fieldGEP, maE->member + ".val");
                                        return fieldVal;
                                    }
                                }
                                // ADT inner type: field not in __tag/__data, search variant sub-structs
                                bool isADT = innerFields.size() >= 2 &&
                                             innerFields[0] == "__tag" && innerFields[1] == "__data";
                                if (isADT) {
                                    for (auto& [stName2, stTy2] : structTypes_) {
                                        if (stName2.starts_with("__") || stName2 == innerTypeName) continue;
                                        auto sfIt2 = structFieldNames_.find(stName2);
                                        if (sfIt2 == structFieldNames_.end()) continue;
                                        if (sfIt2->second.size() >= 2 && sfIt2->second[0] == "__tag" &&
                                            sfIt2->second[1] == "__data") continue;
                                        for (size_t fi = 0; fi < sfIt2->second.size(); ++fi) {
                                            if (sfIt2->second[fi] == maE->member) {
                                                auto* ptrTy2 = llvm::PointerType::getUnqual(*context_);
                                                auto* dataPtrGEP = builder_->CreateStructGEP(structTy, it->second, 0, "ref.data.ptr");
                                                auto* rawPtr = builder_->CreateLoad(ptrTy2, dataPtrGEP, "ref.raw.ptr");
                                                auto* dataAreaPtr = builder_->CreateStructGEP(innerTy, rawPtr, 1, "adt.data.ptr");
                                                auto* fieldGEP = builder_->CreateStructGEP(stTy2, dataAreaPtr, fi, "common." + maE->member);
                                                return builder_->CreateLoad(stTy2->getElementType(fi), fieldGEP, maE->member + ".val");
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Pointer to struct (for class instances)
            if (allocaTy->isPointerTy()) {
                auto& objIdn = maE->object->as<const IdentifierExpr>()->name;
                auto* ptrVal = builder_->CreateLoad(allocaTy, it->second, objIdn + ".ptr");
                std::string cvName = resolveClassName(*maE->object, objIdn);
                if (!cvName.empty()) {
                    auto stIt2 = structTypes_.find(cvName);
                    if (stIt2 != structTypes_.end()) {
                        auto fnIt2 = structFieldNames_.find(cvName);
                        if (fnIt2 != structFieldNames_.end()) {
                            auto& fieldNames = fnIt2->second;
                            for (size_t i = 0; i < fieldNames.size(); ++i) {
                                if (fieldNames[i] == maE->member) {
                                    auto* gep = builder_->CreateStructGEP(stIt2->second, ptrVal, i, maE->member);
                                    return builder_->CreateLoad(stIt2->second->getElementType(i), gep, maE->member + ".val");
                                }
                            }
                        }
                    }
                }
                llvm::Type* pointeeTy = maE->object ? inferredPointeeLLVM(*maE->object) : nullptr;
                if (!pointeeTy) {
                    auto ptIt = ptrElemTypes_.find(maE->object->as<const IdentifierExpr>()->name);
                    if (ptIt != ptrElemTypes_.end()) pointeeTy = ptIt->second;
                }
                if (pointeeTy) {
                    if (auto* ptrStructTy = llvm::dyn_cast<llvm::StructType>(pointeeTy)) {
                        std::string ptrTypeName = ptrStructTy->hasName() ? ptrStructTy->getName().str() : "";
                        auto fnIt3 = structFieldNames_.find(ptrTypeName);
                        if (fnIt3 != structFieldNames_.end()) {
                            auto& fieldNames = fnIt3->second;
                            for (size_t i = 0; i < fieldNames.size(); ++i) {
                                if (fieldNames[i] == maE->member) {
                                    auto* gep = builder_->CreateStructGEP(ptrStructTy, ptrVal, i, maE->member);
                                    return builder_->CreateLoad(ptrStructTy->getElementType(i), gep, maE->member + ".val");
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Static field read: ClassName.fieldName — resolve to module-scope global
    if (maE->object && maE->object->kind == ExprKind::Identifier) {
        std::string objName = maE->object->as<const IdentifierExpr>()->name;
        // Only attempt this when the identifier is not a local variable
        // (i.e., it's a class/type name, not an instance variable).
        if (!namedValues_.count(objName)) {
            std::string globalName = objName + "." + maE->member;
            auto* gv = module_->getGlobalVariable(globalName, true);
            if (gv) {
                return builder_->CreateLoad(gv->getValueType(), gv, globalName + ".val");
            }
        }
    }

    // Data-less enum/ADT variant access: EnumName.Variant (no args) — checked early
    if (maE->object && maE->object->kind == ExprKind::Identifier) {
        std::string enumName = maE->object->as<const IdentifierExpr>()->name;
        auto evIt = errorEnumValues_.find(enumName + "." + maE->member);
        if (evIt != errorEnumValues_.end()) {
            auto stIt = structTypes_.find(enumName);
            if (stIt != structTypes_.end()) {
                auto* adtTy = stIt->second;
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* alloca = createEntryBlockAlloca(fn, adtTy, "adt.tmp");
                auto* tagPtr = builder_->CreateStructGEP(adtTy, alloca, 0, "adt.tag");
                builder_->CreateStore(
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), evIt->second), tagPtr);
                if (adtTy->getNumElements() > 1) {
                    auto* dataPtr = builder_->CreateStructGEP(adtTy, alloca, 1, "adt.data");
                    auto* dataTy = adtTy->getElementType(1);
                    builder_->CreateStore(llvm::Constant::getNullValue(dataTy), dataPtr);
                }
                return builder_->CreateLoad(adtTy, alloca, "adt.val");
            }
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), evIt->second);
        }
    }

    // Fallback: evaluate expression, create temp alloca for field/property access
    if (maE->object) {
        auto* objVal = emitExpr(*maE->object);
        if (objVal) {
            auto* objTy = objVal->getType();
            // For pointer types (references), check Sema-inferred class name and access through pointer
            if (objTy->isPointerTy() && maE->object->kind == ExprKind::Identifier) {
                std::string cvName = resolveClassName(*maE->object,
                    maE->object->as<const IdentifierExpr>()->name);
                if (!cvName.empty()) {
                    auto stIt = structTypes_.find(cvName);
                    if (stIt != structTypes_.end()) {
                        auto* structTy = stIt->second;
                        auto fnIt2 = structFieldNames_.find(cvName);
                        if (fnIt2 != structFieldNames_.end()) {
                            auto& fieldNames = fnIt2->second;
                            for (size_t i = 0; i < fieldNames.size(); ++i) {
                                if (fieldNames[i] == maE->member) {
                                    auto* gep = builder_->CreateStructGEP(structTy, objVal, i, maE->member + ".ptr");
                                    return builder_->CreateLoad(structTy->getElementType(i), gep, maE->member + ".val");
                                }
                            }
                        }
                    }
                }
            }
            // .kind on pointer-to-ADT (Box<ADT>): deref pointer and return __tag
            if (objTy->isPointerTy() && maE->member == "kind") {
                // Determine inner type via field type tracking or refInnerTypeNames_
                std::string innerTypeName;
                if (maE->object->kind == ExprKind::MemberAccess) {
                    auto& innerMA = *maE->object->as<const MemberAccessExpr>();
                    if (innerMA.object && innerMA.object->kind == ExprKind::Identifier) {
                        auto& parentVar = innerMA.object->as<const IdentifierExpr>()->name;
                        std::string parentTypeName = resolveClassName(*innerMA.object, parentVar);
                        if (parentTypeName.empty()) {
                            auto nv2 = namedValues_.find(parentVar);
                            if (nv2 != namedValues_.end()) {
                                auto* pt2 = getValuePtrType(nv2->second);
                                if (auto* st2 = llvm::dyn_cast<llvm::StructType>(pt2))
                                    if (st2->hasName()) parentTypeName = st2->getName().str();
                            }
                        }
                        if (!parentTypeName.empty()) {
                            auto sft2 = structFieldTypeNames_.find(parentTypeName);
                            auto sfn2 = structFieldNames_.find(parentTypeName);
                            if (sft2 != structFieldTypeNames_.end() && sfn2 != structFieldNames_.end()) {
                                for (size_t fi = 0; fi < sfn2->second.size() && fi < sft2->second.size(); ++fi) {
                                    if (sfn2->second[fi] == innerMA.member) {
                                        innerTypeName = sft2->second[fi];
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                if (innerTypeName.empty() && maE->object->kind == ExprKind::Identifier) {
                    innerTypeName = resolveRefInner(*maE->object,
                        maE->object->as<const IdentifierExpr>()->name);
                }
                auto tryLoadAdtKind = [&](const std::string& tn) -> llvm::Value* {
                    auto stIt2 = structTypes_.find(tn);
                    if (stIt2 == structTypes_.end()) return nullptr;
                    auto fnIt3 = structFieldNames_.find(tn);
                    if (fnIt3 == structFieldNames_.end() || fnIt3->second.size() < 2 ||
                        fnIt3->second[0] != "__tag" || fnIt3->second[1] != "__data") return nullptr;
                    auto* adtVal2 = builder_->CreateLoad(stIt2->second, objVal, "box.adt.kind");
                    auto* fn2 = builder_->GetInsertBlock()->getParent();
                    auto* tmpA = createEntryBlockAlloca(fn2, stIt2->second, "kind.tmp");
                    builder_->CreateStore(adtVal2, tmpA);
                    auto* tagPtr = builder_->CreateStructGEP(stIt2->second, tmpA, 0, "adt.kind");
                    return builder_->CreateLoad(llvm::Type::getInt32Ty(*context_), tagPtr, "kind.val");
                };
                if (!innerTypeName.empty()) {
                    auto* result = tryLoadAdtKind(innerTypeName);
                    if (result) return result;
                }
            }
            if (auto* structTy = llvm::dyn_cast<llvm::StructType>(objTy)) {
                auto* fn = builder_->GetInsertBlock()->getParent();
                auto* tmpAlloca = createEntryBlockAlloca(fn, structTy, "tmp.field");
                builder_->CreateStore(objVal, tmpAlloca);

                // ADT enum .kind access: return the __tag value
                if (maE->member == "kind" && structTy->hasName()) {
                    std::string typeName2 = structTy->getName().str();
                    auto fnIt2 = structFieldNames_.find(typeName2);
                    if (fnIt2 != structFieldNames_.end() && fnIt2->second.size() >= 2 &&
                        fnIt2->second[0] == "__tag" && fnIt2->second[1] == "__data") {
                        auto* tagPtr = builder_->CreateStructGEP(structTy, tmpAlloca, 0, "adt.kind");
                        return builder_->CreateLoad(llvm::Type::getInt32Ty(*context_), tagPtr, "kind.val");
                    }
                }

                if (structTy == getOrCreateStringType() && maE->member == "len") {
                    auto* lenPtr = builder_->CreateStructGEP(structTy, tmpAlloca, 1, "str.len.ptr");
                    return builder_->CreateLoad(llvm::Type::getInt64Ty(*context_), lenPtr, "str.len");
                }

                if (maE->member == "len") {
                    std::string typeName = structTy->getName().str();
                    auto fnIt = structFieldNames_.find(typeName);
                    bool lenAtIndex1 = false;
                    if (fnIt != structFieldNames_.end() && fnIt->second.size() > 1
                        && fnIt->second[1] == "len") {
                        lenAtIndex1 = true;
                    } else if (fnIt == structFieldNames_.end() && structTy->getNumElements() >= 3) {
                        lenAtIndex1 = true;
                    }
                    if (lenAtIndex1) {
                        auto* lenPtr = builder_->CreateStructGEP(structTy, tmpAlloca, 1, "vec.len.ptr");
                        return builder_->CreateLoad(llvm::Type::getInt64Ty(*context_), lenPtr, "vec.len");
                    }
                }

                if (maE->member.size() >= 2 && maE->member[0] == '_' &&
                    std::all_of(maE->member.begin() + 1, maE->member.end(), ::isdigit)) {
                    unsigned idx = std::stoul(maE->member.substr(1));
                    if (idx < structTy->getNumElements()) {
                        auto* gep = builder_->CreateStructGEP(structTy, tmpAlloca, idx, maE->member);
                        return builder_->CreateLoad(structTy->getElementType(idx), gep, maE->member + ".val");
                    }
                }

                std::string typeName = structTy->hasName() ? structTy->getName().str() : "";
                auto fnIt = structFieldNames_.find(typeName);
                if (fnIt != structFieldNames_.end()) {
                    auto& fieldNames = fnIt->second;
                    for (size_t i = 0; i < fieldNames.size(); ++i) {
                        if (fieldNames[i] == maE->member) {
                            auto* gep = builder_->CreateStructGEP(structTy, tmpAlloca, i, maE->member);
                            return builder_->CreateLoad(structTy->getElementType(i), gep, maE->member + ".val");
                        }
                    }
                }
                // Fallback: field not found on declared type, search all structs
                // (handles ADT variant type mismatches)
                bool isADTFallback = false;
                if (fnIt != structFieldNames_.end() && fnIt->second.size() >= 2 &&
                    fnIt->second[0] == "__tag" && fnIt->second[1] == "__data")
                    isADTFallback = true;
                for (auto& [stName, stTy2] : structTypes_) {
                    if (stName.starts_with("__") || stName == typeName) continue;
                    auto sfIt = structFieldNames_.find(stName);
                    if (sfIt == structFieldNames_.end()) continue;
                    if (sfIt->second.size() >= 2 && sfIt->second[0] == "__tag") continue;
                    for (size_t fi = 0; fi < sfIt->second.size(); ++fi) {
                        if (sfIt->second[fi] == maE->member) {
                            if (isADTFallback) {
                                auto* dataAreaPtr = builder_->CreateStructGEP(structTy, tmpAlloca, 1, "adt.data.fb");
                                auto* gep = builder_->CreateStructGEP(stTy2, dataAreaPtr, fi, "common." + maE->member);
                                return builder_->CreateLoad(stTy2->getElementType(fi), gep, maE->member + ".val");
                            }
                            if (stTy2->getNumElements() <= structTy->getNumElements()) {
                                auto* reloadAlloca = createEntryBlockAlloca(
                                    builder_->GetInsertBlock()->getParent(), stTy2, "recast.tmp");
                                builder_->CreateStore(
                                    builder_->CreateLoad(stTy2, tmpAlloca, "recast.load"),
                                    reloadAlloca);
                                auto* gep = builder_->CreateStructGEP(stTy2, reloadAlloca, fi, maE->member);
                                return builder_->CreateLoad(stTy2->getElementType(fi), gep, maE->member + ".val");
                            }
                        }
                    }
                }
            }
        }
    }

    // Box<T>/ptr/Ref<T> auto-deref: tracked inner type for field access
    if (maE->object && maE->object->kind == ExprKind::Identifier) {
        auto& objName = maE->object->as<IdentifierExpr>()->name;
        std::string refInner = resolveRefInner(*maE->object, objName);
        if (!refInner.empty()) {
            auto innerStIt = structTypes_.find(refInner);
            if (innerStIt != structTypes_.end()) {
                auto* innerTy = innerStIt->second;
                auto innerFnIt = structFieldNames_.find(refInner);
                if (innerFnIt != structFieldNames_.end()) {
                    auto& innerFields = innerFnIt->second;
                    bool isADT = innerFields.size() >= 2 &&
                                 innerFields[0] == "__tag" && innerFields[1] == "__data";
                    if (!isADT) {
                        auto nIt = namedValues_.find(maE->object->as<IdentifierExpr>()->name);
                        if (nIt != namedValues_.end()) {
                            auto* rawPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), nIt->second, "box.ptr");
                            for (size_t i = 0; i < innerFields.size(); ++i) {
                                if (innerFields[i] == maE->member) {
                                    auto* gep = builder_->CreateStructGEP(innerTy, rawPtr, i, "box." + maE->member);
                                    return builder_->CreateLoad(innerTy->getElementType(i), gep, "box." + maE->member + ".val");
                                }
                            }
                        }
                    } else if (unit_) {
                        auto nIt = namedValues_.find(maE->object->as<IdentifierExpr>()->name);
                        if (nIt != namedValues_.end()) {
                            auto* rawPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), nIt->second, "box.ptr");
                            auto* adtVal = builder_->CreateLoad(innerTy, rawPtr, "box.adt");
                            auto* fn3 = builder_->GetInsertBlock()->getParent();
                            auto* tmpAlloca2 = createEntryBlockAlloca(fn3, innerTy, "adt.tmp");
                            builder_->CreateStore(adtVal, tmpAlloca2);
                            auto* dataPtr = builder_->CreateStructGEP(innerTy, tmpAlloca2, 1, "adt.data");
                            std::string adtBaseName = refInner;
                            if (auto ltPos = adtBaseName.find('<'); ltPos != std::string::npos)
                                adtBaseName = adtBaseName.substr(0, ltPos);
                            for (auto& d : unit_->declarations) {
                                if (!d || d->kind != DeclKind::ErrorDef || d->name != adtBaseName) continue;
                                auto& variantTypes = d->as<ErrorDefDecl>()->variantTypes;
                                for (size_t vi = 0; vi < variantTypes.size(); ++vi) {
                                    uint64_t offset = 0;
                                    for (size_t fi = 0; fi < variantTypes[vi].size(); ++fi) {
                                        auto* fieldTy = toLLVMType(*variantTypes[vi][fi]);
                                        std::string mangledVarType = mangleTypeAnnotation(*variantTypes[vi][fi]);
                                        auto varStIt = structTypes_.find(mangledVarType);
                                        if (varStIt != structTypes_.end()) {
                                            auto varFnIt = structFieldNames_.find(mangledVarType);
                                            if (varFnIt != structFieldNames_.end()) {
                                                for (size_t si = 0; si < varFnIt->second.size(); ++si) {
                                                    if (varFnIt->second[si] == maE->member) {
                                                        auto* fieldAddr = builder_->CreateGEP(
                                                            llvm::Type::getInt8Ty(*context_), dataPtr,
                                                            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), offset));
                                                        auto* gep = builder_->CreateStructGEP(varStIt->second, fieldAddr, si, "adt." + maE->member);
                                                        return builder_->CreateLoad(varStIt->second->getElementType(si), gep, "adt." + maE->member + ".val");
                                                    }
                                                }
                                            }
                                        }
                                        offset += module_->getDataLayout().getTypeAllocSize(fieldTy);
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // Uniform access: when field lookup fails but a zero-arg method with the
    // same name exists, call it. This makes `hm.len` work on HashMap even
    // though len is defined as `fn len() -> i64`. Without this, field-style
    // access silently returned 0 (the fallthrough default), which masked
    // typos and hid real bugs.
    {
        std::string lookupClass;
        if (maE->object) {
            if (maE->object->kind == ExprKind::Identifier) {
                auto& nm = maE->object->as<const IdentifierExpr>()->name;
                lookupClass = resolveClassName(*maE->object, nm);
            } else if (maE->object->kind == ExprKind::SelfExpr && !currentClassName_.empty()) {
                lookupClass = currentClassName_;
            }
        }
        if (!lookupClass.empty()) {
            if (auto* methodFn = findClassInstanceMethod(lookupClass, maE->member)) {
                // Zero-arg methods only — field access can't supply call args.
                size_t expected = methodFn->getFunctionType()->getNumParams();
                // Non-static has implicit self ptr, so expected == 1.
                if (expected == 1) {
                    llvm::Value* objAlloca = nullptr;
                    if (maE->object->kind == ExprKind::Identifier) {
                        auto nvIt = namedValues_.find(maE->object->as<const IdentifierExpr>()->name);
                        if (nvIt != namedValues_.end()) objAlloca = nvIt->second;
                    } else if (maE->object->kind == ExprKind::SelfExpr) {
                        auto selfIt = namedValues_.find("self");
                        if (selfIt != namedValues_.end()) {
                            objAlloca = builder_->CreateLoad(
                                getValuePtrType(selfIt->second), selfIt->second, "self.ptr");
                        }
                    }
                    if (objAlloca) {
                        return builder_->CreateCall(methodFn, {objAlloca}, maE->member + ".uniform");
                    }
                }
            }
        }
    }

    diag_.warning(expr.location, "no member '{}' found on expression — returning default", maE->member);
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
}

llvm::Value* CodeGen::emitStringInterpolation(const Expr& expr) {
    auto* siE = expr.as<const StringInterpExpr>();
    std::string fmtStr;
    std::vector<llvm::Value*> fmtArgs;
    auto* fn = builder_->GetInsertBlock()->getParent();
    auto* strTyShared = getOrCreateStringType();

    // Phase 7 (2026-04-23): `${x}` dispatches through `Trait Display`.
    // For each interp expression, first attempt a trait-method call to
    // `<ClassName>.display(self)` (mangled like any other class method,
    // populated for primitives via the `impl Display for <prim>` blocks in
    // std/fmt.vyx and for user classes via regular `impl` bodies).  When
    // the lookup succeeds we concatenate the returned `string` via `%.*s`,
    // which is byte-identical to the existing `string` path.  The classic
    // hardcoded switch (`%lld`/`%d`/`%f`/`%s`) is kept as a fallback for
    // types whose Display is not (yet) registered so existing tests keep
    // working silently — matches the "keep the fallback" constraint.
    //
    // Key derivation mirrors mangleVyxTypeForCodegen / Sema::primTraitKey so
    // `i32` / `f64` / `bool` / `char` / `string` all line up with the
    // function name emitted by emitClassDecl's primitive-impl path.
    auto classNameForDisplay = [&](const Expr& e, llvm::Value* v) -> std::string {
        std::string nm = inferredClassName(e);
        if (!nm.empty() && findClassMethod(nm, "display")) return nm;
        // Fallback: inspect the LLVM value's type.  Only do this for the
        // primitives whose LLVM shape uniquely identifies the source type
        // (so we don't mis-route e.g. a u32 expr through `i32.display`).
        if (v) {
            auto* vt = v->getType();
            if (vt == strTyShared) {
                if (findClassMethod("string", "display")) return "string";
            } else if (vt->isIntegerTy(1) || vt->isIntegerTy(8)) {
                // bool's value repr is i8 at most use-sites; only i1 in
                // limited pass-through contexts.  Skip for unsigned so u8
                // doesn't mis-route through `bool.display`.
                if (!isUnsignedExpr(e) && findClassMethod("bool", "display"))
                    return "bool";
            } else if (vt->isIntegerTy(32)) {
                // i32/u32/char all share LLVM i32; prefer i32 as the most
                // common case.  Unsigned variant remains on the %u fallback.
                if (!isUnsignedExpr(e) && findClassMethod("i32", "display"))
                    return "i32";
            } else if (vt->isIntegerTy(64)) {
                if (!isUnsignedExpr(e) && findClassMethod("i64", "display"))
                    return "i64";
            } else if (vt->isFloatTy()) {
                if (findClassMethod("f32", "display")) return "f32";
            } else if (vt->isDoubleTy()) {
                if (findClassMethod("f64", "display")) return "f64";
            }
        }
        return {};
    };

    for (auto& part : siE->parts) {
        if (part.isExpr && part.expr) {
            auto* val = emitExpr(*part.expr);
            if (val) {
                // Preserve the string-identity short-circuit: `${s}` where s
                // is already a string skips the `string.display()` round-trip
                // (which is the identity function) for efficiency.
                if (val->getType() == strTyShared) {
                    fmtStr += "%.*s";
                    auto* strTy2 = strTyShared;
                    auto* tmpA = createEntryBlockAlloca(fn, strTy2, "si.str");
                    builder_->CreateStore(val, tmpA);
                    auto* sLen = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_),
                        builder_->CreateStructGEP(strTy2, tmpA, 1));
                    fmtArgs.push_back(builder_->CreateTrunc(sLen, llvm::Type::getInt32Ty(*context_)));
                    val = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_),
                        builder_->CreateStructGEP(strTy2, tmpA, 0));
                    fmtArgs.push_back(val);
                    continue;
                }
                // Try trait-dispatched Display call.
                std::string dispCls = classNameForDisplay(*part.expr, val);
                if (!dispCls.empty()) {
                    llvm::Function* dispFn = findClassMethod(dispCls, "display");
                    if (dispFn && dispFn->getFunctionType()->getNumParams() >= 1) {
                        auto* param0Ty = dispFn->getFunctionType()->getParamType(0);
                        llvm::Value* selfArg = nullptr;
                        if (param0Ty->isPointerTy()) {
                            // User-class Display: self by pointer.  Spill val
                            // to an entry alloca so we can pass its address.
                            auto* tmpSelf = createEntryBlockAlloca(fn, val->getType(), "disp.self");
                            builder_->CreateStore(val, tmpSelf);
                            selfArg = tmpSelf;
                        } else if (param0Ty == val->getType()) {
                            selfArg = val;
                        } else if (param0Ty->isIntegerTy() && val->getType()->isIntegerTy()) {
                            // Width fixup — widen (zext/sext) when the param
                            // is larger, narrow (trunc) when smaller.  The
                            // narrow case is needed for bool whose value
                            // representation at use-sites is i8 while
                            // `impl Display for bool`'s self is i1.
                            unsigned pbw = param0Ty->getIntegerBitWidth();
                            unsigned vbw = val->getType()->getIntegerBitWidth();
                            if (pbw > vbw) {
                                selfArg = isUnsignedExpr(*part.expr)
                                    ? builder_->CreateZExt(val, param0Ty)
                                    : builder_->CreateSExt(val, param0Ty);
                            } else {
                                selfArg = builder_->CreateTrunc(val, param0Ty);
                            }
                        }
                        if (selfArg) {
                            auto* retStr = builder_->CreateCall(dispFn, {selfArg}, "disp.ret");
                            // Result is a `string` struct { ptr, i64, ... }.
                            fmtStr += "%.*s";
                            auto* strTy2 = strTyShared;
                            auto* tmpA = createEntryBlockAlloca(fn, strTy2, "si.disp.str");
                            builder_->CreateStore(retStr, tmpA);
                            auto* sLen = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_),
                                builder_->CreateStructGEP(strTy2, tmpA, 1));
                            fmtArgs.push_back(builder_->CreateTrunc(sLen, llvm::Type::getInt32Ty(*context_)));
                            auto* sPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_),
                                builder_->CreateStructGEP(strTy2, tmpA, 0));
                            fmtArgs.push_back(sPtr);
                            continue;
                        }
                    }
                }
                // ── Fallback: hardcoded per-LLVM-type switch ────────────────
                // Retained for types without a Display impl (rare — every
                // primitive now has one via std/fmt.vyx, but e.g. un-imported
                // compilations or unsigned integers still land here).
                if (val->getType()->isIntegerTy()) {
                    bool unsignedPart = isUnsignedExpr(*part.expr);
                    if (val->getType()->getIntegerBitWidth() > 32) {
                        fmtStr += unsignedPart ? "%llu" : "%lld";
                    } else {
                        fmtStr += unsignedPart ? "%u" : "%d";
                        if (val->getType()->getIntegerBitWidth() < 32) {
                            val = unsignedPart
                                ? builder_->CreateZExt(val, llvm::Type::getInt32Ty(*context_))
                                : builder_->CreateSExt(val, llvm::Type::getInt32Ty(*context_));
                        }
                    }
                } else if (val->getType()->isFloatingPointTy()) {
                    fmtStr += "%f";
                    if (val->getType()->isFloatTy()) {
                        val = builder_->CreateFPExt(val, llvm::Type::getDoubleTy(*context_));
                    }
                } else if (val->getType()->isPointerTy()) {
                    fmtStr += "%s";
                } else if (val->getType()->isStructTy()) {
                    fmtStr += "%s";
                    auto* tmp = createEntryBlockAlloca(fn, val->getType(), "interp.struct.tmp");
                    builder_->CreateStore(val, tmp);
                    auto* stTy = llvm::cast<llvm::StructType>(val->getType());
                    std::string debugName = stTy->getName().str() + ".toString";
                    auto fnIt = functions_.find(debugName);
                    if (fnIt != functions_.end()) {
                        auto* strVal = builder_->CreateCall(fnIt->second, {tmp}, "interp.struct.str");
                        val = extractStringPtr(strVal);
                    } else {
                        val = getOrCreateString("<struct>");
                    }
                }
                fmtArgs.push_back(val);
            }
        } else {
            // Escape `%` in literal parts so printf/snprintf don't interpret
            // them as format specifiers. Without this, `"a % b = ${a % b}"`
            // sent "a % b = %lld" to snprintf, which swallowed " b" as a
            // (malformed) specifier and dropped the arg.
            for (char c : part.text) {
                if (c == '%') fmtStr += "%%";
                else fmtStr += c;
            }
        }
    }

    auto* i64Ty = llvm::Type::getInt64Ty(*context_);
    auto* ptrTy = llvm::PointerType::getUnqual(*context_);
    auto* strTy = getOrCreateStringType();

    auto* snprintfFn = module_->getFunction("snprintf");
    if (!snprintfFn) {
        auto* sty = llvm::FunctionType::get(llvm::Type::getInt32Ty(*context_), {ptrTy, i64Ty, ptrTy}, true);
        snprintfFn = llvm::Function::Create(sty, llvm::Function::ExternalLinkage, "snprintf", *module_);
    }
    auto* strlenFn = module_->getFunction("strlen");
    if (!strlenFn) {
        auto* slty = llvm::FunctionType::get(i64Ty, {ptrTy}, false);
        strlenFn = llvm::Function::Create(slty, llvm::Function::ExternalLinkage, "strlen", *module_);
    }
    auto* mallocFn = module_->getFunction("malloc");
    if (!mallocFn) {
        auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
        mallocFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
    }

    auto* fmtVal = getOrCreateString(fmtStr);

    auto* nullBuf = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
    auto* zero64 = llvm::ConstantInt::get(i64Ty, 0);
    std::vector<llvm::Value*> sizeArgs = {nullBuf, zero64, fmtVal};
    sizeArgs.insert(sizeArgs.end(), fmtArgs.begin(), fmtArgs.end());
    auto* needed = builder_->CreateCall(snprintfFn, sizeArgs, "interp.needed");
    auto* needed64 = builder_->CreateSExt(needed, i64Ty, "interp.needed64");
    auto* bufSize = builder_->CreateAdd(needed64, llvm::ConstantInt::get(i64Ty, 1), "interp.bufsz");
    auto* buf = builder_->CreateCall(mallocFn, {bufSize}, "interp.buf");

    std::vector<llvm::Value*> snprintfArgs = {buf, bufSize, fmtVal};
    snprintfArgs.insert(snprintfArgs.end(), fmtArgs.begin(), fmtArgs.end());
    builder_->CreateCall(snprintfFn, snprintfArgs);

    auto* resAlloca = createEntryBlockAlloca(fn, strTy, "interp.str");
    builder_->CreateStore(buf, builder_->CreateStructGEP(strTy, resAlloca, 0));
    builder_->CreateStore(needed64, builder_->CreateStructGEP(strTy, resAlloca, 1));
    builder_->CreateStore(bufSize, builder_->CreateStructGEP(strTy, resAlloca, 2));
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 1),
        builder_->CreateStructGEP(strTy, resAlloca, 3));
    return builder_->CreateLoad(strTy, resAlloca, "interp.val");
}

llvm::Value* CodeGen::emitIndex(const Expr& expr) {
    auto* idxE = expr.as<const IndexExpr>();
    if (!idxE->object || !idxE->indexExpr) {
        diag_.error(idxE->location, "index expression: missing object or index");
        return nullptr;
    }

    if (idxE->indexExpr->kind == ExprKind::BinaryOp) {
        auto* rangeOp = idxE->indexExpr->as<const BinaryOpExpr>();
        if (rangeOp->op == BinaryOp::RangeOp) {
        auto* objVal = emitExpr(*idxE->object);
        if (objVal && objVal->getType() == getOrCreateStringType()) {
            auto* strTy = getOrCreateStringType();
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* startVal = rangeOp->lhs ? emitExpr(*rangeOp->lhs) : nullptr;
            auto* endVal = rangeOp->rhs ? emitExpr(*rangeOp->rhs) : nullptr;
            if (startVal && endVal) {
                auto* start64 = castToType(startVal, i64Ty);
                auto* end64 = castToType(endVal, i64Ty);

                auto* srcAlloca = createEntryBlockAlloca(fn, strTy, "slice.src");
                builder_->CreateStore(objVal, srcAlloca);
                auto* strLen = builder_->CreateLoad(i64Ty,
                    builder_->CreateStructGEP(strTy, srcAlloca, 1), "slice.strlen");
                auto* oob = builder_->CreateOr(
                    builder_->CreateICmpSLT(start64, llvm::ConstantInt::get(i64Ty, 0)),
                    builder_->CreateOr(
                        builder_->CreateICmpSGT(end64, strLen),
                        builder_->CreateICmpSGT(start64, end64)), "slice.oob");
                auto* curFn = builder_->GetInsertBlock()->getParent();
                auto* panicBB = llvm::BasicBlock::Create(*context_, "slice.panic", curFn);
                auto* okBB = llvm::BasicBlock::Create(*context_, "slice.ok", curFn);
                builder_->CreateCondBr(oob, panicBB, okBB);
                builder_->SetInsertPoint(panicBB);
                auto* printfFn = getOrCreatePrintf();
                builder_->CreateCall(printfFn, {getOrCreateString("PANIC: string slice out of bounds (start=%lld, end=%lld, len=%lld)\n"), start64, end64, strLen});
                emitPanicCall(getOrCreateString(""));
                builder_->SetInsertPoint(okBB);

                auto* sliceLen = builder_->CreateSub(end64, start64, "slice.len");
                auto* srcPtr = builder_->CreateLoad(ptrTy,
                    builder_->CreateStructGEP(strTy, srcAlloca, 0), "slice.src.ptr");
                auto* sliceSrc = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), srcPtr, start64, "slice.start");

                auto* mallocFn = module_->getFunction("malloc");
                if (!mallocFn) {
                    auto* mty = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
                    mallocFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "malloc", *module_);
                }
                auto* allocSize = builder_->CreateAdd(sliceLen, llvm::ConstantInt::get(i64Ty, 1));
                auto* newBuf = builder_->CreateCall(mallocFn, {allocSize}, "slice.buf");
                auto* memcpyFn = module_->getFunction("memcpy");
                if (!memcpyFn) {
                    auto* mty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, i64Ty}, false);
                    memcpyFn = llvm::Function::Create(mty, llvm::Function::ExternalLinkage, "memcpy", *module_);
                }
                builder_->CreateCall(memcpyFn, {newBuf, sliceSrc, sliceLen});
                auto* nullPos = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), newBuf, sliceLen);
                builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), 0), nullPos);

                auto* resultAlloca = createEntryBlockAlloca(fn, strTy, "slice.result");
                auto* memsetFn2 = module_->getFunction("memset");
                if (memsetFn2) {
                    uint64_t sz = module_->getDataLayout().getTypeAllocSize(strTy);
                    builder_->CreateCall(memsetFn2, {
                        resultAlloca,
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sz)
                    });
                }
                builder_->CreateStore(newBuf, builder_->CreateStructGEP(strTy, resultAlloca, 0));
                builder_->CreateStore(sliceLen, builder_->CreateStructGEP(strTy, resultAlloca, 1));
                builder_->CreateStore(allocSize, builder_->CreateStructGEP(strTy, resultAlloca, 2));
                builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 1),
                    builder_->CreateStructGEP(strTy, resultAlloca, 3));
                return builder_->CreateLoad(strTy, resultAlloca, "slice.val");
            }
        }
        }
    }

    auto* arrAddr = (idxE->object->kind == ExprKind::Identifier)
        ? getVariableAddress(*idxE->object)
        : nullptr;

    // Handle indexing into struct member fields (e.g. s.data[0] where data is [i32; 4])
    if (!arrAddr && idxE->object->kind == ExprKind::MemberAccess) {
        arrAddr = getMemberAddress(*idxE->object);
    }

    if (arrAddr) {
        auto* ptrValTy = getValuePtrType(arrAddr);
        if (ptrValTy && ptrValTy->isPointerTy()) {
            auto* idxVal2 = emitExpr(*idxE->indexExpr);
            if (!idxVal2) {
                diag_.error(expr.location, "index: index expression did not produce a value");
                return nullptr;
            }
            auto* ptrVal = builder_->CreateLoad(ptrValTy, arrAddr, "rawptr.val");
            auto* idxExt = idxVal2->getType()->isIntegerTy(64) ? idxVal2
                : builder_->CreateSExt(idxVal2, llvm::Type::getInt64Ty(*context_));
            llvm::Type* elemTy = idxE->object ? inferredPointeeLLVM(*idxE->object) : nullptr;
            if (!elemTy && idxE->object->kind == ExprKind::Identifier) {
                auto ptIt = ptrElemTypes_.find(idxE->object->as<const IdentifierExpr>()->name);
                if (ptIt != ptrElemTypes_.end()) elemTy = ptIt->second;
            }
            if (elemTy) {
                auto* elemPtr = builder_->CreateGEP(elemTy, ptrVal, idxExt, "typed.elem");
                return builder_->CreateLoad(elemTy, elemPtr, "typed.val");
            }
            auto* elemPtr = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), ptrVal, idxExt, "rawptr.elem");
            auto* byte = builder_->CreateLoad(llvm::Type::getInt8Ty(*context_), elemPtr, "rawptr.byte");
            return builder_->CreateZExt(byte, llvm::Type::getInt32Ty(*context_), "rawptr.i32");
        }
    }

    if (!arrAddr) {
        auto* objVal = emitExpr(*idxE->object);
        if (!objVal) {
            diag_.error(expr.location, "index: indexed expression did not produce a value");
            return nullptr;
        }
        auto* objTy = objVal->getType();
        if (auto* stTy = llvm::dyn_cast<llvm::StructType>(objTy)) {
            auto* fn = builder_->GetInsertBlock()->getParent();
            auto* tmpAlloca = createEntryBlockAlloca(fn, stTy, "idx.tmp");
            builder_->CreateStore(objVal, tmpAlloca);
            arrAddr = tmpAlloca;
        } else {
            return objVal;
        }
    }

    auto* idxVal = emitExpr(*idxE->indexExpr);
    if (!idxVal) {
        diag_.error(expr.location, "index: index expression did not produce a value");
        return nullptr;
    }
    auto* arrTy = getValuePtrType(arrAddr);

    if (auto* arrayTy = llvm::dyn_cast<llvm::ArrayType>(arrTy)) {
        auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
        auto* gep = builder_->CreateGEP(arrTy, arrAddr, {zero, idxVal}, "arr.idx");
        return builder_->CreateLoad(arrayTy->getElementType(), gep, "arr.elem.val");
    }

    if (auto* stTy = llvm::dyn_cast<llvm::StructType>(arrTy)) {
        // operator[] overload check FIRST: if operator_index is defined, use it
        // regardless of field count (avoids falling into Vec-layout path for non-Vec structs)
        {
            std::string typeName = stTy->hasName() ? stTy->getName().str() : "";
            if (typeName.empty() && idxE->object && idxE->object->kind == ExprKind::Identifier) {
                typeName = resolveClassName(*idxE->object,
                    idxE->object->as<const IdentifierExpr>()->name);
            }
            if (!typeName.empty()) {
                auto* opFn = findClassMethod(typeName, "operator_index");
                if (opFn) {
                    auto* idxExt = idxVal->getType()->isIntegerTy(64) ? idxVal
                        : builder_->CreateSExt(idxVal, llvm::Type::getInt64Ty(*context_));
                    // Cast idxExt to the expected parameter type
                    if (opFn->arg_size() >= 2) {
                        auto* paramTy = opFn->getFunctionType()->getParamType(1);
                        if (!paramTy->isPointerTy() && idxExt->getType() != paramTy)
                            idxExt = castToType(idxExt, paramTy);
                    }
                    return builder_->CreateCall(opFn, {arrAddr, idxExt}, "idx.result");
                }
            }
        }

        if (stTy->getNumElements() >= 3) {
            auto* ptrTy = llvm::PointerType::getUnqual(*context_);
            auto* i64Ty = llvm::Type::getInt64Ty(*context_);
            auto* dataPtr = builder_->CreateStructGEP(stTy, arrAddr, 0, "vec.idx.data.ptr");
            auto* data = builder_->CreateLoad(ptrTy, dataPtr, "vec.idx.data");
            auto* idxExt = idxVal->getType()->isIntegerTy(64) ? idxVal
                : builder_->CreateSExt(idxVal, i64Ty);
            llvm::Type* idxElemTy = idxE->object ? inferredContainerElemLLVM(*idxE->object) : nullptr;
            if (!idxElemTy && idxE->object->kind == ExprKind::Identifier) {
                auto etIt = containerElemTypes_.find(idxE->object->as<const IdentifierExpr>()->name);
                if (etIt != containerElemTypes_.end()) idxElemTy = etIt->second;
            }
            if (!idxElemTy) idxElemTy = i64Ty;
            if (stTy == getOrCreateStringType()) {
                auto* charPtr = builder_->CreateGEP(llvm::Type::getInt8Ty(*context_), data, idxExt, "str.idx.ptr");
                auto* byte = builder_->CreateLoad(llvm::Type::getInt8Ty(*context_), charPtr, "str.idx.byte");
                return builder_->CreateZExt(byte, i64Ty, "str.idx.char");
            }
            auto* elemPtr = builder_->CreateGEP(idxElemTy, data, idxExt, "vec.idx.elem.ptr");
            return builder_->CreateLoad(idxElemTy, elemPtr, "vec.idx.elem");
        }
    }

    diag_.error(expr.location, "unsupported index operation on this type");
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
}

} // namespace vyx
