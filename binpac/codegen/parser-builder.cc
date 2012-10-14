
#include <hilti.h>

#include "parser-builder.h"
#include "expression.h"
#include "grammar.h"
#include "production.h"
#include "statement.h"
#include "type.h"
#include "declaration.h"
#include "attribute.h"

using namespace binpac;
using namespace binpac::codegen;

// A set of helper functions to create HILTI types.

static shared_ptr<hilti::Type> _hiltiTypeBytes()
{
    return hilti::builder::reference::type(hilti::builder::bytes::type());
}

static shared_ptr<hilti::Type> _hiltiTypeIteratorBytes()
{
    return hilti::builder::iterator::typeBytes();
}

static shared_ptr<hilti::Type> _hiltiTypeLookAhead()
{
    return hilti::builder::integer::type(32);
}

static shared_ptr<hilti::Type> _hiltiTypeCookie()
{
    return hilti::builder::reference::type(hilti::builder::type::byName("BinPACHilti::UserCookie"));
}

static shared_ptr<hilti::Type> _hiltiTypeParseResult()
{
    // (__cur, __lahead, __lahstart).
    hilti::builder::type_list ttypes = { _hiltiTypeIteratorBytes(), _hiltiTypeLookAhead(), _hiltiTypeIteratorBytes() };
    return hilti::builder::tuple::type(ttypes);
}

// Returns the value representin "no look-ahead""
static shared_ptr<hilti::Expression> _hiltiLookAheadNone()
{
    return hilti::builder::integer::create(0);
}

static shared_ptr<hilti::Type> _hiltiTypeMatchTokenState()
{
    return hilti::builder::reference::type(hilti::builder::match_token_state::type());
}

static shared_ptr<hilti::Type> _hiltiTypeMatchResult()
{
    auto i32 = hilti::builder::integer::type(32);
    auto iter = hilti::builder::iterator::type(hilti::builder::bytes::type());
    return hilti::builder::tuple::type({i32, iter});
}

// A class collecting the current set of parser arguments.
class binpac::codegen::ParserState
{
public:
    typedef std::list<shared_ptr<hilti::Expression>> parameter_list;

    ParserState(shared_ptr<binpac::type::Unit> unit,
           shared_ptr<hilti::Expression> self = nullptr,
           shared_ptr<hilti::Expression> data = nullptr,
           shared_ptr<hilti::Expression> cur = nullptr,
           shared_ptr<hilti::Expression> lahead = nullptr,
           shared_ptr<hilti::Expression> lahstart = nullptr,
           shared_ptr<hilti::Expression> cookie = nullptr
           );

    shared_ptr<hilti::Expression> hiltiArguments() const;

    shared_ptr<binpac::type::Unit> unit;
    shared_ptr<hilti::Expression> self;
    shared_ptr<hilti::Expression> data;
    shared_ptr<hilti::Expression> cur;
    shared_ptr<hilti::Expression> lahead;
    shared_ptr<hilti::Expression> lahstart;
    shared_ptr<hilti::Expression> cookie;
};

ParserState::ParserState(shared_ptr<binpac::type::Unit> arg_unit,
               shared_ptr<hilti::Expression> arg_self,
               shared_ptr<hilti::Expression> arg_data,
               shared_ptr<hilti::Expression> arg_cur,
               shared_ptr<hilti::Expression> arg_lahead,
               shared_ptr<hilti::Expression> arg_lahstart,
               shared_ptr<hilti::Expression> arg_cookie
               )
{
    unit = arg_unit;
    self = (arg_self ? arg_self : hilti::builder::id::create("__self"));
    data = (arg_data ? arg_data : hilti::builder::id::create("__data"));
    cur = (arg_cur ? arg_cur : hilti::builder::id::create("__cur"));
    lahead = (arg_lahead ? arg_lahead : hilti::builder::id::create("__lahead"));
    lahstart = (arg_lahstart ? arg_lahstart : hilti::builder::id::create("__lahstart"));
    cookie = (arg_cookie ? arg_cookie : hilti::builder::id::create("__cookie"));
}

shared_ptr<hilti::Expression> ParserState::hiltiArguments() const
{
    hilti::builder::tuple::element_list args = { self, data, cur, lahead, lahstart, cookie };
    return hilti::builder::tuple::create(args, unit->location());
}

ParserBuilder::ParserBuilder(CodeGen* cg)
    : CGVisitor<shared_ptr<hilti::Expression>, shared_ptr<type::unit::item::Field>>(cg, "ParserBuilder")
{
}

ParserBuilder::~ParserBuilder()
{
}

shared_ptr<binpac::type::Unit> ParserBuilder::unit() const
{
    return state()->unit;
}

void ParserBuilder::hiltiExportParser(shared_ptr<type::Unit> unit)
{
    auto parse_host = _hiltiCreateHostFunction(unit, false);
    // auto parse_sink = _hiltiCreateHostFunction(unit, true);
    _hiltiCreateParserInitFunction(unit, parse_host, parse_host);
}

void ParserBuilder::hiltiDefineHook(shared_ptr<ID> id, shared_ptr<Hook> hook)
{
    auto unit = hook->unit();
    assert(unit);

    if ( util::startsWith(id->local(), "%") )
        _hiltiDefineHook(_hookForUnit(unit, id->local()), false, unit, hook->body(), nullptr, hook->priority());

    else {
        auto i = unit->item(id);
        assert(i && i->type());
        auto dd = hook->foreach() ? ast::type::checkedTrait<type::trait::Container>(i->type())->elementType() : nullptr;
        _hiltiDefineHook(_hookForItem(unit, i, hook->foreach()), hook->foreach(), unit, hook->body(), dd, hook->priority());
    }
}

void ParserBuilder::hiltiRunFieldHooks(shared_ptr<type::unit::Item> item)
{
    _hiltiRunHook(_hookForItem(state()->unit, item, false), false, nullptr);
}

void ParserBuilder::hiltiUnitHooks(shared_ptr<type::Unit> unit)
{
    for ( auto i : unit->items() ) {
        if ( ! i->type() )
            continue;

        for ( auto h : i->hooks() ) {
            auto dd = h->foreach() ? ast::type::checkedTrait<type::trait::Container>(i->type())->elementType() : nullptr;
            _hiltiDefineHook(_hookForItem(unit, i->sharedPtr<type::unit::Item>(), h->foreach()),
                             h->foreach(), unit, h->body(), dd, h->priority());
        }
    }

    for ( auto g : unit->globalHooks() ) {
        for ( auto h : g->hooks() ) {

            if ( util::startsWith(g->id()->local(), "%") )
                _hiltiDefineHook(_hookForUnit(unit, g->id()->local()), false, unit, h->body(), nullptr, h->priority());

            else {
                auto i = unit->item(g->id());
                assert(i && i->type());
                auto dd = h->foreach() ? ast::type::checkedTrait<type::trait::Container>(i->type())->elementType() : nullptr;
                _hiltiDefineHook(_hookForItem(unit, i, h->foreach()), h->foreach(), unit, h->body(), dd, h->priority());
            }
        }
    }
}

void ParserBuilder::_hiltiCreateParserInitFunction(shared_ptr<type::Unit> unit,
                                                   shared_ptr<hilti::Expression> parse_host,
                                                   shared_ptr<hilti::Expression> parse_sink)
{
    string name = util::fmt("init_%s", unit->id()->name().c_str());
    auto void_ = hilti::builder::function::result(hilti::builder::void_::type());
    auto func = cg()->moduleBuilder()->pushFunction(name, void_, {});
    func->function()->setInitFunction();

    auto parser = _hiltiParserDefinition(unit);
    auto funcs = cg()->builder()->addLocal("__funcs", hilti::builder::tuple::type({ hilti::builder::caddr::type(), hilti::builder::caddr::type() }));

    hilti::builder::list::element_list mtypes;

    for ( auto p : unit->properties() ) {
        if ( p->id()->name() != "mimetype" )
            continue;

        mtypes.push_back(cg()->hiltiExpression(p->value()));
        }


    auto mime_types = hilti::builder::list::create(hilti::builder::string::type(), mtypes, unit->location());

    cg()->builder()->addInstruction(parser, hilti::instruction::struct_::New,
                                    hilti::builder::id::create("BinPACHilti::Parser"));

    cg()->builder()->addInstruction(hilti::instruction::struct_::Set, parser,
                                    hilti::builder::string::create("name"),
                                    hilti::builder::string::create(unit->id()->name()));

    cg()->builder()->addInstruction(hilti::instruction::struct_::Set, parser,
                                    hilti::builder::string::create("description"),
                                    hilti::builder::string::create("No description yet."));

    cg()->builder()->addInstruction(hilti::instruction::struct_::Set, parser,
                                    hilti::builder::string::create("params"),
                                    hilti::builder::integer::create(unit->parameters().size()));

    cg()->builder()->addInstruction(hilti::instruction::struct_::Set, parser,
                                    hilti::builder::string::create("mime_types"),
                                    mime_types);

    auto f = cg()->builder()->addLocal("__f", hilti::builder::caddr::type());

    cg()->builder()->addInstruction(funcs, hilti::instruction::caddr::Function, parse_host);

    cg()->builder()->addInstruction(f, hilti::instruction::tuple::Index, funcs, hilti::builder::integer::create(0));
    cg()->builder()->addInstruction(hilti::instruction::struct_::Set, parser,
                                    hilti::builder::string::create("parse_func"), f);

    cg()->builder()->addInstruction(f, hilti::instruction::tuple::Index, funcs, hilti::builder::integer::create(1));
    cg()->builder()->addInstruction(hilti::instruction::struct_::Set, parser,
                                    hilti::builder::string::create("resume_func"), f);

    cg()->builder()->addInstruction(funcs, hilti::instruction::caddr::Function, parse_sink);

    cg()->builder()->addInstruction(f, hilti::instruction::tuple::Index, funcs, hilti::builder::integer::create(0));
    cg()->builder()->addInstruction(hilti::instruction::struct_::Set, parser,
                                    hilti::builder::string::create("parse_func_sink"), f);

    cg()->builder()->addInstruction(f, hilti::instruction::tuple::Index, funcs, hilti::builder::integer::create(1));
    cg()->builder()->addInstruction(hilti::instruction::struct_::Set, parser,
                                    hilti::builder::string::create("resume_func_sink"), f);

    // "type_info" is initialized by "BinPACHilti::register_parser".

#if 0
    TODO
        # Set the new_func field if our parser does not receive further
        # parameters.
        if not self._grammar.params():
            name = self._type.nameFunctionNew()
            fid = hilti.operand.ID(hilti.id.Unknown(name, self.cg().moduleBuilder().module().scope()))
            builder.caddr_function(funcs, fid)
            builder.tuple_index(f, funcs, 0)
            builder.struct_set(parser, "new_func", f)

        else:
            # Set the new_func field to null.
            null = builder.addLocal("null", hilti.type.CAddr())
            builder.struct_set(parser, "new_func", null)
#endif

    auto ti = hilti::builder::type::create(cg()->hiltiTypeParseObject(unit));

    cg()->builder()->addInstruction(hilti::instruction::flow::CallVoid,
                                    hilti::builder::id::create("BinPACHilti::register_parser"),
                                    hilti::builder::tuple::create( { parser, ti } ));

    cg()->builder()->addInstruction(hilti::instruction::flow::ReturnVoid);

    cg()->moduleBuilder()->popFunction();
}

shared_ptr<hilti::Expression> ParserBuilder::_hiltiCreateHostFunction(shared_ptr<type::Unit> unit, bool sink)
{
    auto utype = cg()->hiltiType(unit);
    auto rtype = hilti::builder::function::result(utype);

    auto arg1 = hilti::builder::function::parameter("__data", _hiltiTypeBytes(), false, nullptr);
    auto arg2 = hilti::builder::function::parameter("__cookie", _hiltiTypeCookie(), false, nullptr);

    hilti::builder::function::parameter_list args = { arg1, arg2 };

    if ( sink )
        args.push_front(hilti::builder::function::parameter("__self", utype, false, nullptr));

    else {
        // Add unit parameters.
        for ( auto p : unit->parameters() ) {
            auto type = cg()->hiltiType(p->type());

            shared_ptr<hilti::Expression> def = nullptr;

            if ( p->default_() )
                def = cg()->hiltiExpression(p->default_());
            else
                def = cg()->hiltiDefault(p->type());

            args.push_back(hilti::builder::function::parameter(cg()->hiltiID(p->id()), type, true, def));
        }
    }

    string name = util::fmt("parse_%s", unit->id()->name().c_str());

    if ( sink )
        name = "__" + name + "_sink";

    auto func = cg()->moduleBuilder()->pushFunction(name, rtype, args);
    cg()->moduleBuilder()->exportID(name);

    auto self = sink ? hilti::builder::id::create("__self") : _allocateParseObject(unit, false);
    auto data = hilti::builder::id::create("__data");
    auto cur = cg()->builder()->addLocal("__cur", _hiltiTypeIteratorBytes());
    auto lah = cg()->builder()->addLocal("__lahead", _hiltiTypeLookAhead(), _hiltiLookAheadNone());
    auto lahstart = cg()->builder()->addLocal("__lahstart", _hiltiTypeIteratorBytes());
    auto cookie = hilti::builder::id::create("__cookie");

    cg()->builder()->addInstruction(cur, hilti::instruction::iterBytes::Begin, data);

    auto pstate = std::make_shared<ParserState>(unit, self, data, cur, lah, lahstart, cookie);
    pushState(pstate);

    hilti_expression_list params;

    for ( auto p : state()->unit->parameters() )
        params.push_back(hilti::builder::id::create(p->id()->name()));

    _prepareParseObject(params);

    auto presult = cg()->builder()->addLocal("__presult", _hiltiTypeParseResult());
    auto pfunc = cg()->hiltiParseFunction(unit);

    if ( cg()->debugLevel() > 0 ) {
        _hiltiDebug(unit->id()->name());
        cg()->builder()->debugPushIndent();
    }

    cg()->builder()->addInstruction(presult, hilti::instruction::flow::CallResult, pfunc, state()->hiltiArguments());

    if ( cg()->debugLevel() > 0 )
        cg()->builder()->debugPopIndent();

    _finalizeParseObject();

    cg()->builder()->addInstruction(hilti::instruction::flow::ReturnResult, self);

    popState();

    return cg()->moduleBuilder()->popFunction();
}

shared_ptr<hilti::Expression> ParserBuilder::hiltiCreateParseFunction(shared_ptr<type::Unit> unit)
{
    auto grammar = unit->grammar();
    assert(grammar);

    auto name = util::fmt("parse_%s_internal", grammar->name().c_str());
    auto func = _newParseFunction(name, unit);

    _last_parsed_value = nullptr;
    _store_values = 1;
    bool success = processOne(grammar->root());
    assert(success);

    hilti::builder::tuple::element_list elems = { state()->cur, state()->lahead, state()->lahstart };
    cg()->builder()->addInstruction(hilti::instruction::flow::ReturnResult, hilti::builder::tuple::create(elems));

    popState();

    return cg()->moduleBuilder()->popFunction();
}

shared_ptr<ParserState> ParserBuilder::state() const
{
    assert(_states.size());
    return _states.back();
}

void ParserBuilder::pushState(shared_ptr<ParserState> state)
{
    _states.push_back(state);
}

void ParserBuilder::popState()
{
    assert(_states.size());
    _states.pop_back();
}

shared_ptr<hilti::Expression> ParserBuilder::_newParseFunction(const string& name, shared_ptr<type::Unit> u)
{
    auto arg1 = hilti::builder::function::parameter("__self", cg()->hiltiType(u), false, nullptr);
    auto arg2 = hilti::builder::function::parameter("__data", _hiltiTypeBytes(), false, nullptr);
    auto arg3 = hilti::builder::function::parameter("__cur", _hiltiTypeIteratorBytes(), false, nullptr);
    auto arg4 = hilti::builder::function::parameter("__lahead", _hiltiTypeLookAhead(), false, nullptr);
    auto arg5 = hilti::builder::function::parameter("__lahstart", _hiltiTypeIteratorBytes(), false, nullptr);
    auto arg6 = hilti::builder::function::parameter("__cookie", _hiltiTypeCookie(), false, nullptr);

    auto rtype = hilti::builder::function::result(_hiltiTypeParseResult());

    auto func = cg()->moduleBuilder()->pushFunction(name, rtype, { arg1, arg2, arg3, arg4, arg5, arg6 });

    pushState(std::make_shared<ParserState>(u));

    return std::make_shared<hilti::expression::Function>(func->function(), func->location());
}

shared_ptr<hilti::Type> ParserBuilder::hiltiTypeParseObject(shared_ptr<type::Unit> u)
{
    hilti::builder::struct_::field_list fields;

    // One struct field per non-constant unit field.
    for ( auto f : u->fields() ) {
        if ( ast::isA<type::unit::item::field::Constant>(f) )
            continue;

        auto ftype = ast::type::checkedTrait<type::trait::Parseable>(f->type())->fieldType();
        auto type = cg()->hiltiType(ftype);
        assert(type);

        auto sfield = hilti::builder::struct_::field(f->id()->name(), type, nullptr, false, f->location());

        if ( f->anonymous() )
            sfield->metaInfo()->add(std::make_shared<ast::MetaNode>("opt:can-remove"));

        fields.push_back(sfield);
    }

    // One struct field per variable.
    for ( auto v : u->variables() ) {
        auto type = cg()->hiltiType(v->type());
        assert(type);

        shared_ptr<hilti::Expression> def = nullptr;

        if ( v->default_() )
            def = cg()->hiltiExpression(v->default_());
        else
            def = cg()->hiltiDefault(v->type());

        fields.push_back(hilti::builder::struct_::field(v->id()->name(), type, def, false, v->location()));
    }

    // One struct field per parameter.
    for ( auto p : u->parameters() ) {
        auto name = util::fmt("__p_%s", p->id()->name());
        auto type = cg()->hiltiType(p->type());
        auto sfield = hilti::builder::struct_::field(name, type, nullptr, false, p->location());

        fields.push_back(sfield);
    }

    return hilti::builder::struct_::type(fields, u->location());
}

shared_ptr<hilti::Expression> ParserBuilder::_allocateParseObject(shared_ptr<Type> unit, bool store_in_self)
{
    auto rt = cg()->hiltiType(unit);
    auto ut = ast::checkedCast<hilti::type::Reference>(rt)->argType();

    auto pobj = store_in_self ? state()->self : cg()->builder()->addLocal("__pobj", rt);

    cg()->builder()->addInstruction(pobj, hilti::instruction::struct_::New, hilti::builder::type::create(ut));

    return pobj;
}

void ParserBuilder::_prepareParseObject(const hilti_expression_list& params)
{
    // Initialize the parameter fields.
    auto arg = params.begin();
    auto u = state()->unit;

    for ( auto p : u->parameters() ) {
        assert(arg != params.end());
        auto field = hilti::builder::string::create(util::fmt("__p_%s", p->id()->name()));
        cg()->builder()->addInstruction(hilti::instruction::struct_::Set, state()->self, field, *arg++);
    }

    // Initialize non-constant fields with their explicit &defaults.
    for ( auto f : u->fields() ) {
        if ( ast::isA<type::unit::item::field::Constant>(f) )
            continue;

        auto attr = f->attributes()->lookup("default");

        if ( ! attr )
            continue;

        auto id = hilti::builder::string::create(f->id()->name());
        auto def = cg()->hiltiExpression(attr->value());
        cg()->builder()->addInstruction(hilti::instruction::struct_::Set, state()->self, id, def);
    }

    // Initialize variables with either &defaults where given, or their HILTI defaults.
    for ( auto v : u->variables() ) {
        auto id = hilti::builder::string::create(v->id()->name());
        auto attr = v->attributes()->lookup("default");
        auto def = attr ? cg()->hiltiExpression(attr->value()) : cg()->hiltiDefault(v->type());
        cg()->builder()->addInstruction(hilti::instruction::struct_::Set, state()->self, id, def);
    }

    _hiltiRunHook(_hookForUnit(state()->unit, "%init"), false, nullptr);
}

void ParserBuilder::_finalizeParseObject()
{
    _hiltiRunHook(_hookForUnit(state()->unit, "%done"), false, nullptr);

    // Clear the parameters to avoid ref-counting cycles.
    for ( auto p : state()->unit->parameters() ) {
        auto name = util::fmt("__p_%s", p->id()->name());
        auto field = hilti::builder::string::create(util::fmt("__p_%s", p->id()->name()));
        cg()->builder()->addInstruction(hilti::instruction::struct_::Unset, state()->self, field);
    }
}

void ParserBuilder::_startingProduction(shared_ptr<Production> p, shared_ptr<type::unit::item::Field> field)
{
    cg()->builder()->addComment(util::fmt("Production: %s", util::strtrim(p->render().c_str())));
    cg()->builder()->addComment("");

    if ( cg()->debugLevel() ) {
        _hiltiDebugVerbose(util::fmt("parsing %s", util::strtrim(p->render().c_str())));
        _hiltiDebugShowToken("look-ahead", state()->lahead);
        _hiltiDebugShowInput("input", state()->cur);
    }

    if ( ! field || ! storingValues() )
        return;

    // Initalize the struct field with the HILTI default value if not already
    // set.
    auto not_set = builder()->addTmp("not_set", hilti::builder::boolean::type(), nullptr, true);
    auto name = hilti::builder::string::create(field->id()->name());
    cg()->builder()->addInstruction(not_set, hilti::instruction::struct_::IsSet, state()->self, name);
    cg()->builder()->addInstruction(not_set, hilti::instruction::boolean::Not, not_set);

    auto branches = cg()->builder()->addIf(not_set);
    auto set_default = std::get<0>(branches);
    auto cont = std::get<1>(branches);

    cg()->moduleBuilder()->pushBuilder(set_default);
    auto ftype = ast::type::checkedTrait<type::trait::Parseable>(field->type())->fieldType();
    auto def = cg()->hiltiDefault(ftype);
    cg()->builder()->addInstruction(hilti::instruction::struct_::Set, state()->self, name, def);
    cg()->builder()->addInstruction(hilti::instruction::flow::Jump, cont->block());
    cg()->moduleBuilder()->popBuilder(set_default);

    cg()->moduleBuilder()->pushBuilder(cont);

   // Leave builder on stack.
}

void ParserBuilder::_finishedProduction(shared_ptr<Production> p)
{
    cg()->builder()->addComment("");
}

void ParserBuilder::_newValueForField(shared_ptr<type::unit::item::Field> field, shared_ptr<hilti::Expression> value)
{
    auto name = field->id()->name();

    if ( cg()->debugLevel() > 0 && ! ast::isA<type::Unit>(field->type()))
        cg()->builder()->addDebugMsg("binpac", util::fmt("%s = %%s", name), value);

    if ( value ) {
        if ( storingValues() )
            cg()->builder()->addInstruction(hilti::instruction::struct_::Set, state()->self,
                                            hilti::builder::string::create(name), value);
    }

    else {
        auto ftype = ast::type::checkedTrait<type::trait::Parseable>(field->type())->fieldType();
        value = cg()->moduleBuilder()->addTmp("field-val", cg()->hiltiType(ftype));
        cg()->builder()->addInstruction(value, hilti::instruction::struct_::Get, state()->self,
                                        hilti::builder::string::create(name));
    }

    _hiltiRunHook(_hookForItem(state()->unit, field, false), false, nullptr);

    _last_parsed_value = value;
}

shared_ptr<hilti::Expression> ParserBuilder::_hiltiParserDefinition(shared_ptr<type::Unit> unit)
{
    string name = util::fmt("__binpac_parser_%s", unit->id()->name().c_str());

    auto parser = ast::tryCast<hilti::Expression>(cg()->moduleBuilder()->lookupNode("parser-definition", name));

    if ( parser )
        return parser;

    auto t = hilti::builder::reference::type(hilti::builder::type::byName("BinPACHilti::Parser"));
    parser = cg()->moduleBuilder()->addGlobal(name, t);
    cg()->moduleBuilder()->cacheNode("parser-definition", name, parser);
    return parser;
}

void ParserBuilder::_hiltiDebug(const string& msg)
{
    if ( cg()->debugLevel() > 0 )
        cg()->builder()->addDebugMsg("binpac", msg);
}

void ParserBuilder::_hiltiDebugVerbose(const string& msg)
{
    if ( cg()->debugLevel() > 0 )
        cg()->builder()->addDebugMsg("binpac-verbose", string("- ") + msg);
}

void ParserBuilder::_hiltiDebugShowToken(const string& tag, shared_ptr<hilti::Expression> token)
{
    if ( cg()->debugLevel() == 0 )
        return;

    cg()->builder()->addDebugMsg("binpac-verbose", "- %s is %s", hilti::builder::string::create(tag), token);
}

void ParserBuilder::_hiltiDebugShowInput(const string& tag, shared_ptr<hilti::Expression> cur)
{
    if ( cg()->debugLevel() == 0 )
        return;

    auto next = cg()->builder()->addTmp("next5", _hiltiTypeBytes());
    cg()->builder()->addInstruction(next, hilti::instruction::flow::CallResult,
                                    hilti::builder::id::create("BinPACHilti::next_bytes"),
                                    hilti::builder::tuple::create({ cur, hilti::builder::integer::create(5) }));

    cg()->builder()->addDebugMsg("binpac-verbose", "- %s is |%s...|", hilti::builder::string::create(tag), next);
}

std::pair<bool, string> ParserBuilder::_hookName(const string& path)
{
    bool local = false;
    auto name = path;

    // If the module part of the ID matches the current module, remove.
    auto curmod = cg()->moduleBuilder()->module()->id()->name();

    if ( util::startsWith(name, curmod + "::") ) {
        local = true;
        name = name.substr(curmod.size() + 2, string::npos);
    }

    name = util::strreplace(name, "::", "_");
    name = util::strreplace(name, "%", "__0x37");
    name = string("__hook_") + name;

    return std::make_pair(local, name);
}

void ParserBuilder::_hiltiDefineHook(shared_ptr<ID> id, bool foreach, shared_ptr<type::Unit> unit, shared_ptr<Statement> body, shared_ptr<Type> dollardollar, int priority)
{
    auto t = _hookName(id->pathAsString());
    auto local = t.first;
    auto name = t.second;

    hilti::builder::function::parameter_list p = {
        hilti::builder::function::parameter("__self", cg()->hiltiTypeParseObjectRef(unit), false, nullptr),
        hilti::builder::function::parameter("__cookie", _hiltiTypeCookie(), false, nullptr)
    };

    if ( dollardollar )
        p.push_back(hilti::builder::function::parameter("__dollardollar", cg()->hiltiType(dollardollar), false, nullptr));

    shared_ptr<hilti::Type> rtype = nullptr;

    if ( foreach )
        rtype = hilti::builder::boolean::type();
    else
        rtype = hilti::builder::void_::type();

    cg()->moduleBuilder()->pushHook(name, hilti::builder::function::result(rtype), p, nullptr, priority, 0, false);

    pushState(std::make_shared<ParserState>(unit));

    auto msg = util::fmt("- executing hook %s@%s", id->pathAsString(), string(body->location()));
    _hiltiDebugVerbose(msg);

    cg()->hiltiStatement(body);

    popState();

    cg()->moduleBuilder()->popHook();
}

shared_ptr<hilti::Expression> ParserBuilder::hiltiSelf()
{
    return state()->self;
}

shared_ptr<hilti::Expression> ParserBuilder::_hiltiRunHook(shared_ptr<ID> id, bool foreach, shared_ptr<hilti::Expression> dollardollar)
{
    auto msg = util::fmt("- triggering hook %s", id->pathAsString());
    _hiltiDebugVerbose(msg);

    auto t = _hookName(id->pathAsString());
    auto local = t.first;
    auto name = t.second;

    // Declare the hook if it's in our module and we don't have done that yet.
    if ( local && ! cg()->moduleBuilder()->lookupNode("hook", name) ) {
        hilti::builder::function::parameter_list p = {
            hilti::builder::function::parameter("__self", cg()->hiltiTypeParseObjectRef(state()->unit), false, nullptr),
            hilti::builder::function::parameter("__cookie", _hiltiTypeCookie(), false, nullptr)
        };

        if ( dollardollar )
            p.push_back(hilti::builder::function::parameter("__dollardollar", dollardollar->type(), false, nullptr));


        shared_ptr<hilti::Type> rtype = nullptr;

        if ( foreach )
            rtype = hilti::builder::boolean::type();
        else
            rtype = hilti::builder::void_::type();

        auto hook = cg()->moduleBuilder()->declareHook(name, hilti::builder::function::result(rtype), p);

        cg()->moduleBuilder()->cacheNode("hook", name, hook);
    }

    // Run the hook.
    hilti::builder::tuple::element_list args = { state()->self, state()->cookie };

    if ( dollardollar )
        args.push_back(dollardollar);

    if ( ! foreach ) {
        cg()->builder()->addInstruction(hilti::instruction::hook::Run,
                                        hilti::builder::id::create(name),
                                        hilti::builder::tuple::create(args));

        return nullptr;
    }

    else {
        auto stop = cg()->moduleBuilder()->addTmp("hook_result", hilti::builder::boolean::type(), hilti::builder::boolean::create(false));
        cg()->builder()->addInstruction(stop,
                                        hilti::instruction::hook::Run,
                                        hilti::builder::id::create(name),
                                        hilti::builder::tuple::create(args));
        return stop;
    }

}

shared_ptr<binpac::ID> ParserBuilder::_hookForItem(shared_ptr<type::Unit> unit, shared_ptr<type::unit::Item> item, bool foreach)
{
    string fe = foreach ? "%foreach" : "";
    auto id = util::fmt("%s::%s::%s%s", cg()->moduleBuilder()->module()->id()->name(), unit->id()->name(), item->id()->name(), fe);
    return std::make_shared<binpac::ID>(id);
}

shared_ptr<binpac::ID> ParserBuilder::_hookForUnit(shared_ptr<type::Unit> unit, const string& name)
{
    auto id = util::fmt("%s::%s::%s", cg()->moduleBuilder()->module()->id()->name(), unit->id()->name(), name);
    return std::make_shared<binpac::ID>(id);
}

shared_ptr<hilti::Expression> ParserBuilder::_hiltiMatchTokenInit(const string& name, const std::list<shared_ptr<production::Literal>>& literals)
{
    auto mstate = cg()->builder()->addTmp("match", _hiltiTypeMatchTokenState());

    auto glob = cg()->moduleBuilder()->lookupNode("match-token", name);

    if ( ! glob ) {
        std::list<std::pair<string, string>> tokens;

        for ( auto l : literals ) {
            for ( auto p : l->patterns() )
                tokens.push_back(std::make_pair(util::fmt("%s{#%d}", p.first, l->tokenID()), ""));
        }

        auto op = hilti::builder::regexp::create(tokens);
        auto rty = hilti::builder::reference::type(hilti::builder::regexp::type({"&nosub"}));
        glob = cg()->moduleBuilder()->addGlobal(hilti::builder::id::node(name), rty, op);

        cg()->moduleBuilder()->cacheNode("match-token", name, glob);
    }

    auto pattern = ast::checkedCast<hilti::Expression>(glob);

    cg()->builder()->addInstruction(state()->lahstart, hilti::instruction::operator_::Assign, state()->cur); // Record start position.
    cg()->builder()->addInstruction(mstate, hilti::instruction::regexp::MatchTokenInit, pattern);

    return mstate;
}

shared_ptr<hilti::Expression> ParserBuilder::_hiltiMatchTokenAdvance(shared_ptr<hilti::Expression> mstate)
{
    auto mresult = cg()->builder()->addTmp("match_result", _hiltiTypeMatchResult());
    auto eob = cg()->builder()->addTmp("eob", _hiltiTypeIteratorBytes());
    cg()->builder()->addInstruction(eob, hilti::instruction::iterBytes::End, state()->data);
    cg()->builder()->addInstruction(mresult, hilti::instruction::regexp::MatchTokenAdvanceBytes, mstate, state()->cur, eob);
    return mresult;
}

shared_ptr<hilti::builder::BlockBuilder> ParserBuilder::_hiltiAddMatchTokenErrorCases(shared_ptr<Production> prod,
                                                                             hilti::builder::BlockBuilder::case_list* cases,
                                                                             shared_ptr<hilti::builder::BlockBuilder> repeat,
                                                                             std::list<shared_ptr<production::Literal>> expected
                                                                            )
{
    // Not found.
    auto b = cg()->moduleBuilder()->cacheBlockBuilder("not-found", [&] () {
        _hiltiParseError("look-ahead symbol(s) not found");
    });

    cases->push_back(std::make_pair(_hiltiLookAheadNone(), b));

    // Insufficient input.
    b = cg()->moduleBuilder()->cacheBlockBuilder("insufficient-input", [&] () {
        _hiltiYieldAndTryAgain(prod, b, repeat);
    });

    cases->push_back(std::make_pair(hilti::builder::integer::create(-1), b));

    // Internal error: Unexpected token found (default case for the switch).
    b = cg()->moduleBuilder()->cacheBlockBuilder("match-token-error", [&] () {
        cg()->builder()->addInternalError("unexpected look-ahead symbol returned");
    });

    return b;
}

void ParserBuilder::_hiltiParseError(const string& msg)
{
    _hiltiDebugVerbose("raising parse error");
    cg()->builder()->addThrow("BinPACHilti::ParseError", hilti::builder::string::create(msg));
}

void ParserBuilder::_hiltiYieldAndTryAgain(shared_ptr<Production> prod, shared_ptr<hilti::builder::BlockBuilder> builder, shared_ptr<hilti::builder::BlockBuilder> cont)
{
    if ( ! prod->eodOk() ) {
        _hiltiInsufficientInputHandler();
        cg()->builder()->addInstruction(hilti::instruction::flow::Jump, cont->block());
    }

    else {
        auto eod = cg()->moduleBuilder()->pushBuilder("eod-ok");
        auto result = hilti::builder::tuple::create({state()->cur, state()->lahead, state()->lahstart});
        cg()->builder()->addInstruction(hilti::instruction::flow::ReturnResult, result);
        cg()->moduleBuilder()->popBuilder();

        auto at_eod = _hiltiInsufficientInputHandler();
        cg()->builder()->addInstruction(hilti::instruction::flow::IfElse, at_eod, eod->block(), cont->block());
    }
}

shared_ptr<hilti::Expression> ParserBuilder::_hiltiInsufficientInputHandler(bool eod_ok, shared_ptr<hilti::Expression> iter)
{
    shared_ptr<hilti::Expression> result = nullptr;

    auto frozen = cg()->builder()->addTmp("frozen", hilti::builder::boolean::type(), nullptr, true);
    auto resume = cg()->moduleBuilder()->newBuilder("resume");

    auto suspend = cg()->moduleBuilder()->pushBuilder("suspend");
    _hiltiDebugVerbose("out of input, yielding ...");
    cg()->builder()->addInstruction(hilti::instruction::flow::YieldUntil, state()->data);
    cg()->builder()->addInstruction(hilti::instruction::flow::Jump, resume->block());
    cg()->moduleBuilder()->popBuilder(suspend);

    cg()->builder()->addInstruction(frozen, hilti::instruction::bytes::IsFrozenBytes, state()->data);

    if ( eod_ok ) {
        _hiltiDebugVerbose("insufficient input (but end-of-data is ok here)");
        cg()->builder()->addInstruction(hilti::instruction::flow::IfElse, frozen, resume->block(), suspend->block());
        result = frozen;
    }

    else {
        _hiltiDebugVerbose("insufficient input (but end-of-data is ok here)");

        auto error = cg()->moduleBuilder()->cacheBlockBuilder("error", [&] () {
            _hiltiParseError("insufficient input");
        });

        cg()->builder()->addInstruction(hilti::instruction::flow::IfElse, frozen, error->block(), suspend->block());
        result = hilti::builder::boolean::create(false);
    }

    cg()->moduleBuilder()->pushBuilder(resume);

    // Leave on stack.

    return result;
}

shared_ptr<binpac::Expression> ParserBuilder::_fieldByteOrder(shared_ptr<type::unit::item::Field> field, shared_ptr<type::Unit> unit)
{
    shared_ptr<binpac::Expression> order = nullptr;

    // See if the unit has a byte order property defined.
    auto prop_order = unit->property("byteorder");

    if ( prop_order )
        order = prop_order->value();

    // See if the field has a byte order defined.
    auto field_order = field->attributes()->lookup("byteorder");

    if ( field_order )
        order = field_order->value();

    else {
        // See if the unit has a byteorder property.
        auto unit_order = unit->property("byteorder");

        if ( unit_order )
            order = unit_order->value();
    }

    return order;
}

shared_ptr<hilti::Expression> ParserBuilder::_hiltiIntUnpackFormat(int width, bool signed_, shared_ptr<binpac::Expression> byteorder)
{
    auto hltbo = byteorder ? cg()->hiltiExpression(byteorder) : hilti::builder::id::create("BinPAC::ByteOrder::Big");

    string big, little, host;

    switch ( width ) {
     case 8:
        if ( signed_ ) { little = "Int8Little"; big = "Int8Big"; host = "Int8"; }
        else           { little = "UInt8Little"; big = "UInt8Big"; host = "UInt8"; }
        break;

     case 16:
        if ( signed_ ) { little = "Int16Little"; big = "Int16Big"; host = "Int16"; }
        else           { little = "UInt16Little"; big = "UInt16Big"; host = "UInt16"; }
        break;

     case 32:
        if ( signed_ ) { little = "Int32Little"; big = "Int32Big"; host = "Int32"; }
        else           { little = "UInt32Little"; big = "UInt32Big"; host = "UInt32"; }
        break;

     case 64:
        if ( signed_ ) { little = "Int64Little"; big = "Int64Big"; host = "Int64"; }
        else           { little = "UInt64Little"; big = "UInt64Big"; host = "UInt64"; }
        break;

     default:
        internalError("unsupported bitwidth in _hiltiIntUnpackFormat()");
    }

    auto t1 = hilti::builder::tuple::create({
        hilti::builder::id::create("BinPAC::ByteOrder::Little"),
        hilti::builder::id::create(string("Hilti::Packed::") + little) });

    auto t2 = hilti::builder::tuple::create({
        hilti::builder::id::create("BinPAC::ByteOrder::Big"),
        hilti::builder::id::create(string("Hilti::Packed::") + big) });

    auto t3 = hilti::builder::tuple::create({
        hilti::builder::id::create("BinPAC::ByteOrder::Host"),
        hilti::builder::id::create(string("Hilti::Packed::") + host) });

    auto tuple = hilti::builder::tuple::create({ t1, t2, t3 });
    auto result = cg()->moduleBuilder()->addTmp("fmt", hilti::builder::type::byName("Hilti::Packed"));
    cg()->builder()->addInstruction(result, hilti::instruction::Misc::SelectValue, hltbo, tuple);

    return result;
}

void ParserBuilder::disableStoringValues()
{
    --_store_values;
}

void ParserBuilder::enableStoringValues()
{
    ++_store_values;
}

bool ParserBuilder::storingValues()
{
    return _store_values > 0;
}

////////// Visit methods.

void ParserBuilder::visit(constant::Address* a)
{
}

void ParserBuilder::visit(constant::Bitset* b)
{
}

void ParserBuilder::visit(constant::Bool* b)
{
}

void ParserBuilder::visit(constant::Double* d)
{
}

void ParserBuilder::visit(constant::Enum* e)
{
}

void ParserBuilder::visit(constant::Integer* i)
{
}

void ParserBuilder::visit(constant::Interval* i)
{
}

void ParserBuilder::visit(constant::Network* n)
{
}

void ParserBuilder::visit(constant::Port* p)
{
}

void ParserBuilder::visit(constant::String* s)
{
}

void ParserBuilder::visit(constant::Time* t)
{
}

void ParserBuilder::visit(ctor::Bytes* b)
{
}

void ParserBuilder::visit(ctor::RegExp* r)
{
}

void ParserBuilder::visit(production::Boolean* b)
{
}

void ParserBuilder::visit(production::ChildGrammar* c)
{
    auto field = c->pgMeta()->field;
    assert(field);

    _startingProduction(c->sharedPtr<Production>(), field);

    auto child = c->childType();
    auto subself = _allocateParseObject(child, false);

    auto pstate = std::make_shared<ParserState>(child, subself, state()->data, state()->cur, state()->lahead, state()->lahstart, state()->cookie);
    pushState(pstate);

    hilti_expression_list params;

    for ( auto p : field->parameters() )
        params.push_back(cg()->hiltiExpression(p));

    _prepareParseObject(params);

    auto child_result = cg()->builder()->addLocal("__presult", _hiltiTypeParseResult());
    auto child_func = cg()->hiltiParseFunction(child);

    if ( cg()->debugLevel() > 0 ) {
        _hiltiDebug(field->id()->name());
        cg()->builder()->debugPushIndent();
    }

    cg()->builder()->addInstruction(child_result, hilti::instruction::flow::CallResult, child_func, state()->hiltiArguments());

    if ( cg()->debugLevel() > 0 )
        cg()->builder()->debugPopIndent();

    _finalizeParseObject();

    popState();

    cg()->builder()->addInstruction(state()->cur, hilti::instruction::tuple::Index, child_result, hilti::builder::integer::create(0));
    cg()->builder()->addInstruction(state()->lahead, hilti::instruction::tuple::Index, child_result, hilti::builder::integer::create(1));
    cg()->builder()->addInstruction(state()->lahstart, hilti::instruction::tuple::Index, child_result, hilti::builder::integer::create(2));

    _newValueForField(field, subself);
    _finishedProduction(c->sharedPtr<Production>());
}

void ParserBuilder::visit(production::Counter* c)
{
    auto field = c->pgMeta()->field;
    assert(field);

    _startingProduction(c->sharedPtr<Production>(), field);

    auto i = cg()->builder()->addTmp("count-i", hilti::builder::integer::type(64), cg()->hiltiExpression(c->expression()));
    auto b = cg()->builder()->addTmp("count-bool", hilti::builder::boolean::type());
    auto cont = cg()->moduleBuilder()->newBuilder("count-done");
    auto parse_one = cg()->moduleBuilder()->newBuilder("count-parse-one");

    auto loop = cg()->moduleBuilder()->pushBuilder("count-loop");
    cg()->builder()->addInstruction(b, hilti::instruction::integer::Sleq, i, hilti::builder::integer::create(0));
    cg()->builder()->addInstruction(hilti::instruction::flow::IfElse, b, cont->block(), parse_one->block());
    cg()->moduleBuilder()->popBuilder(loop);

    cg()->moduleBuilder()->pushBuilder(parse_one);

    disableStoringValues();
    processOne(c->body());
    enableStoringValues();

    // Run foreach hook.
    assert(_last_parsed_value);
    auto stop = _hiltiRunHook(_hookForItem(state()->unit, field, true), true, _last_parsed_value);
    cg()->builder()->addInstruction(i, hilti::instruction::integer::Decr, i);
    cg()->builder()->addInstruction(hilti::instruction::flow::IfElse, stop, cont->block(), loop->block());

    cg()->moduleBuilder()->popBuilder(parse_one);

    cg()->moduleBuilder()->pushBuilder(cont);

    _newValueForField(field, nullptr);
    _finishedProduction(c->sharedPtr<Production>());

    // Leave builder on stack.
}

void ParserBuilder::visit(production::Epsilon* e)
{
}

void ParserBuilder::visit(production::Literal* l)
{
    auto field = l->pgMeta()->field;
    assert(field);

    _startingProduction(l->sharedPtr<Production>(), field);

    cg()->builder()->addComment(util::fmt("Literal: %s", field->id()->name()));

    auto token_id = hilti::builder::integer::create(l->tokenID());
    auto name = util::fmt("__literal_%d", l->tokenID());

    // See if we have a look-ahead symbol.
    auto cond = cg()->builder()->addTmp("cond", hilti::builder::boolean::type(), nullptr, true);
    auto equal = cg()->builder()->addInstruction(cond, hilti::instruction::integer::Equal, state()->lahead, _hiltiLookAheadNone());

    auto branches = cg()->builder()->addIfElse(cond);
    auto no_lahead = std::get<0>(branches);
    auto have_lahead = std::get<1>(branches);
    auto done = std::get<2>(branches);

    cg()->moduleBuilder()->pushBuilder(no_lahead);

    // We do not have a look-ahead symbol pending, so search for our literal.
    cg()->builder()->addComment("No look-ahead symbol pending, search literal ...");
    auto mstate = _hiltiMatchTokenInit(name, { l->sharedPtr<production::Literal>() });
    auto mresult = _hiltiMatchTokenAdvance(mstate);

    auto symbol = cg()->moduleBuilder()->addTmp("lahead", hilti::builder::integer::type(32), nullptr, true);
    cg()->builder()->addInstruction(symbol, hilti::instruction::tuple::Index, mresult, hilti::builder::integer::create(0));
    cg()->builder()->addInstruction(state()->cur, hilti::instruction::tuple::Index, mresult, hilti::builder::integer::create(1)); // Move position.

    auto found_lit = cg()->moduleBuilder()->pushBuilder("found-literal");
    found_lit->addInstruction(hilti::instruction::flow::Jump, done->block());
    cg()->moduleBuilder()->popBuilder(found_lit);

    hilti::builder::BlockBuilder::case_list cases;
    cases.push_back(std::make_pair(token_id, found_lit));

    auto default_ = _hiltiAddMatchTokenErrorCases(l->sharedPtr<Production>(), &cases, no_lahead, { l->sharedPtr<production::Literal>() });

    cg()->builder()->addSwitch(symbol, default_, cases);

    cg()->moduleBuilder()->popBuilder(no_lahead);

    ///

    cg()->moduleBuilder()->pushBuilder(have_lahead);

    // We have a look-ahead symbol, but its value must match what we expect.
    cg()->builder()->addComment("Look-ahead symbol pending, checking ...");
    cg()->builder()->addInstruction(cond, hilti::instruction::integer::Equal, token_id, state()->lahead);

    auto wrong_lahead = cg()->moduleBuilder()->pushBuilder("wrong-lahead");
    _hiltiParseError("unexpected look-ahead symbol pending");
    cg()->moduleBuilder()->popBuilder();

    cg()->builder()->addInstruction(hilti::instruction::flow::IfElse, cond, done->block(), wrong_lahead->block());

    cg()->moduleBuilder()->pushBuilder(done);

    // Extract token value.
    auto token = cg()->moduleBuilder()->addTmp("token", hilti::builder::reference::type(hilti::builder::bytes::type()));
    cg()->builder()->addInstruction(token, hilti::instruction::bytes::Sub, state()->lahstart, state()->cur);

    // Consume look-ahead.
    cg()->builder()->addInstruction(state()->lahead, hilti::instruction::operator_::Assign, _hiltiLookAheadNone());
    cg()->builder()->addInstruction(hilti::instruction::operator_::Clear, state()->lahstart);
    cg()->builder()->addInstruction(hilti::instruction::operator_::Clear, mresult);
    cg()->builder()->addInstruction(hilti::instruction::operator_::Clear, mstate);

    _newValueForField(field, token);
    _finishedProduction(l->sharedPtr<Production>());

    // Leave builder on stack.
}

void ParserBuilder::visit(production::LookAhead* l)
{
    _startingProduction(l->sharedPtr<Production>(), nullptr);

    _finishedProduction(l->sharedPtr<Production>());
}

void ParserBuilder::visit(production::NonTerminal* n)
{
}

void ParserBuilder::visit(production::Sequence* s)
{
    _startingProduction(s->sharedPtr<Production>(), nullptr);

    for ( auto p : s->sequence() )
        processOne(p);

    _finishedProduction(s->sharedPtr<Production>());
}

void ParserBuilder::visit(production::Switch* s)
{
}

void ParserBuilder::visit(production::Terminal* t)
{
}

void ParserBuilder::visit(production::Variable* v)
{
    auto field = v->pgMeta()->field;
    assert(field);

    _startingProduction(v->sharedPtr<Production>(), field);

    cg()->builder()->addComment(util::fmt("Variable: %s", field->id()->name()));

    shared_ptr<hilti::Expression> value;
    processOne(v->type(), &value, field);

    _newValueForField(field, value);
    _finishedProduction(v->sharedPtr<Production>());
}

void ParserBuilder::visit(production::While* w)
{
    // TODO: Unclear if we need this, but if so, borrow code from
    // production::Until below.
}

#if 0

// Looks like we don't need this, but may reuse the code for the While (if we need that one ...)

void ParserBuilder::visit(production::Until* w)
{
    auto field = w->pgMeta()->field;
    assert(field);

    _startingProduction(w->sharedPtr<Production>(), field);

    auto done = cg()->moduleBuilder()->newBuilder("until-done");
    auto cont = cg()->moduleBuilder()->newBuilder("until-cont");

    auto loop = cg()->moduleBuilder()->pushBuilder("until-loop");

    disableStoringValues();
    processOne(w->body());
    enableStoringValues();

    auto cond = cg()->hiltiExpression(w->expression(), std::make_shared<type::Bool>());

    cg()->builder()->addInstruction(hilti::instruction::flow::IfElse, cond, done->block(), cont->block());
    cg()->moduleBuilder()->popBuilder(loop);

    cg()->moduleBuilder()->pushBuilder(cont);
    assert(_last_parsed_value);
    auto stop = _hiltiRunHook(_hookForItem(state()->unit, field, true), true, _last_parsed_value);
    cg()->builder()->addInstruction(hilti::instruction::flow::IfElse, stop, done->block(), loop->block());
    cg()->moduleBuilder()->popBuilder(cont);

    // Run foreach hook.

    cg()->moduleBuilder()->pushBuilder(done);

    _newValueForField(field, nullptr);
    _finishedProduction(w->sharedPtr<Production>());

    // Leave builder on stack.
}

#endif

void ParserBuilder::visit(production::Loop* l)
{
    auto field = l->pgMeta()->field;
    assert(field);

    _startingProduction(l->sharedPtr<Production>(), field);

    auto done = cg()->moduleBuilder()->newBuilder("until-done");
    auto loop = cg()->moduleBuilder()->pushBuilder("until-loop");

    disableStoringValues();
    processOne(l->body());
    enableStoringValues();

    // Run foreach hook.
    assert(_last_parsed_value);
    auto stop = _hiltiRunHook(_hookForItem(state()->unit, field, true), true, _last_parsed_value);
    cg()->builder()->addInstruction(hilti::instruction::flow::IfElse, stop, done->block(), loop->block());
    cg()->moduleBuilder()->popBuilder(loop);

    cg()->moduleBuilder()->pushBuilder(done);

    _newValueForField(field, nullptr);
    _finishedProduction(l->sharedPtr<Production>());

    // Leave builder on stack.
}

void ParserBuilder::visit(type::Address* a)
{
}

void ParserBuilder::visit(type::Bitset* b)
{
}

void ParserBuilder::visit(type::Bool* b)
{
}

void ParserBuilder::visit(type::Bytes* b)
{
    auto field = arg1();
    auto rtype = hilti::builder::tuple::type({ _hiltiTypeBytes(), _hiltiTypeIteratorBytes() });
    auto result = cg()->builder()->addTmp("unpacked", rtype, nullptr, true);

    auto end = cg()->builder()->addTmp("end", _hiltiTypeIteratorBytes(), nullptr, true);
    cg()->builder()->addInstruction(end, hilti::instruction::iterBytes::End, state()->data);

    auto iters = hilti::builder::tuple::create({ state()->cur, end });

    auto len = field->attributes()->lookup("length");

    if ( len ) {
        auto op1 = iters;
        auto op2 = hilti::builder::id::create("Hilti::Packed::BytesFixed");
        auto op3 = cg()->hiltiExpression(len->value());
        cg()->builder()->addInstruction(result, hilti::instruction::operator_::Unpack, op1, op2, op3);
    }

    else
        internalError(b, "unknown unpack format in type::Bytes");

    auto result_val = cg()->builder()->addTmp("unpacked_val", _hiltiTypeBytes(), nullptr, true);
    cg()->builder()->addInstruction(result_val, hilti::instruction::tuple::Index, result, hilti::builder::integer::create(0));
    cg()->builder()->addInstruction(state()->cur, hilti::instruction::tuple::Index, result, hilti::builder::integer::create(1));

    setResult(result_val);
}

void ParserBuilder::visit(type::Double* d)
{
}

void ParserBuilder::visit(type::Enum* e)
{
}

void ParserBuilder::visit(type::Integer* i)
{
    auto field = arg1();
    auto itype = hilti::builder::integer::type(i->width());
    auto rtype = hilti::builder::tuple::type({ itype, _hiltiTypeIteratorBytes() });
    auto result = cg()->builder()->addTmp("unpacked", rtype, nullptr, true);

    auto end = cg()->builder()->addTmp("end", _hiltiTypeIteratorBytes(), nullptr, true);
    cg()->builder()->addInstruction(end, hilti::instruction::iterBytes::End, state()->data);

    auto iters = hilti::builder::tuple::create({ state()->cur, end });

    auto byteorder = _fieldByteOrder(field, state()->unit);
    auto fmt = _hiltiIntUnpackFormat(i->width(), i->signed_(), byteorder);

    cg()->builder()->addInstruction(result, hilti::instruction::operator_::Unpack, iters, fmt);

    auto result_val = cg()->builder()->addTmp("unpacked_val", itype, nullptr, true);
    cg()->builder()->addInstruction(result_val, hilti::instruction::tuple::Index, result, hilti::builder::integer::create(0));
    cg()->builder()->addInstruction(state()->cur, hilti::instruction::tuple::Index, result, hilti::builder::integer::create(1));

    setResult(result_val);
}

void ParserBuilder::visit(type::Interval* i)
{
}

void ParserBuilder::visit(type::List* l)
{
}

void ParserBuilder::visit(type::Network* n)
{
}

void ParserBuilder::visit(type::Port* p)
{
}

void ParserBuilder::visit(type::Set* s)
{
}

void ParserBuilder::visit(type::String* s)
{
}

void ParserBuilder::visit(type::Time* t)
{
}

void ParserBuilder::visit(type::Unit* u)
{
}

void ParserBuilder::visit(type::unit::Item* i)
{
}

void ParserBuilder::visit(type::unit::item::GlobalHook* h)
{
}

void ParserBuilder::visit(type::unit::item::Variable* v)
{
}

void ParserBuilder::visit(type::unit::item::Property* v)
{
}

void ParserBuilder::visit(type::unit::item::Field* f)
{
}

void ParserBuilder::visit(type::unit::item::field::Constant* c)
{
}

void ParserBuilder::visit(type::unit::item::field::Ctor* c)
{
}

void ParserBuilder::visit(type::unit::item::field::Switch* s)
{
}

void ParserBuilder::visit(type::unit::item::field::AtomicType* t)
{
}

void ParserBuilder::visit(type::unit::item::field::Unit* t)
{
}

void ParserBuilder::visit(type::unit::item::field::switch_::Case* c)
{
}

void ParserBuilder::visit(type::Vector* v)
{
}

