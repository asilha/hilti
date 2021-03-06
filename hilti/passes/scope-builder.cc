
#include "hilti/hilti-intern.h"

using namespace hilti::passes;

class ScopeClearer : public Pass<> {
public:
    ScopeClearer() : Pass<>("hilti::ScopeClearer")
    {
    }
    virtual ~ScopeClearer()
    {
    }

    bool run(shared_ptr<Node> module) override
    {
        return processAllPreOrder(module);
    }

protected:
    void visit(statement::Block* b) override
    {
        b->scope()->clear();
    }
};

shared_ptr<Scope> ScopeBuilder::_checkDecl(Declaration* decl)
{
    auto id = decl->id();
    auto is_hook = ast::rtti::isA<declaration::Hook>(decl);

    if ( ! id ) {
        error(decl, "declaration without an ID");
        return 0;
    }

#if 0 // It can now.
    if ( id->isScoped() && ! is_hook ) {
        error(decl, "declared ID cannot have a scope");
        return 0;
    }
#endif

    auto block = current<statement::Block>();

    if ( ! block ) {
        error(decl,
              util::fmt("declaration of %s is not part of a block", decl->id()->name().c_str()));
        return 0;
    }

    auto scope = block->scope();

    if ( (! is_hook) && scope->has(decl->id(), false) ) {
        // TODO: We allow scoped here so that we can have both an import for
        // a module and also individually prototyped functions from there.
        // However, what we should really do is allow declarations that are
        // equivalent, but for that we need to add infrastructure to find out
        // if two declarations are declaring the same.
        if ( ! decl->id()->isScoped() ) {
            error(decl, util::fmt("ID %s already declared", decl->id()->name().c_str()));
            return 0;
        }
    }

    return scope;
}

bool ScopeBuilder::run(shared_ptr<Node> module)
{
    ScopeClearer clearer;
    clearer.run(module);

    auto m = ast::rtti::tryCast<Module>(module);
    m->body()->scope()->clear();

    if ( ! processAllPreOrder(module) )
        return false;

    for ( auto i : m->importedIDs() ) {
        auto mscope = _context->scopeAlias(i);
        assert(mscope);
        assert(mscope->id());
        mscope->setParent(m->body()->scope());
        m->body()->scope()->addChild(mscope->id(), mscope);
    };

    return errors() == 0;
}

void ScopeBuilder::visit(Module* m)
{
}

void ScopeBuilder::visit(statement::Block* b)
{
    if ( ! b->id() || ! b->id()->name().size() )
        return;

    shared_ptr<Scope> scope = nullptr;

    auto func = current<hilti::Function>();

    if ( ! func ) // TODO: Too bad current() doesn't follow the class hierarchy ...
        func = current<hilti::Hook>();

    if ( func ) {
        if ( ! func->body() )
            // Just a declaration without implementation.
            return;

        scope = ast::rtti::checkedCast<statement::Block>(func->body())->scope();
    }

    else {
        auto module = current<hilti::Module>();

        if ( ! module ) {
            error(b, util::fmt("declaration of block is not part of a function or module"));
            return;
        }

        scope = module->body()->scope();
    }

    if ( scope->has(b->id(), false) ) {
        error(b, util::fmt("ID %s already declared", b->id()->name().c_str()));
        return;
    }

    auto block = b->sharedPtr<statement::Block>();
    auto expr = shared_ptr<expression::Block>(new expression::Block(block, block->location()));
    scope->insert(b->id(), expr);
}

void ScopeBuilder::visit(declaration::Variable* v)
{
    auto scope = _checkDecl(v);

    if ( ! scope )
        return;

    auto var = v->variable()->sharedPtr<Variable>();
    auto expr = std::make_shared<expression::Variable>(var, var->location());
    scope->insert(v->id(), expr, true);
}

void ScopeBuilder::visit(declaration::Type* t)
{
    auto scope = _checkDecl(t);

    if ( ! scope )
        return;

    auto type = t->type();
    auto expr = shared_ptr<expression::Type>(new expression::Type(type, type->location()));
    scope->insert(t->id(), expr, true);

    // Link in any type-specific scope the type may define.
    auto tscope = t->type()->typeScope();

    if ( tscope ) {
        tscope->setParent(scope);
        scope->addChild(t->id(), tscope);
    }
}

void ScopeBuilder::visit(declaration::Constant* c)
{
    auto scope = _checkDecl(c);

    if ( ! scope )
        return;

    scope->insert(c->id(), c->constant());
}

void ScopeBuilder::visit(declaration::Function* f)
{
    auto scope = _checkDecl(f);

    if ( ! scope )
        return;

    auto func = f->function()->sharedPtr<Function>();
    auto expr = shared_ptr<expression::Function>(new expression::Function(func, func->location()));

    // if ( ! f->id()->isScoped() )
    scope->insert(f->id(), expr, true);

    if ( ! func->body() )
        // Just a declaration without implementation.
        return;

    // Add parameters to body's scope.
    scope = ast::rtti::checkedCast<statement::Block>(func->body())->scope();

    for ( auto p : func->type()->parameters() ) {
        auto pexpr = shared_ptr<expression::Parameter>(new expression::Parameter(p, p->location()));
        scope->insert(p->id(), pexpr);
    }
}

void ScopeBuilder::visit(declaration::Hook* t)
{
}

void ScopePrinter::visit(statement::Block* b)
{
    auto mod = current<Module>();

    _out << "Module " << (mod ? mod->id()->name() : string("<null>")) << std::endl;

    b->scope()->dump(_out);
    _out << std::endl;
    _out << "+++++" << std::endl;
    _out << std::endl;
}
