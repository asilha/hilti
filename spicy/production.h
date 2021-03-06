///
/// Classes to define productions for a Spicy grammar.
///

#ifndef SPICY_PGEN_PRODUCTION_H
#define SPICY_PGEN_PRODUCTION_H

#include <ast/visitor.h>

#include "common.h"
#include "ctor.h"

namespace spicy {

class Grammar;

/// Base class for all grammar productions.
class Production : public Node {
    AST_RTTI
public:
    typedef std::list<shared_ptr<Production>> production_list;

    /// Constructor.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// type: An optional type associated with this production.
    ///
    /// l: Associated location.
    Production(const string& symbol, shared_ptr<Type> type, const Location& l = Location::None);

    /// Returns the symbol asssociated with the production.
    const string& symbol() const;

    /// Returns the type associated with this rule.
    shared_ptr<Type> type() const;

    /// Returns the location associated with the production, or Location::None
    /// if none.
    const Location& location() const;

    /// Returns true if it's possible to derive the production to an Epsilon
    /// production. Note that it doesn't \a always need to do so, just one
    /// possible derivation is sufficient.
    virtual bool nullable() const;

    /// Returns true if running out of data while parsing this production
    /// should not be considered an error.
    ///
    /// Can be overridden by derived classes. The default implmentation
    /// returns true iff the producion is nullable().
    virtual bool eodOk() const;

    /// Returns a readable representation of the production, suitable to
    /// include in error message and debugging output.
    string render() override;

    /// Returns true if this production does not recursively contain other
    /// instructions.
    virtual bool atomic() const = 0;

    /// Returns true if this production is flagged by the user as ok to be
    /// parsed in synchronization mode (i.e., after a parser error, or in DPD
    /// mode, it knows how to find a subsequent point in the input stream
    /// where it can continue normally). This is independent of whether the
    /// production actually supports that.
    bool maySynchronize();

    /// Returns true if this production in principle supports being parsed in
    /// synchronization mode (i.e., after a parser error, or in DPD mode, it
    /// knows how to find a subsequent point in the input stream where it can
    /// continue normally). This is independent of whether the user has
    /// requested to activate that support.
    ///
    /// The default implemementation returns false.
    virtual bool supportsSynchronize();

    /// If this production corresponds to a container's item field, sets the
    /// container.
    void setContainer(shared_ptr<type::unit::item::field::Container> c);

    /// If this production corresponds to a container's item field, returns
    /// the container.
    shared_ptr<type::unit::item::field::Container> container() const;

    /// A struct for the parser generator to associate information with a
    /// production.
    struct ParserGenMeta {
        /// A unit field associated with the production.
        shared_ptr<type::unit::item::Field> field = nullptr;

        /// If the production corresponds to a for-each hook, this stores the
        /// corresponding field.
        shared_ptr<type::unit::item::Field> for_each = nullptr;
    };

    /// Returns a pointer to the production's meta information maintained
    /// during parser generation. The parser generated can manipulate the
    /// fields in there directly as necessary.
    ParserGenMeta* pgMeta();

    ACCEPT_VISITOR_ROOT();

protected:
    friend class Grammar;

    /// Returns a readable representation of the production. Must be
    /// overridden by derived classes and will be called by render() (though
    /// that may chose to skip it and refer to the production just by its
    /// symbol).
    virtual string renderProduction() const = 0;

    /// May attempt to simplify the production's internal representation. The
    /// default does nothing.
    virtual void simplify();

    /// Rename the production.
    void setSymbol(const string& sym);

private:
    shared_ptr<Type> _type;
    shared_ptr<type::unit::item::field::Container> _container;

    string _symbol;
    ParserGenMeta _pgmeta;
    Location _location;
};

namespace production {

/// An empty production.
class Epsilon : public Production {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// l: Associated location.
    Epsilon(const Location& l = Location::None);

    ACCEPT_VISITOR(Production);

protected:
    string renderProduction() const override;
    bool nullable() const override;
    bool atomic() const override;
};

/// Base class for terminals.
class Terminal : public Production {
    AST_RTTI
public:
    typedef void (*filter_func)(); // TODO

    /// Constructor.
    ///
    /// type: An optional type associated with this production.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// expr: An optional expression that will be evaluated after the terminal
    /// has been parsed but before its value is assigned to its destination
    /// variable. If the expression is given, *its* value is actually assigned
    /// to the destination variable instead (and also determined the type of
    /// that). This can be used, e.g., into coerce the parsed value into a
    /// different type.
    ///
    /// filter: An optional function called when a value has been parsed for
    /// this terminatal. The function's return value is then used instead of the
    /// parsed value subsequently.
    ///
    /// l: Associated location.
    Terminal(const string& symbol, shared_ptr<Type> type, shared_ptr<Expression> expr,
             filter_func filter, const Location& l = Location::None);

    /// Returns the filter associated with the terminal.
    filter_func filter() const;

    /// Returns the expression associated with the terminal.
    shared_ptr<Expression> expression() const;

    /// Associates a sink with the terminal.
    ///
    /// sink: An expression refercing the sink that parsed data should be
    /// forwarded to.
    void setSink(shared_ptr<Expression> sink);

    /// Returns the sink associated with the terminal, or null if none.
    shared_ptr<Expression> sink() const;

    ACCEPT_VISITOR(Production);

    /// Returns a unique ID for this terminal. The ID is unique across all
    /// grammars and guaranteed to be positive.
    int tokenID() const;

    /// XXX
    virtual string renderTerminal() const = 0;

protected:
    bool atomic() const override;

private:
    filter_func _filter;
    shared_ptr<Expression> _sink = nullptr;
    shared_ptr<Expression> _expr = nullptr;

    mutable int _id;
    static std::map<string, int> _token_ids;
};


/// Base class for a literal. A literal is anythig which can be directly
/// scanned for as a sequence of bytes.
class Literal : public Terminal {
    AST_RTTI
public:
    typedef ctor::RegExp::pattern_list pattern_list;

    /// Constructor. A literal is a production that can be parsed via one or
    /// more regular expressions.
    ///
    /// type: The literal's type. Can be null of \a expr is given (if so,
    /// it's ignored and the expression's type is taken).
    ///
    /// expr: An optional expression that will be evaluated after the
    /// terminal has been parsed but before its value is assigned to its
    /// destination variable. If the expression is given, *its* value is
    /// actually assigned to the destination variable instead (and also
    /// determined the type of that). This can be used, e.g., into coerce the
    /// parsed value into a different type.
    ///
    /// filter: An optional function called when a value has been parsed for
    /// this terminatal. The function's return value is then used instead of
    /// the parsed value subsequently.
    ///
    /// l: Associated location.
    Literal(const string& symbol, shared_ptr<Type> type, shared_ptr<Expression> expr = nullptr,
            filter_func filter = nullptr, const Location& l = Location::None);

    /// Returns an expression representing the literal.
    virtual shared_ptr<Expression> literal() const = 0;

    /// Returns a set of regular expressions corresponding to the literal.
    virtual pattern_list patterns() const = 0;

    bool supportsSynchronize() override;
    string renderTerminal() const override;

    ACCEPT_VISITOR(Terminal);
};

/// A literal represented by a constant.
class Constant : public Literal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// constant: The constant.
    ///
    /// l: Associated location.
    Constant(const string& symbol, shared_ptr<spicy::Constant> constant,
             shared_ptr<Expression> expr = nullptr, filter_func filter = nullptr,
             const Location& l = Location::None);

    /// Returns the literal.
    shared_ptr<spicy::Constant> constant() const;

    shared_ptr<Expression> literal() const override;
    pattern_list patterns() const override;

    ACCEPT_VISITOR(Literal);

protected:
    string renderProduction() const override;

private:
    shared_ptr<spicy::Constant> _const;
};

/// A literal represented by a ctor.
class Ctor : public Literal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// ctor: The ctor.
    ///
    /// l: Associated location.
    Ctor(const string& symbol, shared_ptr<spicy::Ctor> ctor, shared_ptr<Expression> expr = nullptr,
         filter_func filter = nullptr, const Location& l = Location::None);

    /// Returns the literal.
    shared_ptr<spicy::Ctor> ctor() const;

    shared_ptr<Expression> literal() const override;
    pattern_list patterns() const override;

    ACCEPT_VISITOR(Literal);

protected:
    string renderProduction() const override;

private:
    shared_ptr<spicy::Ctor> _ctor;
};

/// A literal represented by a type. Note that normally types aren't
/// literals; they can only if the parsing can for tell that a instance of
/// the type is coming up, i.e., supports look-ahead (as, e.g., in the case
/// of embedded objects).
class TypeLiteral : public Literal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// type: The type.
    ///
    /// l: Associated location.
    TypeLiteral(const string& symbol, shared_ptr<spicy::Type> type,
                shared_ptr<Expression> expr = nullptr, filter_func filter = nullptr,
                const Location& l = Location::None);

    /// Returns the type.
    shared_ptr<spicy::Type> type() const;

    shared_ptr<Expression> literal() const override;
    pattern_list patterns() const override;

    ACCEPT_VISITOR(Literal);

protected:
    string renderProduction() const override;

private:
    shared_ptr<spicy::Type> _type;
};

/// A variable. A variable is a terminal that will be parsed from the input
/// stream according to its type, yet is not recognizable as such in advance
/// by just looking at the available bytes. If we start parsing, we assume it
/// will match (and if not, generate a parse error).
class Variable : public Terminal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// type: The type of the variable.
    ///
    /// expr: An optional expression that will be evaluated after the terminal
    /// has been parsed but before its value is assigned to its destination
    /// variable. If the expression is given, *its* value is actually assigned
    /// to the destination variable instead (and also determined the type of
    /// that). This can be used, e.g., into coerce the parsed value into a
    /// different type.
    ///
    /// filter: An optional function called when a value has been parsed for
    /// this terminatal. The function's return value is then used instead of the
    /// parsed value subsequently.
    ///
    /// l: Associated location.
    Variable(const string& symbol, shared_ptr<Type> type, shared_ptr<Expression> expr = nullptr,
             filter_func filter = nullptr, const Location& l = Location::None);

    ACCEPT_VISITOR(Terminal);

    string renderTerminal() const override;

protected:
    string renderProduction() const override;
};

/// Base class for non-terminals.
class NonTerminal : public Production {
    AST_RTTI
public:
    typedef std::list<production_list> alternative_list;

    /// Constructor.
    ///
    /// type: An optional type associated with this production.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// l: Associated location.
    NonTerminal(const string& symbol, shared_ptr<Type> type, const Location& l = Location::None);

    /// Returns a list of RHS alternatives for this production. Each RHS is
    /// itself a list of Production instances.
    virtual alternative_list rhss() const = 0;

    ACCEPT_VISITOR(Production);

protected:
    bool atomic() const override;
    bool nullable() const override;
};

/// A type described by another grammar from an independent type::Unit type.
class ChildGrammar : public NonTerminal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// child: The production for the child unit type, corresponding to *child*.
    ///
    /// type: The unit type.
    ///
    /// l: Associated location.
    ChildGrammar(const string& symbol, shared_ptr<Production> child, shared_ptr<type::Unit> type,
                 const Location& l = Location::None);

    /// Returns the child production.
    shared_ptr<Production> child() const;

    /// Returns the child type.
    shared_ptr<type::Unit> childType() const;

#if 0
    typedef std::list<shared_ptr<Expression>> expression_list;

    /// Sets a list of parameters passed into the parser for the child grammar.
    ///
    /// params: The expressions to evaluate to get the parameters.
    void setParameters(const expression_list& params);

    /// Returns a list of parameters to pass into the parser for the child
    /// grammar.
    const expression_list& parameters() const;
#endif

    ACCEPT_VISITOR(NonTerminal);

protected:
    string renderProduction() const override;
    alternative_list rhss() const override;
    bool supportsSynchronize() override;

private:
    node_ptr<Production> _child;
    // expression_list _params;
};

/// A wrapper that forwards directly to another grammar (within the same unit
/// type). This can be used to hook into starting/finishing parsing that
/// other grammar.
class Enclosure : public NonTerminal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// child: The enclosed child production.
    ///
    /// type: The unit type.
    ///
    /// l: Associated location.
    Enclosure(const string& symbol, shared_ptr<Production> child,
              const Location& l = Location::None);

    /// Returns the enclosed child production.
    shared_ptr<Production> child() const;

    ACCEPT_VISITOR(NonTerminal);

protected:
    string renderProduction() const override;
    alternative_list rhss() const override;
    bool supportsSynchronize() override;

private:
    node_ptr<Production> _child;
};

/// A sequence of other productions.
class Sequence : public NonTerminal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// seq: list of ~~Production - The sequence of productions.
    ///
    /// type: An optional type associated with this production.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// l: Associated location.
    ///
    /// .. todo: Does this class need the \a type parameter?
    Sequence(const string& symbol, const production_list& seq, shared_ptr<Type> type = nullptr,
             const Location& l = Location::None);

    /// Returns the production sequence.
    production_list sequence() const;

    /// Adds a production to the sequence.
    ///
    /// prod: The production to add to the end of the sequence.
    void add(shared_ptr<Production> prod);

    ACCEPT_VISITOR(NonTerminal);

protected:
    string renderProduction() const override;
    alternative_list rhss() const override;
    bool supportsSynchronize() override;

private:
    std::list<node_ptr<Production>> _seq;
};

/// A pair of alternatives between which we can decide with one token of
/// look-ahead.
class LookAhead : public NonTerminal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// alt1: The first alternative.
    ///
    /// alt2: The first alternative.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// l: Associated location.
    LookAhead(const string& symbol, shared_ptr<Production> alt1, shared_ptr<Production> alt2,
              const Location& l = Location::None);

    typedef std::set<shared_ptr<Terminal>> look_aheads;

    /// Returns the look-aheads for the two alternatives. This function must
    /// only be called after the instance has been added to a ~Grammar, as
    /// that's when the look-aheads are computed.
    ///
    /// Returns: a pair of sets representing the look-aheads for the first and
    /// second alternative, respectively. Each set contains the ~~Literals for
    /// which the corresponding alternative should be selected.
    std::pair<look_aheads, look_aheads> lookAheads() const;

    /// Returns a boolean for each alternative indicating whether the
    /// corresponding look-ahead set is the default case. Any variable
    /// production is automatically considered a default, others may be
    /// marked as such via setDefaultAlternative().
    std::pair<bool, bool> defaultAlternatives();

    /// Returns the two alternatives.
    std::pair<shared_ptr<Production>, shared_ptr<Production>> alternatives() const;

    /// Sets the two alternatives.
    ///
    /// alt1: The first alternative.
    ///
    /// alt2: The first alternative.
    void setAlternatives(shared_ptr<Production> alt1, shared_ptr<Production> alt2);

    // Marks one of the two alterantives as default. Note that any variable
    // production is automatically considered a default, independent of this
    // setting.
    //
    // i: 1 sets alternative 1 to default, 2 alternative 2; anything set
    // neither to be a default.
    void setDefaultAlternative(int i);

    ACCEPT_VISITOR(NonTerminal);

protected:
    friend class ::spicy::Grammar;

    /// Sets the look-ahead sets for the two alternative. Called from the
    /// Grammar calss when it computes the parsing tables.
    void setLookAheads(const look_aheads& lah1, const look_aheads& lah2);

    string renderProduction() const override;
    alternative_list rhss() const override;
    bool supportsSynchronize() override;

private:
    int _default = 0;
    std::pair<look_aheads, look_aheads> _lahs;
    node_ptr<Production> _alt1;
    node_ptr<Production> _alt2;
};

/// A pair of alternatives between which we decide based on a boolean
/// expression.
class Boolean : public NonTerminal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// expr: An expression of type type::Bool.
    ///
    /// alt1: The first alternative to select if \a expr is \a true.
    ///
    /// alt2: The second alternative to select if \a expr is \a false.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// l: Associated location.
    Boolean(const string& symbol, shared_ptr<Expression> expr, shared_ptr<Production> alt1,
            shared_ptr<Production> alt2, const Location& l = Location::None);

    /// Returns the expression associated with the boolean production.
    shared_ptr<Expression> expression() const;

    /// Returns the two alternatives as a tuple. The first element is for the
    /// \a true case, the second for \a false.
    std::pair<shared_ptr<Production>, shared_ptr<Production>> branches() const;

    bool eodOk() const override;

    ACCEPT_VISITOR(NonTerminal);

protected:
    string renderProduction() const override;
    alternative_list rhss() const override;

private:
    shared_ptr<Expression> _expr;
    node_ptr<Production> _alt1;
    node_ptr<Production> _alt2;
};

/// A production executing a given number of times.
class Counter : public NonTerminal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// expr: An expression of an integer type that limits the number of times
    /// the production is parsed.
    ///
    /// body: The production to be repeated.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// l: Associated location.
    Counter(const string& symbol, shared_ptr<Expression> expr, shared_ptr<Production> body,
            const Location& l = Location::None);

    /// Destructor.
    virtual ~Counter();

    /// Returns the counter expression.
    shared_ptr<spicy::Expression> expression() const;

    /// Returns the counter body production.
    shared_ptr<Production> body() const;

    ACCEPT_VISITOR(NonTerminal);

protected:
    string renderProduction() const override;
    alternative_list rhss() const override;

private:
    shared_ptr<Expression> _expr;
    node_ptr<Production> _body;
};

/// A production that parses a byte block of a given length with another production.
class ByteBlock : public NonTerminal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// expr: An expression of an integer type that specifies the number of
    /// bytes to parse.
    ///
    /// body: The production to be parse the byte block with.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// l: Associated location.
    ByteBlock(const string& symbol, shared_ptr<Expression> expr, shared_ptr<Production> body,
              const Location& l = Location::None);

    /// Returns the length expression.
    shared_ptr<spicy::Expression> expression() const;

    /// Returns the counter body production.
    shared_ptr<Production> body() const;

    ACCEPT_VISITOR(NonTerminal);

protected:
    string renderProduction() const override;
    alternative_list rhss() const override;

private:
    shared_ptr<Expression> _expr;
    node_ptr<Production> _body;
};

/// A production executing as long as condition is true.
class While : public NonTerminal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// expr: An expression of type ~~Bool controlling the loop.
    ///
    /// body: The production to be repeated.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// l: Associated location.
    While(const string& symbol, shared_ptr<Expression> expr, shared_ptr<Production> body,
          const Location& l = Location::None);

    /// Returns the condition expression.
    shared_ptr<Expression> expression() const;

    /// Returns the loop body production.
    shared_ptr<Production> body() const;

    ACCEPT_VISITOR(NonTerminal);

protected:
    string renderProduction() const override;
    alternative_list rhss() const override;
    bool supportsSynchronize() override;

private:
    shared_ptr<Expression> _expr;
    node_ptr<Production> _body;
};

/// A production executing until interrupted by a foreach hook.
class Loop : public NonTerminal {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// body: The production to be repeated.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// eod_ok: True if running out of data while being about to enter the
    /// next iteration is ok.
    ///
    /// l: Associated location.
    Loop(const string& symbol, shared_ptr<Production> body, bool eod_ok,
         const Location& l = Location::None);

    /// Returns the loop body production.
    shared_ptr<Production> body() const;

    bool eodOk() const override;

    ACCEPT_VISITOR(NonTerminal);

protected:
    string renderProduction() const override;
    alternative_list rhss() const override;
    bool supportsSynchronize() override;

private:
    node_ptr<Production> _body;
    bool _eod_ok;
};

/// Alternatives between which we decide based on which value out of a set of
/// options is matched; plus a default if none.
class Switch : public NonTerminal {
    AST_RTTI
public:
    typedef std::list<std::pair<std::list<shared_ptr<Expression>>, shared_ptr<Production>>>
        case_list;

    /// Constructor.
    ///
    /// expr: An expression to switch on.
    ///
    /// cases: The alternatives. The value of \a *expr* is matched against the
    /// evaluated values of the expressions given in the list (which must be
    /// of the same type); if a match is found the corresponding production is
    /// selected.
    ///
    /// default_: Default production if none of \a cases matches. If None, a
    /// ~~ParseError will be generated if no other alternative matches.
    ///
    /// symbol: A symbol associated with the production. The symbol must be
    /// unique within the grammar the production is (or will be) part of.
    ///
    /// l: Associated location.
    Switch(const string& symbol, shared_ptr<Expression> expr, const case_list& cases,
           shared_ptr<Production> default_, const Location& l = Location::None);

    /// Returns the switch expression.
    shared_ptr<Expression> expression() const;

    /// Returns the alternatives.
    case_list alternatives() const;

    /// Returns the default production, or null if none.
    shared_ptr<Production> default_() const;

    bool eodOk() const override;

    ACCEPT_VISITOR(NonTerminal);

protected:
    string renderProduction() const override;
    alternative_list rhss() const override;

private:
    std::list<std::pair<std::list<shared_ptr<Expression>>, node_ptr<Production>>> _cases;
    shared_ptr<Expression> _expr;
    node_ptr<Production> _default;
};

/// An internal class representing a production to be resolved later. This is
/// for supporting recursive grammars.
class Unknown : public Production {
    AST_RTTI
public:
    /// Constructor.
    ///
    /// node: The node with which's production this unknown is to be replaced with later.
    Unknown(shared_ptr<Node> node);

    /// Returns the node that needs resolving.
    shared_ptr<Node> node() const;

    bool atomic() const override;

    ACCEPT_VISITOR(Production);

protected:
    string renderProduction() const override;

private:
    shared_ptr<Node> _node;
};
}
}

#endif
