#pragma once
#include "CodeGen.h"

#pragma warning(push, 0)
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/Transforms/Utils.h>
#pragma warning(pop)

#include <cctype>
#include <format>
#include <iostream>

namespace vyx {
inline const std::vector<TypePtr>& getTypeSubTypes(const TypeAnnotation& ann) {
    static const std::vector<TypePtr> empty;
    switch (ann.kind) {
        case TypeAnnotationKind::Generic: return static_cast<const GenericType&>(ann).typeArgs;
        case TypeAnnotationKind::Tuple:   return static_cast<const TupleType&>(ann).elements;
        case TypeAnnotationKind::Function:return static_cast<const FunctionType&>(ann).paramTypes;
        case TypeAnnotationKind::Union:   return static_cast<const UnionType&>(ann).members;
        default: return empty;
    }
}
} // namespace vyx
