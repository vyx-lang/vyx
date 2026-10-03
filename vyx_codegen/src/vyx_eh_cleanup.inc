// Private LLVM emission for versioned shared C++ unwind. Each frame owns a
// doubly linked stack of live cleanup nodes. Nodes are entry allocas, reused
// only after their previous object was released. No process/runtime registry.
static llvm::PointerType* vyx_eh_ptr(VyxRtModule* m) {
    return llvm::PointerType::get(*m->ctx, 0);
}
static llvm::Constant* vyx_eh_null(VyxRtModule* m) {
    return llvm::ConstantPointerNull::get(vyx_eh_ptr(m));
}
static llvm::StructType* vyx_eh_node_type(VyxRtModule* m) {
    auto* ty = llvm::StructType::getTypeByName(*m->ctx, "vyx.eh.cleanup.node");
    if (!ty) {
        ty = llvm::StructType::create(*m->ctx, "vyx.eh.cleanup.node");
        auto* p = vyx_eh_ptr(m);
        ty->setBody({p, p, p, p, p, llvm::Type::getInt1Ty(*m->ctx)});
    }
    return ty;
}
static llvm::Value* vyx_eh_field(VyxRtModule* m, llvm::IRBuilder<>& b,
                                llvm::Value* node, unsigned field) {
    return b.CreateStructGEP(vyx_eh_node_type(m), node, field);
}
static VyxEhFunction& vyx_eh_state(VyxRtModule* m, llvm::Function* fn) {
    auto& state = m->ehFunctions[fn];
    if (!state.head) {
        auto& entry = fn->getEntryBlock();
        llvm::IRBuilder<> b(&entry, entry.getFirstInsertionPt());
        state.head = b.CreateAlloca(vyx_eh_ptr(m), nullptr, "eh.head");
        b.CreateStore(vyx_eh_null(m), state.head);
    }
    return state;
}
static llvm::FunctionCallee vyx_eh_abort(VyxRtModule* m) {
    auto f = m->mod->getOrInsertFunction("abort",
        llvm::FunctionType::get(llvm::Type::getVoidTy(*m->ctx), false));
    if (auto* fn = llvm::dyn_cast<llvm::Function>(f.getCallee())) {
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        fn->addFnAttr(llvm::Attribute::NoReturn);
    }
    return f;
}
static llvm::Function* vyx_eh_find_function(VyxRtModule* m) {
    if (auto* fn = m->mod->getFunction("__vyx_eh_find")) return fn;
    auto* p = vyx_eh_ptr(m);
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(p, {p, p}, false),
        llvm::GlobalValue::InternalLinkage, "__vyx_eh_find", *m->mod);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    auto* entry = llvm::BasicBlock::Create(*m->ctx, "entry", fn);
    auto* loop = llvm::BasicBlock::Create(*m->ctx, "loop", fn);
    auto* check = llvm::BasicBlock::Create(*m->ctx, "check", fn);
    auto* next = llvm::BasicBlock::Create(*m->ctx, "next", fn);
    auto* found = llvm::BasicBlock::Create(*m->ctx, "found", fn);
    auto* done = llvm::BasicBlock::Create(*m->ctx, "done", fn);
    llvm::IRBuilder<> b(entry);
    auto* first = b.CreateLoad(p, fn->getArg(0));
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    auto* node = b.CreatePHI(p, 2);
    node->addIncoming(first, entry);
    b.CreateCondBr(b.CreateICmpEQ(node, vyx_eh_null(m)), done, check);
    b.SetInsertPoint(check);
    auto* object = b.CreateLoad(p, vyx_eh_field(m, b, node, 2));
    b.CreateCondBr(b.CreateICmpEQ(object, fn->getArg(1)), found, next);
    b.SetInsertPoint(next);
    auto* following = b.CreateLoad(p, vyx_eh_field(m, b, node, 1));
    node->addIncoming(following, next);
    b.CreateBr(loop);
    b.SetInsertPoint(found); b.CreateRet(node);
    b.SetInsertPoint(done); b.CreateRet(vyx_eh_null(m));
    return fn;
}
static llvm::Function* vyx_eh_unlink_function(VyxRtModule* m) {
    if (auto* fn = m->mod->getFunction("__vyx_eh_unlink")) return fn;
    auto* p = vyx_eh_ptr(m);
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(
        llvm::Type::getVoidTy(*m->ctx), {p, p}, false),
        llvm::GlobalValue::InternalLinkage, "__vyx_eh_unlink", *m->mod);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    auto* entry = llvm::BasicBlock::Create(*m->ctx, "entry", fn);
    auto* first = llvm::BasicBlock::Create(*m->ctx, "first", fn);
    auto* middle = llvm::BasicBlock::Create(*m->ctx, "middle", fn);
    auto* tail = llvm::BasicBlock::Create(*m->ctx, "tail", fn);
    auto* fixNext = llvm::BasicBlock::Create(*m->ctx, "fix.next", fn);
    auto* done = llvm::BasicBlock::Create(*m->ctx, "done", fn);
    llvm::IRBuilder<> b(entry);
    auto* node = fn->getArg(1);
    auto* prev = b.CreateLoad(p, vyx_eh_field(m, b, node, 0));
    auto* next = b.CreateLoad(p, vyx_eh_field(m, b, node, 1));
    b.CreateCondBr(b.CreateICmpEQ(prev, vyx_eh_null(m)), first, middle);
    b.SetInsertPoint(first); b.CreateStore(next, fn->getArg(0)); b.CreateBr(tail);
    b.SetInsertPoint(middle); b.CreateStore(next, vyx_eh_field(m, b, prev, 1)); b.CreateBr(tail);
    b.SetInsertPoint(tail);
    b.CreateCondBr(b.CreateICmpEQ(next, vyx_eh_null(m)), done, fixNext);
    b.SetInsertPoint(fixNext); b.CreateStore(prev, vyx_eh_field(m, b, next, 0)); b.CreateBr(done);
    b.SetInsertPoint(done);
    b.CreateStore(b.getFalse(), vyx_eh_field(m, b, node, 5));
    b.CreateRetVoid();
    return fn;
}
static llvm::Function* vyx_eh_action(VyxRtModule* m, llvm::Function* dtor,
                                    llvm::Function* release) {
    for (const auto& item : m->ehActions)
        if (item.destructor == dtor && item.release == release) return item.thunk;
    for (auto* callee : {dtor, release}) {
        if (!callee) continue;
        auto* ty = callee->getFunctionType();
        if (ty->getNumParams() != 1 || !ty->getParamType(0)->isPointerTy()
            || !ty->getReturnType()->isVoidTy() || ty->isVarArg()) {
            m->lastError = "shared unwind cleanup requires void(pointer) complete-object operations";
            return nullptr;
        }
    }
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(
        llvm::Type::getVoidTy(*m->ctx), {vyx_eh_ptr(m)}, false),
        llvm::GlobalValue::InternalLinkage,
        "__vyx_eh_action." + std::to_string(m->ehActions.size()), *m->mod);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    m->ehActions.push_back({dtor, release, fn});
    auto* entry = llvm::BasicBlock::Create(*m->ctx, "entry", fn);
    llvm::IRBuilder<> b(entry);
    if (dtor && !dtor->doesNotThrow()) {
        const bool msvc = vyx_rt_module_uses_msvc_eh(m);
        auto personality = m->mod->getOrInsertFunction(
            msvc ? "__CxxFrameHandler3" : "__gxx_personality_v0",
            llvm::FunctionType::get(b.getInt32Ty(), true));
        fn->setPersonalityFn(llvm::cast<llvm::Constant>(personality.getCallee()));
        auto* cont = llvm::BasicBlock::Create(*m->ctx, "destroyed", fn);
        auto* terminate = llvm::BasicBlock::Create(*m->ctx, "terminate", fn);
        auto* call = b.CreateInvoke(dtor->getFunctionType(), dtor, cont, terminate, {fn->getArg(0)});
        call->setCallingConv(dtor->getCallingConv());
        b.SetInsertPoint(terminate);
        llvm::Value* token = nullptr;
        if (msvc) {
            auto* cs = b.CreateCatchSwitch(llvm::ConstantTokenNone::get(*m->ctx), nullptr, 1);
            auto* caught = llvm::BasicBlock::Create(*m->ctx, "caught", fn);
            cs->addHandler(caught);
            b.SetInsertPoint(caught);
            token = b.CreateCatchPad(cs, {vyx_eh_null(m), b.getInt32(64), vyx_eh_null(m)});
        } else {
            auto* lp = b.CreateLandingPad(llvm::StructType::get(vyx_eh_ptr(m), b.getInt32Ty()), 1);
            lp->addClause(vyx_eh_null(m));
        }
        llvm::SmallVector<llvm::OperandBundleDef, 1> bundles;
        if (token) bundles.emplace_back("funclet", std::vector<llvm::Value*>{token});
        b.CreateCall(vyx_eh_abort(m), {}, bundles);
        b.CreateUnreachable();
        b.SetInsertPoint(cont);
    } else if (dtor) {
        auto* call = b.CreateCall(dtor->getFunctionType(), dtor, {fn->getArg(0)});
        call->setCallingConv(dtor->getCallingConv());
    }
    if (release) {
        auto* call = b.CreateCall(release->getFunctionType(), release, {fn->getArg(0)});
        call->setCallingConv(release->getCallingConv());
        call->setDoesNotThrow();
    }
    b.CreateRetVoid();
    return fn;
}
static llvm::AllocaInst* vyx_eh_new_node(VyxRtModule* m, llvm::Function* fn) {
    auto& entry = fn->getEntryBlock();
    llvm::IRBuilder<> b(&entry, entry.getFirstInsertionPt());
    auto* ty = vyx_eh_node_type(m);
    auto* node = b.CreateAlloca(ty, nullptr, "eh.node");
    b.CreateStore(llvm::ConstantAggregateZero::get(ty), node);
    return node;
}
static void vyx_eh_push(VyxRtModule* m, llvm::Value* node, llvm::Value* object,
                        llvm::Value* freeAction) {
    auto& b = *m->builder;
    auto* fn = b.GetInsertBlock()->getParent();
    auto* head = vyx_eh_state(m, fn).head;
    auto* invalid = llvm::BasicBlock::Create(*m->ctx, "eh.reused.live.node", fn);
    auto* push = llvm::BasicBlock::Create(*m->ctx, "eh.push", fn);
    auto* link = llvm::BasicBlock::Create(*m->ctx, "eh.link", fn);
    auto* done = llvm::BasicBlock::Create(*m->ctx, "eh.pushed", fn);
    auto* active = b.CreateLoad(b.getInt1Ty(), vyx_eh_field(m, b, node, 5));
    b.CreateCondBr(active, invalid, push);
    b.SetInsertPoint(invalid); b.CreateCall(vyx_eh_abort(m)); b.CreateUnreachable();
    b.SetInsertPoint(push);
    auto* previous = b.CreateLoad(vyx_eh_ptr(m), head);
    b.CreateStore(vyx_eh_null(m), vyx_eh_field(m, b, node, 0));
    b.CreateStore(previous, vyx_eh_field(m, b, node, 1));
    b.CreateStore(object, vyx_eh_field(m, b, node, 2));
    b.CreateStore(freeAction, vyx_eh_field(m, b, node, 3));
    b.CreateStore(freeAction, vyx_eh_field(m, b, node, 4));
    b.CreateStore(b.getTrue(), vyx_eh_field(m, b, node, 5));
    b.CreateStore(node, head);
    b.CreateCondBr(b.CreateICmpEQ(previous, vyx_eh_null(m)), done, link);
    b.SetInsertPoint(link); b.CreateStore(node, vyx_eh_field(m, b, previous, 0)); b.CreateBr(done);
    b.SetInsertPoint(done);
}
static llvm::Value* vyx_eh_prepare_constructor(VyxRtModule* m, llvm::Value* object) {
    auto& b = *m->builder;
    auto* fn = b.GetInsertBlock()->getParent();
    auto* head = vyx_eh_state(m, fn).head;
    auto* found = b.CreateCall(vyx_eh_find_function(m), {head, object}, "eh.constructor.node");
    auto* old = b.GetInsertBlock();
    auto* fresh = llvm::BasicBlock::Create(*m->ctx, "eh.inline.constructor", fn);
    auto* join = llvm::BasicBlock::Create(*m->ctx, "eh.constructor.ready", fn);
    b.CreateCondBr(b.CreateICmpEQ(found, vyx_eh_null(m)), fresh, join);
    b.SetInsertPoint(fresh);
    auto* node = vyx_eh_new_node(m, fn);
    vyx_eh_push(m, node, object, vyx_eh_null(m));
    auto* freshEnd = b.GetInsertBlock();
    b.CreateBr(join);
    b.SetInsertPoint(join);
    auto* result = b.CreatePHI(vyx_eh_ptr(m), 2, "eh.constructor.cleanup");
    result->addIncoming(found, old);
    result->addIncoming(node, freshEnd);
    return result;
}
static bool vyx_eh_prepare_call(VyxRtModule* m, void* callee,
                                llvm::ArrayRef<llvm::Value*> argv) {
    auto* f = llvm::dyn_cast<llvm::Function>(asVal(callee));
    auto* bb = m->builder->GetInsertBlock();
    if (!f || !bb) return true;
    auto* fn = bb->getParent();
    if (f->hasFnAttribute(llvm::Attribute::UWTable) && !f->doesNotThrow()
        && !fn->hasPersonalityFn()) {
        m->lastError = "shared_abi call requires a caller participating in the same propagation ABI";
        return false;
    }
    if (argv.empty() || !argv[0]->getType()->isPointerTy()
        || !fn->hasPersonalityFn()) return true;
    const bool destroy = m->ehDestructors.count(f) != 0;
    const bool release = m->ehReleases.count(f) != 0;
    auto state = m->ehFunctions.find(fn);
    if ((!destroy && !release) || state == m->ehFunctions.end()) return true;
    auto& b = *m->builder;
    auto* head = state->second.head;
    auto* node = b.CreateCall(vyx_eh_find_function(m), {head, argv[0]}, "eh.finished.node");
    auto* change = llvm::BasicBlock::Create(*m->ctx, "eh.finish", fn);
    auto* unlink = llvm::BasicBlock::Create(*m->ctx, "eh.unlink", fn);
    auto* done = llvm::BasicBlock::Create(*m->ctx, "eh.finished", fn);
    b.CreateCondBr(b.CreateICmpEQ(node, vyx_eh_null(m)), done, change);
    b.SetInsertPoint(change);
    if (destroy) {
        auto* freeAction = b.CreateLoad(vyx_eh_ptr(m), vyx_eh_field(m, b, node, 4));
        b.CreateStore(freeAction, vyx_eh_field(m, b, node, 3));
        b.CreateCondBr(b.CreateICmpEQ(freeAction, vyx_eh_null(m)), unlink, done);
    } else { b.CreateBr(unlink); }
    b.SetInsertPoint(unlink);
    b.CreateCall(vyx_eh_unlink_function(m), {head, node});
    b.CreateBr(done);
    b.SetInsertPoint(done);
    return true;
}
static bool vyx_eh_destructor_needs_invoke(VyxRtModule* m, void* callee) {
    auto* f = llvm::dyn_cast<llvm::Function>(asVal(callee));
    auto* bb = m->builder->GetInsertBlock();
    return f && bb && bb->getParent()->hasPersonalityFn()
        && m->ehDestructors.count(f) != 0 && !f->doesNotThrow();
}
static llvm::BasicBlock* vyx_rt_ensure_cleanup_unwind_block(VyxRtModule* m, llvm::Function* fn) {
    auto& state = vyx_eh_state(m, fn);
    if (state.unwind) return state.unwind;
    auto* padBB = llvm::BasicBlock::Create(*m->ctx, "ehcleanup", fn);
    state.unwind = padBB;
    auto* loop = llvm::BasicBlock::Create(*m->ctx, "eh.cleanup.loop", fn);
    auto* pop = llvm::BasicBlock::Create(*m->ctx, "eh.cleanup.pop", fn);
    auto* run = llvm::BasicBlock::Create(*m->ctx, "eh.cleanup.run", fn);
    auto* resume = llvm::BasicBlock::Create(*m->ctx, "eh.cleanup.resume", fn);
    llvm::IRBuilder<> b(padBB);
    llvm::CleanupPadInst* token = nullptr;
    llvm::LandingPadInst* landing = nullptr;
    if (vyx_rt_module_uses_msvc_eh(m)) {
        token = b.CreateCleanupPad(llvm::ConstantTokenNone::get(*m->ctx));
    } else {
        landing = b.CreateLandingPad(llvm::StructType::get(vyx_eh_ptr(m), b.getInt32Ty()), 0, "eh.lpad");
        landing->setCleanup(true);
    }
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    auto* node = b.CreateLoad(vyx_eh_ptr(m), state.head, "eh.top");
    b.CreateCondBr(b.CreateICmpEQ(node, vyx_eh_null(m)), resume, pop);
    b.SetInsertPoint(pop);
    auto* object = b.CreateLoad(vyx_eh_ptr(m), vyx_eh_field(m, b, node, 2));
    auto* action = b.CreateLoad(vyx_eh_ptr(m), vyx_eh_field(m, b, node, 3));
    auto* next = b.CreateLoad(vyx_eh_ptr(m), vyx_eh_field(m, b, node, 1));
    b.CreateStore(next, state.head);
    b.CreateStore(b.getFalse(), vyx_eh_field(m, b, node, 5));
    b.CreateCondBr(b.CreateICmpEQ(action, vyx_eh_null(m)), loop, run);
    b.SetInsertPoint(run);
    llvm::SmallVector<llvm::OperandBundleDef, 1> bundles;
    if (token) bundles.emplace_back("funclet", std::vector<llvm::Value*>{token});
    auto* actionTy = llvm::FunctionType::get(b.getVoidTy(), {vyx_eh_ptr(m)}, false);
    auto* call = b.CreateCall(actionTy, action, {object}, bundles);
    call->setDoesNotThrow();
    b.CreateBr(loop);
    b.SetInsertPoint(resume);
    if (token) b.CreateCleanupRet(token, nullptr);
    else b.CreateResume(landing);
    return padBB;
}
