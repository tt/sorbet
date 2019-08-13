#include "core/errors/resolver.h"
#include "ast/Helpers.h"
#include "ast/Trees.h"
#include "ast/ast.h"
#include "ast/treemap/treemap.h"
#include "core/Error.h"
#include "core/Names.h"
#include "core/StrictLevel.h"
#include "core/core.h"
#include "resolver/resolver.h"
#include "resolver/type_syntax.h"

#include "absl/strings/str_cat.h"
#include "common/Timer.h"
#include "core/Symbols.h"
#include <utility>
#include <vector>

using namespace std;

namespace sorbet::resolver {
namespace {

/*
 * Note: There are multiple separate tree walks defined in this file, the main
 * ones being:
 *
 * - ResolveConstantsWalk
 * - ResolveSignaturesWalk
 *
 * There are also other important parts of resolver found elsewhere in the
 * resolver/ package (GlobalPass, type_syntax). Below we describe
 * ResolveConstantsWalk, which is particularly sophisticated.
 *
 *                                - - - - -
 *
 * Ruby supports resolving constants via ancestors--superclasses and mixins.
 * Since superclass and mixins are themselves constant references, we thus may
 * not be able to resolve certain constants until after we've resolved others.
 *
 * To solve this, we collect any failed resolutions in a number of TODO lists,
 * and iterate over them to a fixed point (namely, either all constants
 * resolve, or no new constants resolve and we stub out any that remain).
 * In practice this fixed point computation terminates after 3 or fewer passes
 * on most real codebases.
 *
 * The four TODO lists that this loop maintains are:
 *
 *  - constants to be resolved
 *  - ancestors to be filled that require constants to be resolved
 *  - class aliases (class aliases know the symbol they alias to)
 *  - type aliases (type aliases know the fully parsed type of their RHS, and
 *    thus require their RHS to be resolved)
 *
 * Successful resolutions are removed from the lists, and then we loop again.
 * We track all these lists separately for the dual reasons that
 *
 * 1. Upon successful resolution, we need to do additional work (mutating the
 *    symbol table to reflect the new ancestors) and
 * 2. Resolving those constants potentially renders additional constants
 *    resolvable, and so if any resolution succeeds, we need to keep looping in
 *    the outer loop.
 *
 * After this pass:
 *
 * - ast::UnresolvedConstantLit nodes (constants that have a NameRef) are
 *   replaced with ast::ConstantLit nodes (constants that have a SymbolRef).
 * - Every constant SymbolRef has enough to completely understand it's own
 *   place in the ancestor hierarchy.
 * - Every type alias symbol carries with it the type it should be treated as.
 *
 * The resolveConstants method is the best place to start if you want to browse
 * the fixed point loop at a high level.
 */

class ResolveConstantsWalk {
    friend class ResolveSanityCheckWalk;

private:
    struct Nesting {
        const shared_ptr<Nesting> parent;
        const core::SymbolRef scope;

        Nesting(shared_ptr<Nesting> parent, core::SymbolRef scope) : parent(std::move(parent)), scope(scope) {}
    };
    shared_ptr<Nesting> nesting_;

    struct ResolutionItem {
        shared_ptr<Nesting> scope;
        ast::ConstantLit *out;

        ResolutionItem() = default;
        ResolutionItem(ResolutionItem &&rhs) noexcept = default;
        ResolutionItem &operator=(ResolutionItem &&rhs) noexcept = default;

        ResolutionItem(const ResolutionItem &rhs) = delete;
        const ResolutionItem &operator=(const ResolutionItem &rhs) = delete;
    };
    struct AncestorResolutionItem {
        ast::ConstantLit *ancestor;
        core::SymbolRef klass;

        bool isSuperclass; // true if superclass, false for mixin

        AncestorResolutionItem() = default;
        AncestorResolutionItem(AncestorResolutionItem &&rhs) noexcept = default;
        AncestorResolutionItem &operator=(AncestorResolutionItem &&rhs) noexcept = default;

        AncestorResolutionItem(const AncestorResolutionItem &rhs) = delete;
        const AncestorResolutionItem &operator=(const AncestorResolutionItem &rhs) = delete;
    };

    struct ClassAliasResolutionItem {
        core::SymbolRef lhs;
        ast::ConstantLit *rhs;

        ClassAliasResolutionItem() = default;
        ClassAliasResolutionItem(ClassAliasResolutionItem &&) noexcept = default;
        ClassAliasResolutionItem &operator=(ClassAliasResolutionItem &&rhs) noexcept = default;

        ClassAliasResolutionItem(const ClassAliasResolutionItem &) = delete;
        const ClassAliasResolutionItem &operator=(const ClassAliasResolutionItem &) = delete;
    };

    struct TypeAliasResolutionItem {
        core::SymbolRef lhs;
        ast::Expression *rhs;

        TypeAliasResolutionItem(core::SymbolRef lhs, ast::Expression *rhs) : lhs(lhs), rhs(rhs) {}
        TypeAliasResolutionItem(TypeAliasResolutionItem &&) noexcept = default;
        TypeAliasResolutionItem &operator=(TypeAliasResolutionItem &&rhs) noexcept = default;

        TypeAliasResolutionItem(const TypeAliasResolutionItem &) = delete;
        const TypeAliasResolutionItem &operator=(const TypeAliasResolutionItem &) = delete;
    };

    vector<ResolutionItem> todo_;
    vector<AncestorResolutionItem> todoAncestors_;
    vector<ClassAliasResolutionItem> todoClassAliases_;
    vector<TypeAliasResolutionItem> todoTypeAliases_;

    static core::SymbolRef resolveLhs(core::Context ctx, shared_ptr<Nesting> nesting, core::NameRef name) {
        Nesting *scope = nesting.get();
        while (scope != nullptr) {
            auto lookup = scope->scope.data(ctx)->findMember(ctx, name);
            if (lookup.exists()) {
                return lookup;
            }
            scope = scope->parent.get();
        }
        return nesting->scope.data(ctx)->findMemberTransitive(ctx, name);
    }

    static bool isAlreadyResolved(core::Context ctx, const ast::ConstantLit &original) {
        auto sym = original.symbol;
        if (!sym.exists()) {
            return false;
        }
        auto data = sym.data(ctx);
        if (data->isTypeAlias()) {
            return data->resultType != nullptr;
        } else {
            return true;
        }
    }

    class ResolutionChecker {
    public:
        bool seenUnresolved = false;

        unique_ptr<ast::ConstantLit> postTransformConstantLit(core::Context ctx,
                                                              unique_ptr<ast::ConstantLit> original) {
            seenUnresolved |= !isAlreadyResolved(ctx, *original);
            return original;
        };
    };

    static bool isFullyResolved(core::Context ctx, const ast::Expression *expression) {
        ResolutionChecker checker;
        unique_ptr<ast::Expression> dummy(const_cast<ast::Expression *>(expression));
        dummy = ast::TreeMap::apply(ctx, checker, std::move(dummy));
        ENFORCE(dummy.get() == expression);
        dummy.release();
        return !checker.seenUnresolved;
    }

    static core::SymbolRef resolveConstant(core::Context ctx, shared_ptr<Nesting> nesting,
                                           const unique_ptr<ast::UnresolvedConstantLit> &c) {
        if (ast::isa_tree<ast::EmptyTree>(c->scope.get())) {
            core::SymbolRef result = resolveLhs(ctx, nesting, c->cnst);
            return result;
        }
        ast::Expression *resolvedScope = c->scope.get();
        if (auto *id = ast::cast_tree<ast::ConstantLit>(resolvedScope)) {
            auto sym = id->symbol;
            if (sym.exists() && sym.data(ctx)->isTypeAlias()) {
                if (auto e = ctx.state.beginError(c->loc, core::errors::Resolver::ConstantInTypeAlias)) {
                    e.setHeader("Resolving constants through type aliases is not supported");
                }
                return core::Symbols::untyped();
            }
            if (!id->symbol.exists()) {
                // TODO: try to resolve if not resolved.
                return core::Symbols::noSymbol();
            }
            core::SymbolRef resolved = id->symbol.data(ctx)->dealias(ctx);
            core::SymbolRef result = resolved.data(ctx)->findMember(ctx, c->cnst);
            return result;
        } else {
            if (auto e = ctx.state.beginError(c->loc, core::errors::Resolver::DynamicConstant)) {
                e.setHeader("Dynamic constant references are unsupported");
            }
            return core::Symbols::untyped();
        }
    }

    // We have failed to resolve the constant. We'll need to report the error and stub it so that we can proceed
    static void constantResolutionFailed(core::MutableContext ctx, ResolutionItem &job) {
        auto resolved = resolveConstant(ctx.withOwner(job.scope->scope), job.scope, job.out->original);
        if (resolved.exists() && resolved.data(ctx)->isTypeAlias()) {
            if (resolved.data(ctx)->resultType == nullptr) {
                // This is actually a use-site error, but we limit ourselves to emitting it once by checking resultType
                auto loc = resolved.data(ctx)->loc();
                if (auto e = ctx.state.beginError(loc, core::errors::Resolver::RecursiveTypeAlias)) {
                    e.setHeader("Unable to resolve right hand side of type alias `{}`", resolved.data(ctx)->show(ctx));
                    e.addErrorLine(job.out->original->loc, "Type alias used here");
                }
                resolved.data(ctx)->resultType =
                    core::Types::untyped(ctx, resolved); // <<-- This is the reason this takes a MutableContext
            }
            job.out->symbol = resolved;
            return;
        }
        ENFORCE(!resolved.exists());

        core::SymbolRef scope;
        if (job.out->symbol.exists()) {
            scope = job.out->symbol.data(ctx)->dealias(ctx);
        } else if (auto *id = ast::cast_tree<ast::ConstantLit>(job.out->original->scope.get())) {
            scope = id->symbol.data(ctx)->dealias(ctx);
        } else {
            scope = job.scope->scope;
        }

        auto customAutogenError = job.out->original->cnst == core::Symbols::Subclasses().data(ctx)->name;
        if (scope != core::Symbols::StubModule() || customAutogenError) {
            if (auto e = ctx.state.beginError(job.out->original->loc, core::errors::Resolver::StubConstant)) {
                e.setHeader("Unable to resolve constant `{}`", job.out->original->cnst.show(ctx));

                if (customAutogenError) {
                    e.addErrorSection(
                        core::ErrorSection("If this constant is generated by Autogen, you "
                                           "may need to re-generate the .rbi. Try running:\n"
                                           "  scripts/bin/remote-script sorbet/shim_generation/autogen.rb"));
                } else if (scope.data(ctx)->isClass()) {
                    auto suggested = scope.data(ctx)->findMemberFuzzyMatch(ctx, job.out->original->cnst);
                    if (suggested.size() > 3) {
                        suggested.resize(3);
                    }
                    if (!suggested.empty()) {
                        vector<core::ErrorLine> lines;
                        for (auto suggestion : suggested) {
                            lines.emplace_back(core::ErrorLine::from(suggestion.symbol.data(ctx)->loc(),
                                                                     "Did you mean: `{}`?",
                                                                     suggestion.symbol.show(ctx)));
                        }
                        e.addErrorSection(core::ErrorSection(lines));
                    }
                }
            }
        }

        if (scope == core::Symbols::StubModule()) {
            scope = core::Symbols::noSymbol();
        }

        job.out->symbol = core::Symbols::StubModule();
        job.out->resolutionScope = scope;
    }

    static bool resolveJob(core::Context ctx, ResolutionItem &job) {
        if (isAlreadyResolved(ctx, *job.out)) {
            return true;
        }
        auto resolved = resolveConstant(ctx.withOwner(job.scope->scope), job.scope, job.out->original);
        if (!resolved.exists()) {
            return false;
        }
        if (resolved.data(ctx)->isTypeAlias()) {
            if (resolved.data(ctx)->resultType != nullptr) {
                // A TypeAliasResolutionItem job completed successfully,
                // or we forced the type alias this constant refers to to resolve.
                job.out->symbol = resolved;
                return true;
            }
            return false;
        }

        job.out->symbol = resolved;
        return true;
    }

    static bool resolveTypeAliasJob(core::MutableContext ctx, TypeAliasResolutionItem &job) {
        core::SymbolRef enclosingTypeMember;
        core::SymbolRef enclosingClass = job.lhs.data(ctx)->enclosingClass(ctx);
        while (enclosingClass != core::Symbols::root()) {
            auto typeMembers = enclosingClass.data(ctx)->typeMembers();
            if (!typeMembers.empty()) {
                enclosingTypeMember = typeMembers[0];
                break;
            }
            enclosingClass = enclosingClass.data(ctx)->owner.data(ctx)->enclosingClass(ctx);
        }
        if (enclosingTypeMember.exists()) {
            if (auto e = ctx.state.beginError(job.rhs->loc, core::errors::Resolver::TypeAliasInGenericClass)) {
                e.setHeader("Type aliases are not allowed in generic classes");
                e.addErrorLine(enclosingTypeMember.data(ctx)->loc(), "Here is enclosing generic member");
            }
            job.lhs.data(ctx)->resultType = core::Types::untyped(ctx, job.lhs);
            return true;
        }
        if (isFullyResolved(ctx, job.rhs)) {
            auto allowSelfType = true;
            auto allowRebind = false;
            auto allowTypeMember = true;
            job.lhs.data(ctx)->resultType = TypeSyntax::getResultType(
                ctx, *(job.rhs), ParsedSig{}, TypeSyntaxArgs{allowSelfType, allowRebind, allowTypeMember, job.lhs});
            return true;
        }

        return false;
    }

    static bool resolveClassAliasJob(core::MutableContext ctx, ClassAliasResolutionItem &it) {
        auto rhsSym = it.rhs->symbol;
        if (!rhsSym.exists()) {
            return false;
        }

        auto rhsData = rhsSym.data(ctx);
        if (rhsData->isTypeAlias()) {
            if (auto e = ctx.state.beginError(it.rhs->loc, core::errors::Resolver::ReassignsTypeAlias)) {
                e.setHeader("Reassigning a type alias is not allowed");
                e.addErrorLine(rhsData->loc(), "Originally defined here");
                e.replaceWith("Declare as type alias", it.rhs->loc, "T.type_alias({})", it.rhs->loc.source(ctx));
            }
            it.lhs.data(ctx)->resultType = core::Types::untypedUntracked();
            return true;
        } else {
            if (rhsData->dealias(ctx) != it.lhs) {
                it.lhs.data(ctx)->resultType = core::make_type<core::AliasType>(rhsSym);
            } else {
                if (auto e =
                        ctx.state.beginError(it.lhs.data(ctx)->loc(), core::errors::Resolver::RecursiveClassAlias)) {
                    e.setHeader("Class alias aliases to itself");
                }
                it.lhs.data(ctx)->resultType = core::Types::untypedUntracked();
            }
            return true;
        }
    }

    static core::SymbolRef stubSymbolForAncestor(const AncestorResolutionItem &item) {
        if (item.isSuperclass) {
            return core::Symbols::StubSuperClass();
        } else {
            return core::Symbols::StubMixin();
        }
    }

    static bool resolveAncestorJob(core::MutableContext ctx, AncestorResolutionItem &job, bool lastRun) {
        auto ancestorSym = job.ancestor->symbol;
        if (!ancestorSym.exists()) {
            return false;
        }

        core::SymbolRef resolved;
        if (ancestorSym.data(ctx)->isTypeAlias()) {
            if (!lastRun) {
                return false;
            }
            if (auto e = ctx.state.beginError(job.ancestor->loc, core::errors::Resolver::DynamicSuperclass)) {
                e.setHeader("Superclasses and mixins may not be type aliases");
            }
            resolved = stubSymbolForAncestor(job);
        } else {
            resolved = ancestorSym.data(ctx)->dealias(ctx);
        }

        if (!resolved.data(ctx)->isClass()) {
            if (!lastRun) {
                return false;
            }
            if (auto e = ctx.state.beginError(job.ancestor->loc, core::errors::Resolver::DynamicSuperclass)) {
                e.setHeader("Superclasses and mixins may only use class aliases like `{}`", "A = Integer");
            }
            resolved = stubSymbolForAncestor(job);
        }

        if (resolved == job.klass) {
            if (auto e = ctx.state.beginError(job.ancestor->loc, core::errors::Resolver::CircularDependency)) {
                e.setHeader("Circular dependency: `{}` is a parent of itself", job.klass.data(ctx)->show(ctx));
                e.addErrorLine(resolved.data(ctx)->loc(), "Class definition");
            }
            resolved = stubSymbolForAncestor(job);
        } else if (resolved.data(ctx)->derivesFrom(ctx, job.klass)) {
            if (auto e = ctx.state.beginError(job.ancestor->loc, core::errors::Resolver::CircularDependency)) {
                e.setHeader("Circular dependency: `{}` and `{}` are declared as parents of each other",
                            job.klass.data(ctx)->show(ctx), resolved.data(ctx)->show(ctx));
                e.addErrorLine(job.klass.data(ctx)->loc(), "One definition");
                e.addErrorLine(resolved.data(ctx)->loc(), "Other definition");
            }
            resolved = stubSymbolForAncestor(job);
        }

        if (job.isSuperclass) {
            if (resolved == core::Symbols::todo()) {
                // No superclass specified
            } else if (!job.klass.data(ctx)->superClass().exists() ||
                       job.klass.data(ctx)->superClass() == core::Symbols::todo() ||
                       job.klass.data(ctx)->superClass() == resolved) {
                job.klass.data(ctx)->setSuperClass(resolved);
            } else {
                if (auto e = ctx.state.beginError(job.ancestor->loc, core::errors::Resolver::RedefinitionOfParents)) {
                    e.setHeader("Class parents redefined for class `{}`", job.klass.data(ctx)->show(ctx));
                }
            }
        } else {
            ENFORCE(resolved.data(ctx)->isClass());
            job.klass.data(ctx)->mixins().emplace_back(resolved);
        }

        return true;
    }

    static void tryRegisterSealedSubclass(core::MutableContext ctx, AncestorResolutionItem &job) {
        ENFORCE(job.ancestor->symbol.exists(), "Ancestor must exist, or we can't check whether it's sealed.");
        auto ancestorSym = job.ancestor->symbol.data(ctx)->dealias(ctx);

        if (!ancestorSym.data(ctx)->isClassSealed()) {
            return;
        }

        // TODO(jez) Would it ever make sense to put an AppliedType into the union?
        // TODO(jez) Do we want to make sure that the child class doesn't have any type members?

        ancestorSym.data(ctx)->recordSealedSubclass(ctx, job.klass);
    }

    void transformAncestor(core::Context ctx, core::SymbolRef klass, unique_ptr<ast::Expression> &ancestor,
                           bool isSuperclass = false) {
        if (auto *constScope = ast::cast_tree<ast::UnresolvedConstantLit>(ancestor.get())) {
            unique_ptr<ast::UnresolvedConstantLit> inner(constScope);
            ancestor.release();
            auto scopeTmp = nesting_;
            if (isSuperclass) {
                nesting_ = nesting_->parent;
            }
            ancestor = postTransformUnresolvedConstantLit(ctx, std::move(inner));
            nesting_ = scopeTmp;
        }
        AncestorResolutionItem job;
        job.klass = klass;
        job.isSuperclass = isSuperclass;

        if (auto *cnst = ast::cast_tree<ast::ConstantLit>(ancestor.get())) {
            auto sym = cnst->symbol;
            if (sym.exists() && sym.data(ctx)->isTypeAlias()) {
                if (auto e = ctx.state.beginError(cnst->loc, core::errors::Resolver::DynamicSuperclass)) {
                    e.setHeader("Superclasses and mixins may not be type aliases");
                }
                return;
            }
            ENFORCE(sym.exists() || ast::isa_tree<ast::ConstantLit>(cnst->original->scope.get()) ||
                    ast::isa_tree<ast::EmptyTree>(cnst->original->scope.get()));
            if (isSuperclass && sym == core::Symbols::todo()) {
                return;
            }
            job.ancestor = cnst;
        } else if (ancestor->isSelfReference()) {
            auto loc = ancestor->loc;
            auto enclosingClass = ctx.owner.data(ctx)->enclosingClass(ctx);
            auto nw = make_unique<ast::UnresolvedConstantLit>(loc, std::move(ancestor), enclosingClass.data(ctx)->name);
            auto out = make_unique<ast::ConstantLit>(loc, enclosingClass, std::move(nw));
            job.ancestor = out.get();
            ancestor = std::move(out);
        } else if (ast::isa_tree<ast::EmptyTree>(ancestor.get())) {
            return;
        } else {
            ENFORCE(false, "Namer should have not allowed this");
        }

        todoAncestors_.emplace_back(std::move(job));
    }

public:
    ResolveConstantsWalk(core::Context ctx) : nesting_(make_unique<Nesting>(nullptr, core::Symbols::root())) {}

    unique_ptr<ast::ClassDef> preTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> original) {
        nesting_ = make_unique<Nesting>(std::move(nesting_), original->symbol);
        return original;
    }

    unique_ptr<ast::Expression> postTransformUnresolvedConstantLit(core::Context ctx,
                                                                   unique_ptr<ast::UnresolvedConstantLit> c) {
        if (auto *constScope = ast::cast_tree<ast::UnresolvedConstantLit>(c->scope.get())) {
            unique_ptr<ast::UnresolvedConstantLit> inner(constScope);
            c->scope.release();
            c->scope = postTransformUnresolvedConstantLit(ctx, std::move(inner));
        }
        auto loc = c->loc;
        auto out = make_unique<ast::ConstantLit>(loc, core::Symbols::noSymbol(), std::move(c));
        ResolutionItem job{nesting_, out.get()};
        if (resolveJob(ctx, job)) {
            categoryCounterInc("resolve.constants.nonancestor", "firstpass");
        } else {
            todo_.emplace_back(std::move(job));
        }
        return out;
    }

    unique_ptr<ast::Expression> postTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> original) {
        core::SymbolRef klass = original->symbol;

        for (auto &ancst : original->ancestors) {
            bool isSuperclass = (original->kind == ast::Class && &ancst == &original->ancestors.front() &&
                                 !klass.data(ctx)->isSingletonClass(ctx));
            transformAncestor(isSuperclass ? ctx : ctx.withOwner(klass), klass, ancst, isSuperclass);
        }

        auto singleton = klass.data(ctx)->lookupSingletonClass(ctx);
        for (auto &ancst : original->singletonAncestors) {
            ENFORCE(singleton.exists());
            transformAncestor(ctx.withOwner(klass), singleton, ancst);
        }

        nesting_ = nesting_->parent;
        return original;
    }

    unique_ptr<ast::Expression> postTransformAssign(core::Context ctx, unique_ptr<ast::Assign> asgn) {
        auto *id = ast::cast_tree<ast::ConstantLit>(asgn->lhs.get());
        if (id == nullptr || !id->symbol.dataAllowingNone(ctx)->isStaticField()) {
            return asgn;
        }

        auto *send = ast::cast_tree<ast::Send>(asgn->rhs.get());
        if (send != nullptr && send->fun == core::Names::typeAlias()) {
            if (send->args.size() == 0) {
                // if we have an invalid (i.e. nullary) call to TypeAlias, then we'll treat it as a type alias for
                // Untyped and report an error here: otherwise, we end up in a state at the end of constant resolution
                // that won't match our expected invariants (and in fact will fail our sanity checks)
                auto temporaryUntyped = ast::MK::Untyped(asgn->lhs.get()->loc);
                send->args.emplace_back(std::move(temporaryUntyped));

                // because we're synthesizing a fake "untyped" here and actually adding it to the AST, we won't report
                // an arity mismatch for `T.untyped` in the future, so report the arity mismatch now
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeAlias)) {
                    e.setHeader("No argument given to `{}`", "T.type_alias");
                }
            }
            auto typeAliasItem = TypeAliasResolutionItem{id->symbol, send->args[0].get()};
            this->todoTypeAliases_.emplace_back(std::move(typeAliasItem));

            // We also enter a ResolutionItem for the lhs of a type alias so even if the type alias isn't used,
            // we'll still emit a warning when the rhs of a type alias doesn't resolve.
            auto item = ResolutionItem{nesting_, id};
            this->todo_.emplace_back(std::move(item));
            return asgn;
        }

        auto *rhs = ast::cast_tree<ast::ConstantLit>(asgn->rhs.get());
        if (rhs == nullptr) {
            return asgn;
        }

        auto item = ClassAliasResolutionItem{id->symbol, rhs};

        // TODO(perf) currently, by construction the last item in resolve todo list is the one this alias depends on
        // We may be able to get some perf by using this
        this->todoClassAliases_.emplace_back(std::move(item));
        return asgn;
    }

    static bool compareLocs(core::Context ctx, core::Loc lhs, core::Loc rhs) {
        core::StrictLevel left = core::StrictLevel::Strong;
        core::StrictLevel right = core::StrictLevel::Strong;

        if (lhs.file().exists()) {
            left = lhs.file().data(ctx).strictLevel;
        }
        if (rhs.file().exists()) {
            right = rhs.file().data(ctx).strictLevel;
        }

        if (left != right) {
            return right < left;
        }
        if (lhs.file() != rhs.file()) {
            return lhs.file() < rhs.file();
        }
        if (lhs.beginPos() != rhs.beginPos()) {
            return lhs.beginPos() < rhs.beginPos();
        }
        return lhs.endPos() < rhs.endPos();
    }

    static int constantDepth(ast::ConstantLit *exp) {
        int depth = 0;
        ast::ConstantLit *scope = exp;
        while (scope->original && (scope = ast::cast_tree<ast::ConstantLit>(scope->original->scope.get()))) {
            depth += 1;
        }
        return depth;
    }

    struct ResolveWalkResult {
        vector<ResolutionItem> todo_;
        vector<AncestorResolutionItem> todoAncestors_;
        vector<ClassAliasResolutionItem> todoClassAliases_;
        vector<TypeAliasResolutionItem> todoTypeAliases_;
        vector<ast::ParsedFile> trees;
    };

    static bool locCompare(core::Loc lhs, core::Loc rhs) {
        if (lhs.file() < rhs.file()) {
            return true;
        }
        if (lhs.file() > rhs.file()) {
            return false;
        }
        if (lhs.beginPos() < rhs.beginPos()) {
            return true;
        }
        if (lhs.beginPos() > rhs.beginPos()) {
            return false;
        }
        return lhs.endPos() < rhs.endPos();
    }

    static vector<ast::ParsedFile> resolveConstants(core::MutableContext ctx, vector<ast::ParsedFile> trees,
                                                    WorkerPool &workers) {
        Timer timeit(ctx.state.errorQueue->logger, "resolver.resolve_constants");
        core::Context ictx = ctx;
        auto resultq = make_shared<BlockingBoundedQueue<ResolveWalkResult>>(trees.size());
        auto fileq = make_shared<ConcurrentBoundedQueue<ast::ParsedFile>>(trees.size());
        for (auto &tree : trees) {
            fileq->push(move(tree), 1);
        }

        workers.multiplexJob("resolveConstantsWalk", [ictx, fileq, resultq]() {
            Timer timeit(ictx.state.tracer(), "ResolveConstantsWorker");
            ResolveConstantsWalk constants(ictx);
            vector<ast::ParsedFile> partiallyResolvedTrees;
            ast::ParsedFile job;
            for (auto result = fileq->try_pop(job); !result.done(); result = fileq->try_pop(job)) {
                if (result.gotItem()) {
                    job.tree = ast::TreeMap::apply(ictx, constants, std::move(job.tree));
                    partiallyResolvedTrees.emplace_back(move(job));
                }
            }
            if (!partiallyResolvedTrees.empty()) {
                ResolveWalkResult result{move(constants.todo_), move(constants.todoAncestors_),
                                         move(constants.todoClassAliases_), move(constants.todoTypeAliases_),
                                         move(partiallyResolvedTrees)};
                auto computedTreesCount = result.trees.size();
                resultq->push(move(result), computedTreesCount);
            }
        });
        trees.clear();
        vector<ResolutionItem> todo;
        vector<AncestorResolutionItem> todoAncestors;
        vector<ClassAliasResolutionItem> todoClassAliases;
        vector<TypeAliasResolutionItem> todoTypeAliases;

        {
            ResolveWalkResult threadResult;
            for (auto result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), ctx.state.tracer());
                 !result.done();
                 result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), ctx.state.tracer())) {
                if (result.gotItem()) {
                    todo.insert(todo.end(), make_move_iterator(threadResult.todo_.begin()),
                                make_move_iterator(threadResult.todo_.end()));
                    todoAncestors.insert(todoAncestors.end(), make_move_iterator(threadResult.todoAncestors_.begin()),
                                         make_move_iterator(threadResult.todoAncestors_.end()));
                    todoClassAliases.insert(todoClassAliases.end(),
                                            make_move_iterator(threadResult.todoClassAliases_.begin()),
                                            make_move_iterator(threadResult.todoClassAliases_.end()));
                    todoTypeAliases.insert(todoTypeAliases.end(),
                                           make_move_iterator(threadResult.todoTypeAliases_.begin()),
                                           make_move_iterator(threadResult.todoTypeAliases_.end()));
                    trees.insert(trees.end(), make_move_iterator(threadResult.trees.begin()),
                                 make_move_iterator(threadResult.trees.end()));
                }
            }
        }

        fast_sort(todo,
                  [](const auto &lhs, const auto &rhs) -> bool { return locCompare(lhs.out->loc, rhs.out->loc); });
        fast_sort(todoAncestors, [](const auto &lhs, const auto &rhs) -> bool {
            return locCompare(lhs.ancestor->loc, rhs.ancestor->loc);
        });
        fast_sort(todoClassAliases,
                  [](const auto &lhs, const auto &rhs) -> bool { return locCompare(lhs.rhs->loc, rhs.rhs->loc); });
        fast_sort(todoTypeAliases,
                  [](const auto &lhs, const auto &rhs) -> bool { return locCompare(lhs.rhs->loc, rhs.rhs->loc); });
        fast_sort(trees,
                  [](const auto &lhs, const auto &rhs) -> bool { return locCompare(lhs.tree->loc, rhs.tree->loc); });

        Timer timeit1(ctx.state.errorQueue->logger, "resolver.resolve_constants.fixed_point");

        bool progress = true;
        bool first = true; // we need to run at least once to force class aliases and type aliases

        while (progress && (first || !todo.empty() || !todoAncestors.empty())) {
            first = false;
            counterInc("resolve.constants.retries");
            {
                Timer timeit(ctx.state.errorQueue->logger, "resolver.resolve_constants.fixed_point.ancestors");
                // This is an optimization. The order should not matter semantically
                // We try to resolve most ancestors second because this makes us much more likely to resolve everything
                // else.
                int origSize = todoAncestors.size();
                auto it =
                    remove_if(todoAncestors.begin(), todoAncestors.end(), [ctx](AncestorResolutionItem &job) -> bool {
                        auto resolved = resolveAncestorJob(ctx, job, false);
                        if (resolved) {
                            tryRegisterSealedSubclass(ctx, job);
                        }
                        return resolved;
                    });
                todoAncestors.erase(it, todoAncestors.end());
                progress = (origSize != todoAncestors.size());
                categoryCounterAdd("resolve.constants.ancestor", "retry", origSize - todoAncestors.size());
            }
            {
                Timer timeit(ctx.state.errorQueue->logger, "resolver.resolve_constants.fixed_point.constants");
                int origSize = todo.size();
                auto it = remove_if(todo.begin(), todo.end(),
                                    [ctx](ResolutionItem &job) -> bool { return resolveJob(ctx, job); });
                todo.erase(it, todo.end());
                progress = progress || (origSize != todo.size());
                categoryCounterAdd("resolve.constants.nonancestor", "retry", origSize - todo.size());
            }
            {
                Timer timeit(ctx.state.errorQueue->logger, "resolver.resolve_constants.fixed_point.class_aliases");
                // This is an optimization. The order should not matter semantically
                // This is done as a "pre-step" because the first iteration of this effectively ran in TreeMap.
                // every item in todoClassAliases implicitly depends on an item in item in todo
                // there would be no point in running the todoClassAliases step before todo

                int origSize = todoClassAliases.size();
                auto it =
                    remove_if(todoClassAliases.begin(), todoClassAliases.end(),
                              [ctx](ClassAliasResolutionItem &it) -> bool { return resolveClassAliasJob(ctx, it); });
                todoClassAliases.erase(it, todoClassAliases.end());
                progress = progress || (origSize != todoClassAliases.size());
                categoryCounterAdd("resolve.constants.aliases", "retry", origSize - todoClassAliases.size());
            }
            {
                Timer timeit(ctx.state.errorQueue->logger, "resolver.resolve_constants.fixed_point.type_aliases");
                int origSize = todoTypeAliases.size();
                auto it =
                    remove_if(todoTypeAliases.begin(), todoTypeAliases.end(),
                              [ctx](TypeAliasResolutionItem &it) -> bool { return resolveTypeAliasJob(ctx, it); });
                todoTypeAliases.erase(it, todoTypeAliases.end());
                progress = progress || (origSize != todoTypeAliases.size());
                categoryCounterAdd("resolve.constants.typealiases", "retry", origSize - todoTypeAliases.size());
            }
        }
        // We can no longer resolve new constants. All the code below reports errors

        categoryCounterAdd("resolve.constants.nonancestor", "failure", todo.size());
        categoryCounterAdd("resolve.constants.ancestor", "failure", todoAncestors.size());

        /*
         * Sort errors so we choose a deterministic error to report for each
         * missing constant:
         *
         * - Visit the strictest files first. If we were to report an error in
         *     an untyped file it would get suppressed, even if the same error
         *     also appeared in a typed file.
         *
         * - Break ties within strictness levels by file ID. We populate file
         *     IDs in the order we are given files on the command-line, so this
         *     means users see the error on the first file they provided.
         *
         * - Within a file, report the first occurrence.
         */
        fast_sort(todo, [ctx](const auto &lhs, const auto &rhs) -> bool {
            if (lhs.out->loc == rhs.out->loc) {
                return constantDepth(lhs.out) < constantDepth(rhs.out);
            }
            return compareLocs(ctx, lhs.out->loc, rhs.out->loc);
        });

        fast_sort(todoAncestors, [ctx](const auto &lhs, const auto &rhs) -> bool {
            if (lhs.ancestor->loc == rhs.ancestor->loc) {
                return constantDepth(lhs.ancestor) < constantDepth(rhs.ancestor);
            }
            return compareLocs(ctx, lhs.ancestor->loc, rhs.ancestor->loc);
        });

        // Note that this is missing alias stubbing, thus resolveJob needs to be able to handle missing aliases.

        {
            Timer timeit(ctx.state.errorQueue->logger, "resolver.resolve_constants.errors");
            for (auto &job : todo) {
                constantResolutionFailed(ctx, job);
            }

            for (auto &job : todoAncestors) {
                auto resolved = resolveAncestorJob(ctx, job, true);
                if (!resolved) {
                    resolved = resolveAncestorJob(ctx, job, true);
                    ENFORCE(resolved);
                }
            }
        }

        return trees;
    }
};

class ResolveTypeParamsWalk {
public:
    unique_ptr<ast::Assign> postTransformAssign(core::MutableContext ctx, unique_ptr<ast::Assign> asgn) {
        auto *id = ast::cast_tree<ast::ConstantLit>(asgn->lhs.get());
        if (id == nullptr || !id->symbol.exists()) {
            return asgn;
        }

        auto sym = id->symbol;
        auto data = sym.data(ctx);
        if (data->isTypeAlias()) {
            return asgn;
        }

        if (data->isTypeMember()) {
            auto send = ast::cast_tree<ast::Send>(asgn->rhs.get());
            ENFORCE(send->recv->isSelfReference());
            ENFORCE(send->fun == core::Names::typeMember() || send->fun == core::Names::typeTemplate());
            auto *memberType = core::cast_type<core::LambdaParam>(data->resultType.get());
            ENFORCE(memberType != nullptr);

            // NOTE: the resultType is set back in the namer to be a LambdaParam
            // with `T.untyped` for its bounds. We fix that here by setting the
            // bounds to top and bottom.
            memberType->lowerBound = core::Types::bottom();
            memberType->upperBound = core::Types::top();

            core::LambdaParam *parentType = nullptr;
            auto parentMember = data->owner.data(ctx)->superClass().data(ctx)->findMember(ctx, data->name);
            if (parentMember.exists()) {
                if (parentMember.data(ctx)->isTypeMember()) {
                    parentType = core::cast_type<core::LambdaParam>(parentMember.data(ctx)->resultType.get());
                    ENFORCE(parentType != nullptr);
                } else if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::ParentTypeBoundsMismatch)) {
                    const auto parentShow = parentMember.data(ctx)->show(ctx);
                    e.setHeader("`{}` is a type member but `{}` is not a type member", data->show(ctx), parentShow);
                    e.addErrorLine(parentMember.data(ctx)->loc(), "`{}` definition", parentShow);
                }
            }

            // When no args are supplied, this implies that the upper and lower
            // bounds of the type parameter are top and bottom.
            ast::Hash *hash = nullptr;
            if (send->args.size() == 1) {
                hash = ast::cast_tree<ast::Hash>(send->args[0].get());
            } else if (send->args.size() == 2) {
                hash = ast::cast_tree<ast::Hash>(send->args[1].get());
            }

            if (hash) {
                int i = -1;
                for (auto &keyExpr : hash->keys) {
                    i++;
                    auto lit = ast::cast_tree<ast::Literal>(keyExpr.get());
                    if (lit && lit->isSymbol(ctx)) {
                        ParsedSig emptySig;
                        auto allowSelfType = true;
                        auto allowRebind = false;
                        auto allowTypeMember = false;
                        core::TypePtr resTy =
                            TypeSyntax::getResultType(ctx, *(hash->values[i]), emptySig,
                                                      TypeSyntaxArgs{allowSelfType, allowRebind, allowTypeMember, sym});

                        switch (lit->asSymbol(ctx)._id) {
                            case core::Names::fixed()._id:
                                memberType->lowerBound = resTy;
                                memberType->upperBound = resTy;
                                break;

                            case core::Names::lower()._id:
                                memberType->lowerBound = resTy;
                                break;

                            case core::Names::upper()._id:
                                memberType->upperBound = resTy;
                                break;
                        }
                    }
                }
            }

            // If the parent bounds existis, validate the new bounds against
            // those of the parent.
            // NOTE: these errors could be better for cases involving
            // `fixed`.
            if (parentType != nullptr) {
                if (!core::Types::isSubType(ctx, parentType->lowerBound, memberType->lowerBound)) {
                    if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::ParentTypeBoundsMismatch)) {
                        e.setHeader("parent lower bound `{}` is not a subtype of lower bound `{}`",
                                    parentType->lowerBound->show(ctx), memberType->lowerBound->show(ctx));
                    }
                }
                if (!core::Types::isSubType(ctx, memberType->upperBound, parentType->upperBound)) {
                    if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::ParentTypeBoundsMismatch)) {
                        e.setHeader("upper bound `{}` is not a subtype of parent upper bound `{}`",
                                    memberType->upperBound->show(ctx), parentType->upperBound->show(ctx));
                    }
                }
            }

            // Ensure that the new lower bound is a subtype of the upper
            // bound. This will be a no-op in the case that the type member
            // is fixed.
            if (!core::Types::isSubType(ctx, memberType->lowerBound, memberType->upperBound)) {
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidTypeMemberBounds)) {
                    e.setHeader("`{}` is not a subtype of `{}`", memberType->lowerBound->show(ctx),
                                memberType->upperBound->show(ctx));
                }
            }
        }

        return asgn;
    }
};

class ResolveSignaturesWalk {
private:
    std::vector<int> nestedBlockCounts;

    ast::Local *getArgLocal(core::Context ctx, const core::ArgInfo &argSym, const ast::MethodDef &mdef, int pos,
                            bool isOverloaded) {
        if (!isOverloaded) {
            return ast::MK::arg2Local(mdef.args[pos].get());
        } else {
            // we cannot rely on method and symbol arguments being aligned, as method could have more arguments.
            // we roundtrip through original symbol that is stored in mdef.
            auto internalNameToLookFor = argSym.name;
            auto originalArgIt = absl::c_find_if(mdef.symbol.data(ctx)->arguments(),
                                                 [&](const auto &arg) { return arg.name == internalNameToLookFor; });
            ENFORCE(originalArgIt != mdef.symbol.data(ctx)->arguments().end());
            auto realPos = originalArgIt - mdef.symbol.data(ctx)->arguments().begin();
            return ast::MK::arg2Local(mdef.args[realPos].get());
        }
    }

    void fillInInfoFromSig(core::MutableContext ctx, core::SymbolRef method, core::Loc exprLoc, ParsedSig sig,
                           bool isOverloaded, const ast::MethodDef &mdef) {
        ENFORCE(isOverloaded || mdef.symbol == method);
        ENFORCE(isOverloaded || method.data(ctx)->arguments().size() == mdef.args.size());

        if (!sig.seen.returns && !sig.seen.void_) {
            if (auto e = ctx.state.beginError(exprLoc, core::errors::Resolver::InvalidMethodSignature)) {
                e.setHeader("Malformed `{}`: No return type specified. Specify one with .returns()", "sig");
            }
        }
        if (sig.seen.returns && sig.seen.void_) {
            if (auto e = ctx.state.beginError(exprLoc, core::errors::Resolver::InvalidMethodSignature)) {
                e.setHeader("Malformed `{}`: Don't use both .returns() and .void", "sig");
            }
        }

        if (sig.seen.abstract) {
            method.data(ctx)->setAbstract();
        }
        if (sig.seen.implementation) {
            method.data(ctx)->setImplementation();
        }
        if (sig.seen.incompatibleOverride) {
            method.data(ctx)->setIncompatibleOverride();
        }
        if (sig.seen.generated) {
            method.data(ctx)->setHasGeneratedSig();
        } else {
            // HasGeneratedSig can be already set in incremental runs. Make sure we update it.
            // TODO: In future, enforce that the previous LOC was a tombstone if we're actually unsetting generated sig.
            method.data(ctx)->unsetHasGeneratedSig();
        }
        if (!sig.typeArgs.empty()) {
            method.data(ctx)->setGenericMethod();
            for (auto &typeSpec : sig.typeArgs) {
                if (typeSpec.type) {
                    auto name = ctx.state.freshNameUnique(core::UniqueNameKind::TypeVarName, typeSpec.name, 1);
                    auto sym = ctx.state.enterTypeArgument(typeSpec.loc, method, name, core::Variance::CoVariant);
                    auto asTypeVar = core::cast_type<core::TypeVar>(typeSpec.type.get());
                    ENFORCE(asTypeVar != nullptr);
                    asTypeVar->sym = sym;
                    sym.data(ctx)->resultType = typeSpec.type;
                }
            }
        }
        if (sig.seen.overridable) {
            method.data(ctx)->setOverridable();
        }
        if (sig.seen.override_) {
            method.data(ctx)->setOverride();
        }
        if (sig.seen.final) {
            method.data(ctx)->setFinalMethod();
        }
        if (sig.seen.bind) {
            method.data(ctx)->setReBind(sig.bind);
        }

        // Get the parameters order from the signature
        vector<ParsedSig::ArgSpec> sigParams;
        for (auto &spec : sig.argTypes) {
            sigParams.push_back(spec);
        }

        vector<ast::Local *> defParams; // Parameters order from the method declaration
        bool seenOptional = false;

        auto methodInfo = method.data(ctx);
        methodInfo->resultType = sig.returns;
        int i = -1;
        for (auto &arg : methodInfo->arguments()) {
            ++i;
            const auto local = getArgLocal(ctx, arg, mdef, i, isOverloaded);
            auto treeArgName = local->localVariable._name;
            ENFORCE(local != nullptr);

            // Check that optional keyword parameters are after all the required ones
            bool isKwd = arg.flags.isKeyword;
            bool isReq = !arg.flags.isBlock && !arg.flags.isRepeated && !arg.flags.isDefault;
            if (isKwd && !isReq) {
                seenOptional = true;
            } else if (isKwd && seenOptional && isReq) {
                if (auto e = ctx.state.beginError(arg.loc, core::errors::Resolver::BadParameterOrdering)) {
                    e.setHeader("Malformed `{}`. Required parameter `{}` must be declared before all the optional ones",
                                "sig", treeArgName.show(ctx));
                    e.addErrorLine(exprLoc, "Signature");
                }
            }

            defParams.push_back(local);

            auto spec = absl::c_find_if(sig.argTypes, [&](const auto &spec) { return spec.name == treeArgName; });

            if (spec != sig.argTypes.end()) {
                ENFORCE(spec->type != nullptr);
                arg.type = spec->type;
                arg.loc = spec->loc;
                arg.rebind = spec->rebind;
                sig.argTypes.erase(spec);
            } else if (arg.type == nullptr) {
                arg.type = core::Types::untyped(ctx, method);
                // We silence the "type not specified" error when a sig does not mention the synthesized block arg.
                bool isBlkArg = arg.name == core::Names::blkArg();
                if (!isOverloaded && !isBlkArg && (sig.seen.params || sig.seen.returns || sig.seen.void_)) {
                    // Only error if we have any types
                    if (auto e = ctx.state.beginError(arg.loc, core::errors::Resolver::InvalidMethodSignature)) {
                        e.setHeader("Malformed `{}`. Type not specified for argument `{}`", "sig",
                                    treeArgName.show(ctx));
                        e.addErrorLine(exprLoc, "Signature");
                    }
                }
            }

            if (isOverloaded && arg.flags.isKeyword) {
                if (auto e = ctx.state.beginError(arg.loc, core::errors::Resolver::InvalidMethodSignature)) {
                    e.setHeader("Malformed `{}`. Overloaded functions cannot have keyword arguments:  `{}`", "sig",
                                treeArgName.show(ctx));
                }
            }
        }

        for (auto spec : sig.argTypes) {
            if (auto e = ctx.state.beginError(spec.loc, core::errors::Resolver::InvalidMethodSignature)) {
                e.setHeader("Unknown argument name `{}`", spec.name.show(ctx));
            }
        }

        // Check params ordering match between signature and definition
        if (sig.argTypes.empty()) {
            int j = 0;
            for (auto spec : sigParams) {
                auto param = defParams[j];
                auto sname = spec.name.show(ctx);
                auto dname = param->localVariable._name.show(ctx);
                if (sname != dname) {
                    if (auto e = ctx.state.beginError(param->loc, core::errors::Resolver::BadParameterOrdering)) {
                        e.setHeader("Bad parameter ordering for `{}`, expected `{}` instead", dname, sname);
                        e.addErrorLine(spec.loc, "Expected index in signature:");
                    }
                }
                j++;
            }
        }
    }

    // In order to check a default argument that looks like
    //
    //     sig {params(x: T)}
    //     def foo(x: <expr>)
    //       ...
    //     end
    //
    // we elaborate the method definition to
    //
    //     def foo(x: <expr>)
    //       T.let(<expr>, T)
    //       ...
    //     end
    //
    // which will then get checked later on in the pipeline.
    void injectOptionalArgs(core::MutableContext ctx, ast::MethodDef *mdef) {
        ast::InsSeq::STATS_store lets;

        if (mdef->symbol.data(ctx)->isAbstract()) {
            // TODO(jez) Check that abstract methods don't have defined bodies earlier (currently done in infer)
            // so that we can unblock checking default arguments of abstract methods
            return;
        }

        int i = -1;
        for (auto &argSym : mdef->symbol.data(ctx)->arguments()) {
            i++;
            auto &argExp = mdef->args[i];
            auto argType = argSym.type;

            if (auto *optArgExp = ast::cast_tree<ast::OptionalArg>(argExp.get())) {
                // Using optArgExp's loc will make errors point to the arg list, even though the T.let is in the body.
                auto let = make_unique<ast::Cast>(optArgExp->loc, argType, optArgExp->default_->deepCopy(),
                                                  core::Names::let());
                lets.emplace_back(std::move(let));
            }
        }

        if (!lets.empty()) {
            auto loc = mdef->rhs->loc;
            mdef->rhs = ast::MK::InsSeq(loc, std::move(lets), std::move(mdef->rhs));
        }
    }

    // Force errors from any signatures that didn't attach to methods.
    // `lastSigs` will always be empty after this function is called.
    void processLeftoverSigs(core::MutableContext ctx, InlinedVector<ast::Send *, 1> &lastSigs) {
        if (!lastSigs.empty()) {
            // These sigs won't have been parsed, as there was no methods to
            // attach them to -- parse them here manually to force any errors.
            for (auto sig : lastSigs) {
                auto allowSelfType = true;
                auto allowRebind = false;
                auto allowTypeMember = true;
                TypeSyntax::parseSig(
                    ctx, sig, nullptr,
                    TypeSyntaxArgs{allowSelfType, allowRebind, allowTypeMember, core::Symbols::untyped()});
            }

            if (auto e = ctx.state.beginError(lastSigs[0]->loc, core::errors::Resolver::InvalidMethodSignature)) {
                e.setHeader("Malformed `{}`. No method def following it", "sig");
            }

            lastSigs.clear();
        }
    }

    void processClassBody(core::MutableContext ctx, unique_ptr<ast::ClassDef> &klass) {
        InlinedVector<ast::Send *, 1> lastSigs;
        for (auto &stat : klass->rhs) {
            processStatement(ctx, stat, lastSigs);
        }

        processLeftoverSigs(ctx, lastSigs);

        auto toRemove = remove_if(klass->rhs.begin(), klass->rhs.end(),
                                  [](unique_ptr<ast::Expression> &stat) -> bool { return stat.get() == nullptr; });
        klass->rhs.erase(toRemove, klass->rhs.end());
    }

    void processInSeq(core::MutableContext ctx, unique_ptr<ast::InsSeq> &seq) {
        InlinedVector<ast::Send *, 1> lastSigs;

        // Explicitly check in the contxt of the class, not <static-init>
        auto classCtx = ctx.withOwner(ctx.owner.data(ctx)->enclosingClass(ctx));

        for (auto &stat : seq->stats) {
            processStatement(classCtx, stat, lastSigs);
        }
        if (!ast::isa_tree<ast::EmptyTree>(seq->expr.get())) {
            processStatement(classCtx, seq->expr, lastSigs);
        }

        processLeftoverSigs(classCtx, lastSigs);

        auto toRemove = remove_if(seq->stats.begin(), seq->stats.end(),
                                  [](unique_ptr<ast::Expression> &stat) -> bool { return stat.get() == nullptr; });
        seq->stats.erase(toRemove, seq->stats.end());
    }

    void processStatement(core::MutableContext ctx, unique_ptr<ast::Expression> &stat,
                          InlinedVector<ast::Send *, 1> &lastSigs) {
        typecase(
            stat.get(),

            [&](ast::Send *send) {
                if (TypeSyntax::isSig(ctx, send)) {
                    if (!lastSigs.empty()) {
                        if (!ctx.permitOverloadDefinitions(send->loc.file())) {
                            if (auto e = ctx.state.beginError(lastSigs[0]->loc,
                                                              core::errors::Resolver::OverloadNotAllowed)) {
                                e.setHeader("Unused type annotation. No method def before next annotation");
                                e.addErrorLine(send->loc, "Type annotation that will be used instead");
                            }
                        }
                    }

                    lastSigs.emplace_back(send);
                    return;
                }
            },

            [&](ast::MethodDef *mdef) {
                if (debug_mode) {
                    bool hasSig = !lastSigs.empty();
                    bool DSL = mdef->isDSLSynthesized();
                    bool isRBI = mdef->loc.file().data(ctx).isRBI();
                    if (hasSig) {
                        categoryCounterInc("method.sig", "true");
                    } else {
                        categoryCounterInc("method.sig", "false");
                    }
                    if (DSL) {
                        categoryCounterInc("method.dsl", "true");
                    } else {
                        categoryCounterInc("method.dsl", "false");
                    }
                    if (isRBI) {
                        categoryCounterInc("method.rbi", "true");
                    } else {
                        categoryCounterInc("method.rbi", "false");
                    }
                    if (hasSig && !isRBI && !DSL) {
                        counterInc("types.sig.human");
                    }
                }

                if (!lastSigs.empty()) {
                    prodCounterInc("types.sig.count");

                    auto loc = lastSigs[0]->loc;
                    if (loc.file().data(ctx).originalSigil == core::StrictLevel::None &&
                        !lastSigs.front()->isDSLSynthesized()) {
                        if (auto e = ctx.state.beginError(loc, core::errors::Resolver::SigInFileWithoutSigil)) {
                            e.setHeader("To use `sig`, this file must declare an explicit `# typed:` sigil (found: "
                                        "none). If you're not sure which one to use, start with `# typed: false`");
                        }
                    }

                    bool isOverloaded = lastSigs.size() > 1 && ctx.permitOverloadDefinitions(loc.file());
                    auto originalName = mdef->symbol.data(ctx)->name;
                    if (isOverloaded) {
                        ctx.state.mangleRenameSymbol(mdef->symbol, originalName);
                    }
                    int i = 0;

                    // process signatures in the context of either the current
                    // class, or the current singleton class, depending on if
                    // the current method is a self method.
                    core::SymbolRef sigOwner;
                    if (mdef->isSelf()) {
                        sigOwner = ctx.owner.data(ctx)->singletonClass(ctx);
                    } else {
                        sigOwner = ctx.owner;
                    }

                    while (i < lastSigs.size()) {
                        auto allowSelfType = true;
                        auto allowRebind = false;
                        auto allowTypeMember = true;
                        auto sig = TypeSyntax::parseSig(
                            ctx.withOwner(sigOwner), ast::cast_tree<ast::Send>(lastSigs[i]), nullptr,
                            TypeSyntaxArgs{allowSelfType, allowRebind, allowTypeMember, mdef->symbol});
                        core::SymbolRef overloadSym;
                        if (isOverloaded) {
                            vector<int> argsToKeep;
                            int argId = -1;
                            for (auto &argTree : mdef->args) {
                                argId++;
                                const auto local = ast::MK::arg2Local(argTree.get());
                                auto treeArgName = local->localVariable._name;
                                ENFORCE(local != nullptr);
                                auto spec =
                                    absl::c_find_if(sig.argTypes, [&](auto &spec) { return spec.name == treeArgName; });
                                if (spec != sig.argTypes.end()) {
                                    argsToKeep.emplace_back(argId);
                                }
                            }
                            overloadSym = ctx.state.enterNewMethodOverload(lastSigs[i]->loc, mdef->symbol, originalName,
                                                                           i, argsToKeep);
                            if (i != lastSigs.size() - 1) {
                                overloadSym.data(ctx)->setOverloaded();
                            }
                        } else {
                            overloadSym = mdef->symbol;
                        }
                        fillInInfoFromSig(ctx, overloadSym, lastSigs[i]->loc, move(sig), isOverloaded, *mdef);
                        i++;
                    }

                    if (!isOverloaded) {
                        injectOptionalArgs(ctx, mdef);
                    }

                    // OVERLOAD
                    lastSigs.clear();
                }

                if (mdef->symbol.data(ctx)->isAbstract()) {
                    if (!ast::isa_tree<ast::EmptyTree>(mdef->rhs.get())) {
                        if (auto e =
                                ctx.state.beginError(mdef->rhs->loc, core::errors::Resolver::AbstractMethodWithBody)) {
                            e.setHeader("Abstract methods must not contain any code in their body");
                            e.replaceWith("Delete the body", mdef->rhs->loc, "");
                        }

                        mdef->rhs = ast::MK::EmptyTree();
                    }
                    if (!mdef->symbol.data(ctx)->enclosingClass(ctx).data(ctx)->isClassAbstract()) {
                        if (auto e = ctx.state.beginError(mdef->loc,
                                                          core::errors::Resolver::AbstractMethodOutsideAbstract)) {
                            e.setHeader("Before declaring an abstract method, you must mark your class/module "
                                        "as abstract using `abstract!` or `interface!`");
                        }
                    }
                } else if (mdef->symbol.data(ctx)->enclosingClass(ctx).data(ctx)->isClassInterface()) {
                    if (auto e = ctx.state.beginError(mdef->loc, core::errors::Resolver::ConcreteMethodInInterface)) {
                        e.setHeader("All methods in an interface must be declared abstract");
                    }
                }
            },
            [&](ast::ClassDef *cdef) {
                // Leave in place
            },

            [&](ast::EmptyTree *e) { stat.reset(nullptr); },

            [&](ast::Expression *e) {});
    }

    // Resolve the type of the rhs of a constant declaration. This logic is
    // extremely simplistic; We only handle simple literals, and explicit casts.
    //
    // We don't handle array or hash literals, because intuiting the element
    // type (once we have generics) will be nontrivial.
    core::TypePtr resolveConstantType(core::Context ctx, unique_ptr<ast::Expression> &expr, core::SymbolRef ofSym) {
        core::TypePtr result;
        typecase(
            expr.get(), [&](ast::Literal *a) { result = a->value; },
            [&](ast::Cast *cast) {
                if (cast->cast != core::Names::let()) {
                    if (auto e = ctx.state.beginError(cast->loc, core::errors::Resolver::ConstantAssertType)) {
                        e.setHeader("Use `{}` to specify the type of constants", "T.let");
                    }
                }
                result = cast->type;
            },
            [&](ast::InsSeq *outer) { result = resolveConstantType(ctx, outer->expr, ofSym); },
            [&](ast::Expression *expr) {
                if (auto *send = ast::cast_tree<ast::Send>(expr)) {
                    if (send->fun == core::Names::typeAlias()) {
                        // short circuit if this is a type alias
                        return;
                    }
                }
                if (ast::isa_tree<ast::UnresolvedConstantLit>(expr) || ast::isa_tree<ast::ConstantLit>(expr)) {
                    // we don't want to report an error here because constants that are aliases for other constants can
                    // easily have their types inferred.
                    return;
                }
                if (auto e = ctx.state.beginError(expr->loc, core::errors::Resolver::ConstantMissingTypeAnnotation)) {
                    e.setHeader("Constants must have type annotations with `{}` when specifying `{}`", "T.let",
                                "# typed: strict");
                }
            });
        return result;
    }

    bool handleDeclaration(core::MutableContext ctx, unique_ptr<ast::Assign> &asgn) {
        auto *uid = ast::cast_tree<ast::UnresolvedIdent>(asgn->lhs.get());
        if (uid == nullptr) {
            return false;
        }

        if (uid->kind != ast::UnresolvedIdent::Instance && uid->kind != ast::UnresolvedIdent::Class) {
            return false;
        }
        ast::Expression *recur = asgn->rhs.get();
        while (auto outer = ast::cast_tree<ast::InsSeq>(recur)) {
            recur = outer->expr.get();
        }

        auto *cast = ast::cast_tree<ast::Cast>(recur);
        if (cast == nullptr) {
            return false;
        } else if (cast->cast != core::Names::let()) {
            if (auto e = ctx.state.beginError(cast->loc, core::errors::Resolver::ConstantAssertType)) {
                e.setHeader("Use `{}` to specify the type of constants", "T.let");
            }
        }

        core::SymbolRef scope;
        if (uid->kind == ast::UnresolvedIdent::Class) {
            if (!ctx.owner.data(ctx)->isClass()) {
                if (auto e = ctx.state.beginError(uid->loc, core::errors::Resolver::InvalidDeclareVariables)) {
                    e.setHeader("Class variables must be declared at class scope");
                }
            }

            scope = ctx.owner.data(ctx)->enclosingClass(ctx);
        } else {
            // we need to check nested block counts because we want all fields to be declared on top level of either
            // class or body, rather then nested in some block
            if (nestedBlockCounts.back() == 0 && ctx.owner.data(ctx)->isClass()) {
                // Declaring a class instance variable
            } else if (nestedBlockCounts.back() == 0 && ctx.owner.data(ctx)->name == core::Names::initialize()) {
                // Declaring a instance variable
            } else if (ctx.owner.data(ctx)->isMethod() && ctx.owner.data(ctx)->owner.data(ctx)->isSingletonClass(ctx)) {
                // Declaring a class instance variable in a static method
                if (auto e = ctx.state.beginError(uid->loc, core::errors::Resolver::InvalidDeclareVariables)) {
                    e.setHeader("Singleton instance variables must be declared inside the class body");
                }
            } else {
                // Inside a method; declaring a normal instance variable
                if (auto e = ctx.state.beginError(uid->loc, core::errors::Resolver::InvalidDeclareVariables)) {
                    e.setHeader("Instance variables must be declared inside `initialize`");
                }
            }
            scope = ctx.selfClass();
        }

        auto prior = scope.data(ctx)->findMember(ctx, uid->name);
        if (prior.exists()) {
            if (core::Types::equiv(ctx, prior.data(ctx)->resultType, cast->type)) {
                // We already have a symbol for this field, and it matches what we already saw, so we can short circuit.
                return true;
            } else {
                if (auto e = ctx.state.beginError(uid->loc, core::errors::Resolver::DuplicateVariableDeclaration)) {
                    e.setHeader("Redeclaring variable `{}` with mismatching type", uid->name.data(ctx)->show(ctx));
                    e.addErrorLine(prior.data(ctx)->loc(), "Previous declaration is here:");
                }
                return false;
            }
        }
        core::SymbolRef var;

        if (uid->kind == ast::UnresolvedIdent::Class) {
            var = ctx.state.enterStaticFieldSymbol(uid->loc, scope, uid->name);
        } else {
            var = ctx.state.enterFieldSymbol(uid->loc, scope, uid->name);
        }

        var.data(ctx)->resultType = cast->type;
        return true;
    }

    core::SymbolRef methodOwner(core::Context ctx) {
        core::SymbolRef owner = ctx.owner.data(ctx)->enclosingClass(ctx);
        if (owner == core::Symbols::root()) {
            // Root methods end up going on object
            owner = core::Symbols::Object();
        }
        return owner;
    }

public:
    ResolveSignaturesWalk() {
        nestedBlockCounts.emplace_back(0);
    }

    unique_ptr<ast::Assign> postTransformAssign(core::MutableContext ctx, unique_ptr<ast::Assign> asgn) {
        if (handleDeclaration(ctx, asgn)) {
            return asgn;
        }

        auto *id = ast::cast_tree<ast::ConstantLit>(asgn->lhs.get());
        if (id == nullptr || !id->symbol.exists()) {
            return asgn;
        }

        auto sym = id->symbol;
        auto data = sym.data(ctx);
        if (data->isTypeAlias() || data->isTypeMember()) {
            return asgn;
        }

        if (data->isStaticField() && data->resultType == nullptr) {
            data->resultType = resolveConstantType(ctx, asgn->rhs, sym);
            if (data->resultType == nullptr) {
                auto rhs = move(asgn->rhs);
                auto loc = rhs->loc;
                asgn->rhs = ast::MK::Send1(loc, ast::MK::Constant(loc, core::Symbols::Magic()),
                                           core::Names::suggestType(), move(rhs));
                data->resultType = core::Types::untyped(ctx, sym);
            }
        } else {
            // we might have already resolved this constant but we want to make sure to still report some errors if
            // those errors come up
            resolveConstantType(ctx, asgn->rhs, sym);
        }

        return asgn;
    }

    unique_ptr<ast::ClassDef> preTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> original) {
        nestedBlockCounts.emplace_back(0);
        return original;
    }

    unique_ptr<ast::Expression> postTransformClassDef(core::MutableContext ctx, unique_ptr<ast::ClassDef> original) {
        processClassBody(ctx.withOwner(original->symbol), original);
        return original;
    }

    unique_ptr<ast::MethodDef> preTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> original) {
        nestedBlockCounts.emplace_back(0);
        return original;
    }

    unique_ptr<ast::Expression> postTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> original) {
        nestedBlockCounts.pop_back();
        return original;
    }

    unique_ptr<ast::Block> preTransformBlock(core::Context ctx, unique_ptr<ast::Block> block) {
        nestedBlockCounts.back() += 1;
        return block;
    }

    unique_ptr<ast::Expression> postTransformBlock(core::Context ctx, unique_ptr<ast::Block> block) {
        nestedBlockCounts.back() -= 1;
        return block;
    }

    unique_ptr<ast::Expression> postTransformInsSeq(core::MutableContext ctx, unique_ptr<ast::InsSeq> original) {
        processInSeq(ctx, original);
        return original;
    }

    unique_ptr<ast::Expression> postTransformSend(core::MutableContext ctx, unique_ptr<ast::Send> send) {
        if (auto *id = ast::cast_tree<ast::ConstantLit>(send->recv.get())) {
            if (id->symbol != core::Symbols::T()) {
                return send;
            }
            switch (send->fun._id) {
                case core::Names::let()._id:
                case core::Names::assertType()._id:
                case core::Names::cast()._id: {
                    if (send->args.size() < 2) {
                        return send;
                    }

                    // Compute the containing class when translating the type,
                    // as there's a very good chance this has been called from a
                    // method context.
                    core::SymbolRef ownerClass = ctx.owner.data(ctx)->enclosingClass(ctx);

                    auto expr = std::move(send->args[0]);
                    ParsedSig emptySig;
                    auto allowSelfType = true;
                    auto allowRebind = false;
                    auto allowTypeMember = true;
                    auto type = TypeSyntax::getResultType(
                        ctx.withOwner(ownerClass), *(send->args[1]), emptySig,
                        TypeSyntaxArgs{allowSelfType, allowRebind, allowTypeMember, core::Symbols::noSymbol()});
                    return ast::MK::InsSeq1(send->loc, ast::MK::KeepForTypechecking(std::move(send->args[1])),
                                            make_unique<ast::Cast>(send->loc, type, std::move(expr), send->fun));
                }
                case core::Names::revealType()._id:
                    // This error does not match up with our "upper error levels are super sets
                    // of errors from lower levels" claim. This is ONLY an error in lower levels.
                    if (send->loc.file().data(ctx).strictLevel <= core::StrictLevel::False) {
                        if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::RevealTypeInUntypedFile)) {
                            e.setHeader("`{}` can only reveal types in `{}` files (or higher)", "T.reveal_type",
                                        "# typed: true");
                        }
                    }
                    return send;
                default:
                    return send;
            }
        } else if (send->recv.get()->isSelfReference()) {
            if (send->fun != core::Names::aliasMethod()) {
                return send;
            }

            vector<core::NameRef> args;
            for (auto &arg : send->args) {
                auto lit = ast::cast_tree<ast::Literal>(arg.get());
                if (lit == nullptr || !lit->isSymbol(ctx)) {
                    continue;
                }
                core::NameRef name = lit->asSymbol(ctx);

                args.emplace_back(name);
            }
            if (send->args.size() != 2) {
                return send;
            }
            if (args.size() != 2) {
                return send;
            }

            auto fromName = args[0];
            auto toName = args[1];

            auto owner = methodOwner(ctx);
            core::SymbolRef toMethod = owner.data(ctx)->findMember(ctx, toName);
            if (!toMethod.exists()) {
                if (auto e = ctx.state.beginError(send->args[1]->loc, core::errors::Resolver::BadAliasMethod)) {
                    e.setHeader("Can't make method alias from `{}` to non existing method `{}`", fromName.show(ctx),
                                toName.show(ctx));
                }
                toMethod = core::Symbols::Sorbet_Private_Static_badAliasMethodStub();
            }

            core::SymbolRef fromMethod = owner.data(ctx)->findMemberNoDealias(ctx, fromName);
            if (fromMethod.exists() && fromMethod.data(ctx)->dealias(ctx) != toMethod) {
                if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::BadAliasMethod)) {
                    auto dealiased = fromMethod.data(ctx)->dealias(ctx);
                    if (fromMethod == dealiased) {
                        e.setHeader("Redefining the existing method `{}` as a method alias",
                                    fromMethod.data(ctx)->show(ctx));
                        e.addErrorLine(fromMethod.data(ctx)->loc(), "Previous definition");
                    } else {
                        e.setHeader("Redefining method alias `{}` from `{}` to `{}`", fromMethod.data(ctx)->show(ctx),
                                    dealiased.data(ctx)->show(ctx), toMethod.data(ctx)->show(ctx));
                        e.addErrorLine(fromMethod.data(ctx)->loc(), "Previous alias definition");
                        e.addErrorLine(dealiased.data(ctx)->loc(), "Previous alias pointed to");
                        e.addErrorLine(toMethod.data(ctx)->loc(), "Redefining alias to");
                    }
                }
                return send;
            }

            core::SymbolRef alias = ctx.state.enterMethodSymbol(send->loc, owner, fromName);
            alias.data(ctx)->resultType = core::make_type<core::AliasType>(toMethod);

            return send;
        } else {
            return send;
        }
    }
};

class ResolveMixesInClassMethodsWalk {
    void processMixesInClassMethods(core::MutableContext ctx, ast::Send *send) {
        if (!ctx.owner.data(ctx)->isClass() || !ctx.owner.data(ctx)->isClassModule()) {
            if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMixinDeclaration)) {
                e.setHeader("`{}` can only be declared inside a module, not a class", send->fun.data(ctx)->show(ctx));
            }
            // Keep processing it anyways
        }

        if (send->args.size() != 1) {
            if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMixinDeclaration)) {
                e.setHeader("Wrong number of arguments to `{}`: Expected: `{}`, got: `{}`",
                            send->fun.data(ctx)->show(ctx), 1, send->args.size());
            }
            return;
        }
        auto *front = send->args.front().get();
        auto *id = ast::cast_tree<ast::ConstantLit>(front);
        if (id == nullptr || !id->symbol.exists() || !id->symbol.data(ctx)->isClass()) {
            if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMixinDeclaration)) {
                e.setHeader("Argument to `{}` must be statically resolvable to a module",
                            send->fun.data(ctx)->show(ctx));
            }
            return;
        }
        if (id->symbol.data(ctx)->isClassClass()) {
            if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMixinDeclaration)) {
                e.setHeader("`{}` is a class, not a module; Only modules may be mixins",
                            id->symbol.data(ctx)->show(ctx));
            }
            return;
        }
        if (id->symbol == ctx.owner) {
            if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMixinDeclaration)) {
                e.setHeader("Must not pass your self to `{}`", send->fun.data(ctx)->show(ctx));
            }
            return;
        }
        auto existing = ctx.owner.data(ctx)->findMember(ctx, core::Names::classMethods());
        if (existing.exists() && existing != id->symbol) {
            if (auto e = ctx.state.beginError(send->loc, core::errors::Resolver::InvalidMixinDeclaration)) {
                e.setHeader("Redeclaring `{}` from module `{}` to module `{}`", send->fun.data(ctx)->show(ctx),
                            existing.data(ctx)->show(ctx), id->symbol.data(ctx)->show(ctx));
            }
            return;
        }
        ctx.owner.data(ctx)->members()[core::Names::classMethods()] = id->symbol;
    }

public:
    unique_ptr<ast::Expression> postTransformSend(core::MutableContext ctx, unique_ptr<ast::Send> original) {
        if (original->recv->isSelfReference() && original->fun == core::Names::mixesInClassMethods()) {
            processMixesInClassMethods(ctx, original.get());
            return ast::MK::EmptyTree();
        }
        return original;
    }
};

class ResolveSanityCheckWalk {
public:
    unique_ptr<ast::Expression> postTransformClassDef(core::MutableContext ctx, unique_ptr<ast::ClassDef> original) {
        ENFORCE(original->symbol != core::Symbols::todo(), "These should have all been resolved: {}",
                original->toString(ctx));
        if (original->symbol == core::Symbols::root()) {
            ENFORCE(ctx.state.lookupStaticInitForFile(original->loc).exists());
        } else {
            ENFORCE(ctx.state.lookupStaticInitForClass(original->symbol).exists());
        }
        return original;
    }
    unique_ptr<ast::Expression> postTransformMethodDef(core::MutableContext ctx, unique_ptr<ast::MethodDef> original) {
        ENFORCE(original->symbol != core::Symbols::todo(), "These should have all been resolved: {}",
                original->toString(ctx));
        return original;
    }
    unique_ptr<ast::Expression> postTransformUnresolvedConstantLit(core::MutableContext ctx,
                                                                   unique_ptr<ast::UnresolvedConstantLit> original) {
        ENFORCE(false, "These should have all been removed: {}", original->toString(ctx));
        return original;
    }
    unique_ptr<ast::ConstantLit> postTransformConstantLit(core::MutableContext ctx,
                                                          unique_ptr<ast::ConstantLit> original) {
        ENFORCE(ResolveConstantsWalk::isAlreadyResolved(ctx, *original));
        return original;
    }
};
}; // namespace

vector<ast::ParsedFile> Resolver::run(core::MutableContext ctx, vector<ast::ParsedFile> trees, WorkerPool &workers) {
    trees = ResolveConstantsWalk::resolveConstants(ctx, std::move(trees), workers);
    finalizeAncestors(ctx.state);
    trees = resolveMixesInClassMethods(ctx, std::move(trees));
    finalizeSymbols(ctx.state);
    trees = resolveTypeParams(ctx, std::move(trees));
    trees = resolveSigs(ctx, std::move(trees));
    sanityCheck(ctx, trees);

    return trees;
}

vector<ast::ParsedFile> Resolver::resolveTypeParams(core::MutableContext ctx, vector<ast::ParsedFile> trees) {
    ResolveTypeParamsWalk sigs;
    Timer timeit(ctx.state.errorQueue->logger, "resolver.type_params");
    for (auto &tree : trees) {
        tree.tree = ast::TreeMap::apply(ctx, sigs, std::move(tree.tree));
    }

    return trees;
}

vector<ast::ParsedFile> Resolver::resolveSigs(core::MutableContext ctx, vector<ast::ParsedFile> trees) {
    ResolveSignaturesWalk sigs;
    Timer timeit(ctx.state.errorQueue->logger, "resolver.sigs_vars_and_flatten");
    for (auto &tree : trees) {
        tree.tree = ast::TreeMap::apply(ctx, sigs, std::move(tree.tree));
    }

    return trees;
}

vector<ast::ParsedFile> Resolver::resolveMixesInClassMethods(core::MutableContext ctx, vector<ast::ParsedFile> trees) {
    ResolveMixesInClassMethodsWalk mixesInClassMethods;
    Timer timeit(ctx.state.errorQueue->logger, "resolver.mixes_in_class_methods");
    for (auto &tree : trees) {
        tree.tree = ast::TreeMap::apply(ctx, mixesInClassMethods, std::move(tree.tree));
    }
    return trees;
}

void Resolver::sanityCheck(core::MutableContext ctx, vector<ast::ParsedFile> &trees) {
    if (debug_mode) {
        Timer timeit(ctx.state.errorQueue->logger, "resolver.sanity_check");
        ResolveSanityCheckWalk sanity;
        for (auto &tree : trees) {
            tree.tree = ast::TreeMap::apply(ctx, sanity, std::move(tree.tree));
        }
    }
}

vector<ast::ParsedFile> Resolver::runTreePasses(core::MutableContext ctx, vector<ast::ParsedFile> trees) {
    auto workers = WorkerPool::create(0, ctx.state.tracer());
    trees = ResolveConstantsWalk::resolveConstants(ctx, std::move(trees), *workers);
    trees = resolveMixesInClassMethods(ctx, std::move(trees));
    trees = resolveTypeParams(ctx, std::move(trees));
    trees = resolveSigs(ctx, std::move(trees));
    sanityCheck(ctx, trees);
    // This check is FAR too slow to run on large codebases, especially with sanitizers on.
    // But it can be super useful to uncomment when debugging certain issues.
    // ctx.state.sanityCheck();

    return trees;
}

vector<ast::ParsedFile> Resolver::runConstantResolution(core::MutableContext ctx, vector<ast::ParsedFile> trees,
                                                        WorkerPool &workers) {
    trees = ResolveConstantsWalk::resolveConstants(ctx, std::move(trees), workers);
    sanityCheck(ctx, trees);

    return trees;
}

} // namespace sorbet::resolver
