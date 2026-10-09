# HIR builder source map

The builder converts resolved AST declarations into `HirUnit` records and
materializes concrete function bodies for native codegen.
[`hir_builder.vyx`](hir_builder.vyx) keeps `HirUnitBuilder` storage,
construction, final release, and the public native-handle API. The files in
this directory share its logical module, `bootstrap.hir_builder`.

```text
builder/
├── hir_builder.vyx   state, construction and native-handle API
├── declarations/    declaration registration, attributes and storage
├── types/           canonical type resolution, traits and layout
├── generics/        bindings, inference and closed instances
├── resolution/      lookup, calls, receivers and deferred typing
├── lower/           expressions, statements, patterns and closures
├── functions/       roots and body requests
├── lifetime/        ownership facts and body record release
├── metadata/        Reflection and DCI producer metadata
└── support/         indexes, scope storage, text helpers and profiling
```

## Where to change behavior

| Responsibility | Files / entry points |
|---|---|
| Register declarations, fields and function attributes | [declarations.vyx](declarations/declarations.vyx): `register_decl`, `register_decl_chain`; [attributes.vyx](declarations/attributes.vyx) |
| Defining modules and value lookup | [declaration_scopes.vyx](declarations/declaration_scopes.vyx): `preindex_decl_modules`, `raw_value_decl_for_name` |
| Global and associated storage | [storage.vyx](declarations/storage.vyx): `apply_storage_item_flags`, `build_value_item_init_for_env` |
| Canonical type IDs and type syntax | [type_resolution.vyx](types/type_resolution.vyx): `type_id_from_ref`, `type_ref_from_text`; [generic_text.vyx](generics/generic_text.vyx) |
| Generic inference and substitution | [generic_inference.vyx](generics/generic_inference.vyx): `infer_call_bindings`; [generic_environment.vyx](generics/generic_environment.vyx): `resolve_type_text_in_current_env` |
| Check whether an instance is closed | [template_analysis.vyx](generics/template_analysis.vyx): `function_has_open_template_types`, `decl_body_types_close_in_current_env` |
| Materialize class and function instances | [class_instances.vyx](generics/class_instances.vyx): `ensure_class_instance`; [function_instances.vyx](generics/function_instances.vyx): `ensure_function_instance`; [method_instances.vyx](generics/method_instances.vyx) |
| Find classes and methods | [class_lookup.vyx](resolution/class_lookup.vyx), [method_lookup.vyx](resolution/method_lookup.vyx) |
| Select overloads and free calls | [overload_resolution.vyx](resolution/overload_resolution.vyx), [free_calls.vyx](resolution/free_calls.vyx) |
| Resolve calls and receivers | [calls.vyx](resolution/calls.vyx): `resolve_call_expr_after_children`; [method_calls.vyx](resolution/method_calls.vyx): `concrete_method_target_for_call_expr`; [call_arguments.vyx](resolution/call_arguments.vyx) |
| Member access and expected types | [member_resolution.vyx](resolution/member_resolution.vyx): `resolve_member_expr_after_children`; [expected_types.vyx](resolution/expected_types.vyx): `apply_expected_expr_type` |
| Build expressions, statements and patterns | [expressions.vyx](lower/expressions.vyx): `build_expr`; [statements.vyx](lower/statements.vyx): `build_stmt`; [patterns.vyx](lower/patterns.vyx) |
| Expression targets, primitives and operators | [expression_targets.vyx](resolution/expression_targets.vyx), [builtins.vyx](lower/builtins.vyx), [operators.vyx](lower/operators.vyx) |
| Parameters, construction, closures and packs | [parameters.vyx](lower/parameters.vyx), [constructors.vyx](lower/constructors.vyx), [closures.vyx](lower/closures.vyx), [packs.vyx](generics/packs.vyx) |
| Borrow, move and drop metadata | [ownership.vyx](lifetime/ownership.vyx): `set_local_drop_for_type`; [string_ownership.vyx](lifetime/string_ownership.vyx) |
| Trait requirements and dynamic payloads | [traits.vyx](types/traits.vyx): `type_text_satisfies_dyn_trait`, `materialize_dyn_trait_impl_methods` |
| Reflection and record layout | [reflection.vyx](metadata/reflection.vyx): `build_reflection_manifest`; [record_layout.vyx](types/record_layout.vyx): `class_storage_size` |
| DCI producer metadata and overrides | [dci.vyx](metadata/dci.vyx): `build_dci_stub_specs`, `finalize_foreign_cpp_metadata`; lifecycle binding delegates to `bootstrap.dci_binder` |
| Export and language roots | [roots.vyx](functions/roots.vyx): `materialize_function_roots`, `materialize_marked_codegen_roots` |
| Function-body requests and construction | [function_bodies.vyx](functions/function_bodies.vyx): `materialize_function_body_for_codegen`, `build_function_body_core_for_function` |
| Deferred member, call and local typing | [deferred_resolution.vyx](resolution/deferred_resolution.vyx): `resolve_deferred_member_exprs_once`, `finalize_codegen_hir` |
| Release generated body records | [body_storage.vyx](lifetime/body_storage.vyx): `discard_function_body_records_after_codegen` |
| Allocation and phase measurements | [profiling.vyx](support/profiling.vyx); shared syntax operations in [text_helpers.vyx](support/text_helpers.vyx) |

## Index storage and lifetimes

| Files | Key / lifetime |
|---|---|
| [pointer_maps.vyx](support/pointer_maps.vyx), [instance_maps.vyx](support/instance_maps.vyx) | AST pointer identity, concrete instance keys and owner relations; owned by the builder |
| [name_maps.vyx](support/name_maps.vyx), [pair_maps.vyx](support/pair_maps.vyx), [id_maps.vyx](support/id_maps.vyx) | Canonical names, owner/member pairs and numeric IDs; owned by the builder |
| [local_scope.vyx](support/local_scope.vyx), [generic_bindings.vyx](generics/generic_bindings.vyx) | Scope chains and generic/pack bindings; restored to the saved head when leaving a scope |

Keep raw AST identity separate from concrete instance identity. A template
declaration can have multiple closed instances; an item from the open template
cannot substitute for a failed closed-instance lookup.

## Native codegen flow

1. `hir_build_codegen_builder_handle_with_export_roots` creates the builder,
   indexes declaring modules and registers declarations.
2. Root selection and reachable-body requests retain concrete signatures.
3. `hir_codegen_builder_handle_materialize_function_body` prepares one requested
   body. Expression construction and deferred resolution complete its HIR facts.
4. The lowering pipeline binds DCI lifecycle facts, resolves ownership and
   verifies HIR before building MIR.
5. `hir_codegen_builder_handle_finish_function_codegen` discards the generated
   body/local ranges and marks the function done. If there is no body to discard,
   it invalidates the materialization resolution caches instead.
6. `hir_codegen_builder_handle_release_codegen_state` releases builder indexes;
   the handle is then freed separately.

IDs in body ranges are not persistent references after discard. Preserve the
checks for the latest owned range and the retained parameter/capture locals.
Do not replace this flow with eager construction of every function body.

## Build boundary

`Vyx.toml` explicitly lists the facade followed by these implementation files
in the compiler target. `auto_sources = false` requires a new file to be added
to that list. The project builder compiles them as one logical module; other
modules consume its interface. The private builder and support classes remain
private. Moving a method does not change its visibility or owner type.

This organization provides a source boundary for each operation. The files
still share one builder and its caches; it does not create independent HIR
sessions or allow concurrent mutation of the builder.

Use the [module contract gate](../../../../probes/gates/compiler-modules/README.md)
for multi-file interfaces, exports and incremental rebuilds. HIR behavior is
covered by the [generic interface](../../../../probes/gates/generic_interfaces/README.md),
[cross-module](../../../../probes/gates/cross-module/README.md),
[match](../../../../probes/gates/match-expression/README.md) and
[compiler usability](../../../../probes/gates/compiler-usability/README.md) gates.
Build the tested SDK compiler from this source tree before running them.
