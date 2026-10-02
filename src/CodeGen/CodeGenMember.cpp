#include "CodeGenIncludes.h"

namespace vyx {

llvm::Value* CodeGen::getMemberAddress(const Expr& expr, int depth) {
    if (expr.kind != ExprKind::MemberAccess) return nullptr;
    auto& ma = *expr.as<const MemberAccessExpr>();
    if (!ma.object) return nullptr;
    if (depth > 8) {
        diag_.error(ma.location, "member access path is too deep (max 8 segments)");
        return nullptr;
    }

    if (ma.object->kind == ExprKind::SelfExpr && !currentClassName_.empty()) {
        auto selfIt = namedValues_.find("self");
        auto stIt = structTypes_.find(currentClassName_);
        if (selfIt != namedValues_.end() && stIt != structTypes_.end()) {
            auto* selfPtr = builder_->CreateLoad(getValuePtrType(selfIt->second), selfIt->second, "self.ptr");
            auto* structTy = stIt->second;
            auto fnIt = structFieldNames_.find(currentClassName_);
            if (fnIt != structFieldNames_.end()) {
                auto& fieldNames = fnIt->second;
                for (size_t i = 0; i < fieldNames.size(); ++i) {
                    if (fieldNames[i] == ma.member) {
                        return builder_->CreateStructGEP(structTy, selfPtr, i, "self." + ma.member);
                    }
                }
            }
        }
    }

    // Static field address: ClassName.fieldName — resolve to module-scope global
    if (ma.object->kind == ExprKind::Identifier) {
        std::string objName = ma.object->as<const IdentifierExpr>()->name;
        if (!namedValues_.count(objName)) {
            std::string globalName = objName + "." + ma.member;
            auto* gv = module_->getGlobalVariable(globalName, true);
            if (gv) return gv;
        }
    }

    if (ma.object->kind == ExprKind::Identifier) {
        auto it = namedValues_.find(ma.object->as<const IdentifierExpr>()->name);
        if (it != namedValues_.end()) {
            auto* allocaTy = getValuePtrType(it->second);
            if (auto* structTy = llvm::dyn_cast<llvm::StructType>(allocaTy)) {
                std::string typeName = structTy->getName().str();
                auto fnIt = structFieldNames_.find(typeName);
                if (fnIt != structFieldNames_.end()) {
                    auto& fieldNames = fnIt->second;
                    for (size_t i = 0; i < fieldNames.size(); ++i) {
                        if (fieldNames[i] == ma.member) {
                            return builder_->CreateStructGEP(structTy, it->second, i, ma.member);
                        }
                    }
                }
                if (typeName == "__RefCounted") {
                    std::string innerTypeName = resolveRefInner(*ma.object,
                        ma.object->as<const IdentifierExpr>()->name);
                    if (innerTypeName.empty()) {
                        int matchCount = 0;
                        std::string candidateName;
                        for (auto& [stName, stTy2] : structTypes_) {
                            // Skip compiler-internal layout aliases and well-known generic
                            // container/ADT families.  Option / Result go through the
                            // lang-item helper so stdlib renames remain filtered.
                            if (stName.starts_with("__") || stName.starts_with("Box<") ||
                                stName.starts_with("Vec<") || stName.starts_with("Dict<") ||
                                isOptionOrResultSlotName(stName)) continue;
                            auto sfIt = structFieldNames_.find(stName);
                            if (sfIt == structFieldNames_.end()) continue;
                            if (sfIt->second.size() >= 2 && sfIt->second[0] == "__tag" &&
                                sfIt->second[1] == "__data") continue;
                            for (auto& fn2 : sfIt->second) {
                                if (fn2 == ma.member) {
                                    candidateName = stName;
                                    matchCount++;
                                    break;
                                }
                            }
                        }
                        if (matchCount >= 1) innerTypeName = candidateName;
                    }
                    if (!innerTypeName.empty()) {
                        auto innerStIt = structTypes_.find(innerTypeName);
                        if (innerStIt != structTypes_.end()) {
                            auto* innerTy = innerStIt->second;
                            auto innerFnIt = structFieldNames_.find(innerTypeName);
                            if (innerFnIt != structFieldNames_.end()) {
                                auto& innerFields = innerFnIt->second;
                                for (size_t i = 0; i < innerFields.size(); ++i) {
                                    if (innerFields[i] == ma.member) {
                                        auto* dataPtrGEP = builder_->CreateStructGEP(structTy, it->second, 0, "ref.data.ptr");
                                        auto* rawPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), dataPtrGEP, "ref.raw.ptr");
                                        return builder_->CreateStructGEP(innerTy, rawPtr, i, "ref." + ma.member);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (allocaTy && allocaTy->isPointerTy() && !allocaTy->isStructTy()) {
                std::string refInner = resolveRefInner(*ma.object,
                    ma.object->as<const IdentifierExpr>()->name);
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
                                for (size_t i = 0; i < innerFields.size(); ++i) {
                                    if (innerFields[i] == ma.member) {
                                        auto* rawPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), it->second, "box.raw.ptr");
                                        return builder_->CreateStructGEP(innerTy, rawPtr, i, "box." + ma.member);
                                    }
                                }
                            } else if (unit_) {
                                auto* rawPtr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), it->second, "box.raw.ptr");
                                auto* adtAlloca = builder_->CreateLoad(innerTy, rawPtr, "box.adt");
                                auto* fn2 = builder_->GetInsertBlock()->getParent();
                                auto* tmpAlloca = createEntryBlockAlloca(fn2, innerTy, "adt.tmp");
                                builder_->CreateStore(adtAlloca, tmpAlloca);
                                auto* dataPtr = builder_->CreateStructGEP(innerTy, tmpAlloca, 1, "adt.data.ptr");
                                std::string adtBaseName = refInner;
                                if (auto ltPos = adtBaseName.find('<'); ltPos != std::string::npos)
                                    adtBaseName = adtBaseName.substr(0, ltPos);
                                for (auto& d : unit_->declarations) {
                                    if (!d || d->kind != DeclKind::ErrorDef || d->name != adtBaseName) continue;
                                    auto& variants = d->as<ErrorDefDecl>()->variants;
                                    auto& variantTypes = d->as<ErrorDefDecl>()->variantTypes;
                                    for (size_t vi = 0; vi < variants.size(); ++vi) {
                                        if (vi >= variantTypes.size()) continue;
                                        uint64_t offset = 0;
                                        for (size_t fi = 0; fi < variantTypes[vi].size(); ++fi) {
                                            auto* fieldTy = toLLVMType(*variantTypes[vi][fi]);
                                            auto varStIt = structTypes_.find(mangleTypeAnnotation(*variantTypes[vi][fi]));
                                            if (varStIt != structTypes_.end()) {
                                                auto varFnIt = structFieldNames_.find(varStIt->second->getName().str());
                                                if (varFnIt != structFieldNames_.end()) {
                                                    for (size_t si = 0; si < varFnIt->second.size(); ++si) {
                                                        if (varFnIt->second[si] == ma.member) {
                                                            auto* fieldAddr = builder_->CreateGEP(
                                                                llvm::Type::getInt8Ty(*context_), dataPtr,
                                                                llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), offset),
                                                                "adt.variant.ptr");
                                                            return builder_->CreateStructGEP(varStIt->second, fieldAddr, si, "adt." + ma.member);
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
    }

    if (ma.object->kind == ExprKind::MemberAccess) {
        auto* parentAddr = getMemberAddress(*ma.object, depth + 1);
        if (parentAddr) {
            auto* parentTy = getValuePtrType(parentAddr);
            if (auto* structTy = llvm::dyn_cast_or_null<llvm::StructType>(parentTy)) {
                std::string typeName = structTy->hasName() ? structTy->getName().str() : "";
                auto fnIt = structFieldNames_.find(typeName);
                if (fnIt != structFieldNames_.end()) {
                    auto& fieldNames = fnIt->second;
                    for (size_t i = 0; i < fieldNames.size(); ++i) {
                        if (fieldNames[i] == ma.member) {
                            return builder_->CreateStructGEP(structTy, parentAddr, i, ma.member);
                        }
                    }
                }
            }
            if (parentTy && parentTy->isPointerTy()) {
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(parentAddr)) {
                    auto* srcTy = gep->getSourceElementType();
                    if (auto* srcStructTy = llvm::dyn_cast<llvm::StructType>(srcTy)) {
                        std::string srcName = srcStructTy->hasName() ? srcStructTy->getName().str() : "";
                        auto& parentMa = *ma.object->as<const MemberAccessExpr>();
                        auto fnIt = structFieldNames_.find(srcName);
                        auto ftIt = structFieldTypeNames_.find(srcName);
                        if (fnIt != structFieldNames_.end() && ftIt != structFieldTypeNames_.end()) {
                            for (size_t pi = 0; pi < fnIt->second.size() && pi < ftIt->second.size(); ++pi) {
                                if (fnIt->second[pi] == parentMa.member) {
                                    std::string targetName = ftIt->second[pi];
                                    auto stIt = structTypes_.find(targetName);
                                    if (stIt != structTypes_.end()) {
                                        auto* loadedPtr = builder_->CreateLoad(
                                            llvm::PointerType::getUnqual(*context_), parentAddr, "chain.load");
                                        auto fnIt2 = structFieldNames_.find(targetName);
                                        if (fnIt2 != structFieldNames_.end()) {
                                            for (size_t i = 0; i < fnIt2->second.size(); ++i) {
                                                if (fnIt2->second[i] == ma.member) {
                                                    return builder_->CreateStructGEP(
                                                        stIt->second, loadedPtr, i, ma.member);
                                                }
                                            }
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
    }

    return nullptr;
}

} // namespace vyx
