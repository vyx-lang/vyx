#pragma once
#include "Type.h"
#include "../Parser/AST.h"
#include <string>
#include <map>
#include <vector>

namespace vyx {

/// Represents a mapping from generic parameter names to concrete types
struct GenericSubstitution {
    std::map<std::string, VyxTypePtr> substitutions;
    
    /// Add a substitution for a generic parameter
    void add(const std::string& paramName, VyxTypePtr concreteType) {
        substitutions[paramName] = concreteType;
    }
    
    /// Check if a type name is a generic parameter
    bool isGenericParam(const std::string& typeName) const {
        return substitutions.find(typeName) != substitutions.end();
    }
    
    /// Get the concrete type for a generic parameter
    VyxTypePtr getConcreteType(const std::string& paramName) const {
        auto it = substitutions.find(paramName);
        if (it != substitutions.end()) {
            return it->second;
        }
        return nullptr;
    }
};

/// Template instantiation engine for generic functions
class TemplateResolver {
public:
    TemplateResolver() = default;
    
    /// Substitute generic types in a type annotation with concrete types
    VyxTypePtr substituteType(const TypeAnnotation& type, const GenericSubstitution& subst);
    
    /// Substitute generic types in a function parameter
    ParamDecl substituteParam(const ParamDecl& param, const GenericSubstitution& subst);
    
    /// Create a mangled name for a template instantiation
    static std::string mangleTemplateName(const std::string& baseName, const std::vector<VyxTypePtr>& argTypes);

    /// Convert an AST TypeAnnotation to a structured VyxType.
    /// Pure structural conversion: no symbol lookup, no substitution. Names
    /// that match well-known builtins (`i32`, `string`, `Vec`, `Dict`, …)
    /// produce the corresponding kinded type; everything else is returned
    /// as either a `Class` (if the name appears to be a user nominal type)
    /// or `Generic` (bare identifier). Callers who want substitution then
    /// apply `::vyx::substituteType(result, env)` from Type.h.
    ///
    /// If `genericParams` is non-null, names appearing in it are treated as
    /// Generic parameters instead of Class — this is how templated decls
    /// resolve their own `T` during signature conversion.
    static VyxTypePtr resolveTypeAnnotation(const TypeAnnotation& ann,
                                            const std::vector<std::string>* genericParams = nullptr);
    
    /// Infer generic parameter types from function call arguments
    static GenericSubstitution inferGenericTypes(
        const std::vector<std::string>& genericParams,
        const std::vector<ParamDecl>& declaredParams,
        const std::vector<ExprPtr>& callArgs,
        const std::vector<VyxTypePtr>& argTypes
    );
    
private:
    /// Recursively substitute types in complex type annotations
    VyxTypePtr substituteComplexType(const TypeAnnotation& type, const GenericSubstitution& subst);
};

} // namespace vyx
