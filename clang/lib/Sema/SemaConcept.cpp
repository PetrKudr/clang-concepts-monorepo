//===-- SemaConcept.cpp - Semantic Analysis for Constraints and Concepts --===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for C++ constraints and concepts.
//
//===----------------------------------------------------------------------===//

#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaInternal.h"
#include "clang/Sema/SemaDiagnostic.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Sema/TemplateDeduction.h"
#include "clang/Sema/Template.h"
#include "clang/Sema/Overload.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/SemaInternal.h"
#include "clang/AST/ExprCXX.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
using namespace clang;
using namespace sema;

bool Sema::CheckConstraintExpression(Expr *ConstraintExpression,
                                     Expr **Culprit) {
  // C++2a [temp.constr.atomic]p1
  // ..E shall be a constant expression of type bool.

  if (auto *BinOp = dyn_cast<BinaryOperator>(ConstraintExpression))
    if (BinOp->getOpcode() == BO_LAnd || BinOp->getOpcode() == BO_LOr)
      return CheckConstraintExpression(BinOp->getLHS()) &&
          CheckConstraintExpression(BinOp->getRHS());

  // An atomic constraint!
  if (ConstraintExpression->isTypeDependent())
    return true;

  QualType Type = ConstraintExpression->IgnoreParenImpCasts()->getType()
                      .getNonReferenceType().getUnqualifiedType();
  if (!Context.hasSameType(Type, Context.BoolTy)) {
    Diag(ConstraintExpression->getExprLoc(),
         diag::err_non_bool_atomic_constraint) << Type
        << ConstraintExpression->getSourceRange();
    if (Culprit)
      *Culprit = ConstraintExpression;
    return false;
  }
  return true;
}

template <typename AtomicEvaluator>
static bool
calculateConstraintSatisfaction(Sema &S, const Expr *ConstraintExpr,
                                ConstraintSatisfaction &Satisfaction,
                                AtomicEvaluator &&Evaluator) {
  if (auto *BO = dyn_cast<BinaryOperator>(ConstraintExpr)) {
    if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr) {
      if (calculateConstraintSatisfaction(S, BO->getLHS(), Satisfaction,
                                          Evaluator))
        return true;

      bool IsLHSSatisfied = Satisfaction.IsSatisfied;

      if (BO->getOpcode() == BO_LOr && IsLHSSatisfied)
        // [temp.constr.op] p3
        //    A disjunction is a constraint taking two operands. To determine if
        //    a disjunction is satisfied, the satisfaction of the first operand
        //    is checked. If that is satisfied, the disjunction is satisfied.
        //    Otherwise, the disjunction is satisfied if and only if the second
        //    operand is satisfied.
        return false;

      if (BO->getOpcode() == BO_LAnd && !IsLHSSatisfied)
        // [temp.constr.op] p2
        //    A conjunction is a constraint taking two operands. To determine if
        //    a conjunction is satisfied, the satisfaction of the first operand
        //    is checked. If that is not satisfied, the conjunction is not
        //    satisfied. Otherwise, the conjunction is satisfied if and only if
        //    the second operand is satisfied.
        return false;

      return calculateConstraintSatisfaction(S, BO->getRHS(), Satisfaction,
          std::forward<AtomicEvaluator>(Evaluator));
    }
  } else if (auto *PO = dyn_cast<ParenExpr>(ConstraintExpr))
    return calculateConstraintSatisfaction(S, PO->getSubExpr(), Satisfaction,
        std::forward<AtomicEvaluator>(Evaluator));
  else if (auto *C = dyn_cast<ExprWithCleanups>(ConstraintExpr))
    return calculateConstraintSatisfaction(S, C->getSubExpr(), Satisfaction,
        std::forward<AtomicEvaluator>(Evaluator));

  // An atomic constraint expression
  ExprResult SubstitutedAtomicExpr = Evaluator(ConstraintExpr);

  if (SubstitutedAtomicExpr.isInvalid())
    return true;

  if (!SubstitutedAtomicExpr.isUsable())
    // Evaluator has decided satisfaction without yielding an expression.
    return false;

  EnterExpressionEvaluationContext ConstantEvaluated(
      S, Sema::ExpressionEvaluationContext::ConstantEvaluated);
  SmallVector<PartialDiagnosticAt, 2> EvaluationDiags;
  Expr::EvalResult EvalResult;
  EvalResult.Diag = &EvaluationDiags;
  if (!SubstitutedAtomicExpr.get()->EvaluateAsRValue(EvalResult, S.Context)) {
      // C++2a [temp.constr.atomic]p1
      //   ...E shall be a constant expression of type bool.
    S.Diag(SubstitutedAtomicExpr.get()->getBeginLoc(),
           diag::err_non_constant_constraint_expression)
        << SubstitutedAtomicExpr.get()->getSourceRange();
    for (const PartialDiagnosticAt &PDiag : EvaluationDiags)
      S.Diag(PDiag.first, PDiag.second);
    return true;
  }

  Satisfaction.IsSatisfied = EvalResult.Val.getInt().getBoolValue();
  if (!Satisfaction.IsSatisfied)
    Satisfaction.Details.emplace_back(ConstraintExpr,
                                      SubstitutedAtomicExpr.get());

  return false;
}

template<typename SubstitutionInstantiatingTemplateCreator>
static bool calculateConstraintSatisfaction(Sema &S,
    SubstitutionInstantiatingTemplateCreator Creator,
    const MultiLevelTemplateArgumentList &TemplateArgs,
    SourceLocation TemplateNameLoc, const Expr *ConstraintExpr,
    ConstraintSatisfaction &Satisfaction,
    bool *ContainsUnexpandedParameterPack = nullptr,
    bool *IsDependent = nullptr) {
  return calculateConstraintSatisfaction(
      S, ConstraintExpr, Satisfaction, [&](const Expr *AtomicExpr) {
        EnterExpressionEvaluationContext ConstantEvaluated(
            S, Sema::ExpressionEvaluationContext::ConstantEvaluated);

        // Atomic constraint - substitute arguments and check satisfaction.
        ExprResult SubstitutedExpression;
        {
          TemplateDeductionInfo Info(TemplateNameLoc);
          Sema::InstantiatingTemplate Inst(
              Creator(AtomicExpr->getBeginLoc(), AtomicExpr->getSourceRange(),
                      Info));
          if (Inst.isInvalid())
            return ExprError();
          // We do not want error diagnostics escaping here.
          Sema::SFINAETrap Trap(S);
          SubstitutedExpression = S.SubstExpr(const_cast<Expr *>(AtomicExpr),
                                              TemplateArgs);
          if (SubstitutedExpression.isInvalid() || Trap.hasErrorOccurred()) {
            // C++2a [temp.constr.atomic]p1
            //   ...If substitution results in an invalid type or expression, the
            //   constraint is not satisfied.
            if (!Trap.hasErrorOccurred())
              // A non-SFINAE error has occured as a result of this
              // substitution.
              return ExprError();

            auto *SubstDiag =
                new PartialDiagnosticAt{SourceLocation(),
                                        PartialDiagnostic::NullDiagnostic()};
            Info.takeSFINAEDiagnostic(*SubstDiag);
            Satisfaction.Details.emplace_back(AtomicExpr, SubstDiag);
            Satisfaction.IsSatisfied = false;
            return ExprEmpty();
          }
        }

        if (ContainsUnexpandedParameterPack != nullptr)
          *ContainsUnexpandedParameterPack |=
              SubstitutedExpression.get()->containsUnexpandedParameterPack();

        if (SubstitutedExpression.get()->isInstantiationDependent()) {
          // This might happen when constraint expressions present somewhere in
          // a member declaration of a template are instantiated:
          //
          // template<typename T>
          // struct S {
          //   template<typename U,
          //            decltype(
          //              requires { requires sizeof(T{} + U{}) > 1); }
          //            ) W>
          //   struct M { };
          // }
          //
          // Referencing S<int> will trigger the instantiation of the
          // nested-requirement, with only the <T> argument, and no the <U>
          // argument. We will treat this as satisfied for now because the
          // expression will be instantiated again anyway with both the <T> and
          // the <U> arguments.
          if (IsDependent != nullptr)
            *IsDependent = true;
          Satisfaction.IsSatisfied = true;
          return ExprEmpty();
        }

        if (!S.CheckConstraintExpression(SubstitutedExpression.get()))
          return ExprError();

        return SubstitutedExpression;
      });
}

template<typename SubstitutionInstantiatingTemplateCreator>
static bool
CheckConstraintSatisfaction(Sema &S, ArrayRef<const Expr *> ConstraintExprs,
                            const MultiLevelTemplateArgumentList &TemplateArgs,
                            SourceRange TemplateIDRange,
                            SubstitutionInstantiatingTemplateCreator Creator,
                            ConstraintSatisfaction &Satisfaction,
                            bool *ContainsUnexpandedParameterPack = nullptr,
                            bool *IsDependent = nullptr) {
  for (const Expr *ConstraintExpr : ConstraintExprs) {
    if (calculateConstraintSatisfaction(S, Creator, TemplateArgs,
                                        TemplateIDRange.getBegin(),
                                        ConstraintExpr, Satisfaction,
                                        ContainsUnexpandedParameterPack,
                                        IsDependent))
      return true;
    if (!Satisfaction.IsSatisfied)
      // [temp.constr.op] p2
      //   [...] To determine if a conjunction is satisfied, the satisfaction
      //   of the first operand is checked. If that is not satisfied, the
      //   conjunction is not satisfied. [...]
      return false;
  }
  return false;
}

bool Sema::CheckConstraintSatisfaction(NamedDecl *ConstraintOwner,
    NamedDecl *Template, ArrayRef<const Expr *> ConstraintExprs,
    const MultiLevelTemplateArgumentList &TemplateArgs,
    SourceRange TemplateIDRange, ConstraintSatisfaction &Satisfaction) {
  if (ConstraintExprs.empty()) {
    Satisfaction.IsSatisfied = true;
    return false;
  }

  llvm::FoldingSetNodeID ID;
  void *InsertPos;
  ConstraintSatisfaction::Profile(ID, Context, ConstraintOwner,
                                  TemplateArgs.getInnermost());
  ConstraintSatisfaction *Cached =
      SatisfactionCache.FindNodeOrInsertPos(ID, InsertPos);
  if (!Cached) {
    InstantiatingTemplate Inst(*this, TemplateIDRange.getBegin(),
        InstantiatingTemplate::ConstraintsCheck{}, Template,
        TemplateArgs.getInnermost(), TemplateIDRange);
    if (Inst.isInvalid())
      return true;

    Cached = new ConstraintSatisfaction(ConstraintOwner,
                                        TemplateArgs.getInnermost());

    if (::CheckConstraintSatisfaction(*this, ConstraintExprs,
        TemplateArgs, TemplateIDRange,
        [&] (SourceLocation PointOfInstantiation,
             SourceRange InstantiationRange,
             TemplateDeductionInfo &DeductionInfo) {
            return std::move(InstantiatingTemplate(*this, PointOfInstantiation,
                InstantiatingTemplate::ConstraintSubstitution{}, Template,
                DeductionInfo, InstantiationRange));
        }, *Cached)) {
      delete Cached;
      return true;
    }

    SatisfactionCache.InsertNode(Cached);
  }

  Satisfaction = *Cached;
  return false;
}

bool Sema::CheckConstraintSatisfaction(NestedRequirement *Req,
    const Expr *ConstraintExpr,
    const MultiLevelTemplateArgumentList &TemplateArgs,
    ConstraintSatisfaction &Satisfaction,
    bool &IsDependent, bool &ContainsUnexpandedParameterPack) {
  IsDependent = false;
  ContainsUnexpandedParameterPack = false;

  InstantiatingTemplate Inst(*this, ConstraintExpr->getBeginLoc(), Req,
      InstantiatingTemplate::ConstraintsCheck{},
      ConstraintExpr->getSourceRange());
  if (Inst.isInvalid())
    return true;

  return ::CheckConstraintSatisfaction(*this, {ConstraintExpr},
      TemplateArgs, ConstraintExpr->getSourceRange(),
      [&] (SourceLocation PointOfInstantiation,
           SourceRange InstantiationRange,
           TemplateDeductionInfo &DeductionInfo) {
          return std::move(InstantiatingTemplate(*this, PointOfInstantiation,
              Req, DeductionInfo, InstantiationRange));
      }, Satisfaction, &ContainsUnexpandedParameterPack, &IsDependent);
}

bool Sema::CheckConstraintSatisfaction(const Expr *ConstraintExpr,
                                       ConstraintSatisfaction &Satisfaction) {
  return calculateConstraintSatisfaction(
      *this, ConstraintExpr, Satisfaction,
      [](const Expr *AtomicExpr) -> ExprResult {
        return ExprResult(const_cast<Expr *>(AtomicExpr));
      });
}

bool Sema::EnsureTemplateArgumentListConstraints(
    TemplateDecl *TD, ArrayRef<TemplateArgument> TemplateArgs,
    SourceRange TemplateIDRange) {
  ConstraintSatisfaction Satisfaction;
  llvm::SmallVector<const Expr *, 3> AssociatedConstraints;
  TD->getAssociatedConstraints(AssociatedConstraints);
  TemplateArgumentList TAL(TemplateArgumentList::OnStack, TemplateArgs);
  MultiLevelTemplateArgumentList MLTAL =
      getTemplateInstantiationArgs(TD, /*Innermost=*/&TAL);
  if (CheckConstraintSatisfaction(TD, TD, AssociatedConstraints,
                                  MLTAL, TemplateIDRange, Satisfaction))
    return true;

  if (!Satisfaction.IsSatisfied) {
    SmallString<128> TemplateArgString;
    TemplateArgString = " ";
    TemplateArgString += getTemplateArgumentBindingsText(
        TD->getTemplateParameters(), TemplateArgs.data(), TemplateArgs.size());

    Diag(TemplateIDRange.getBegin(),
         diag::err_template_arg_list_constraints_not_satisfied)
        << (int)getTemplateNameKindForDiagnostics(TemplateName(TD)) << TD
        << TemplateArgString << TemplateIDRange;
    DiagnoseUnsatisfiedConstraint(Satisfaction);
    return true;
  }
  return false;
}

static void diagnoseWellFormedUnsatisfiedConstraintExpr(Sema &S,
                                                        Expr *SubstExpr,
                                                        bool First = true) {
  if (BinaryOperator *BO = dyn_cast<BinaryOperator>(SubstExpr)) {
    switch (BO->getOpcode()) {
    // These two cases will in practice only be reached when using fold
    // expressions with || and &&, since otherwise the || and && will have been
    // broken down into atomic constraints during satisfaction checking.
    case BO_LOr:
      // Or evaluated to false - meaning both RHS and LHS evaluated to false.
      diagnoseWellFormedUnsatisfiedConstraintExpr(S, BO->getLHS(), First);
      diagnoseWellFormedUnsatisfiedConstraintExpr(S, BO->getRHS(),
                                                  /*First=*/false);
      return;
    case BO_LAnd:
      bool LHSSatisfied;
      BO->getLHS()->EvaluateAsBooleanCondition(LHSSatisfied, S.Context);
      if (LHSSatisfied) {
        // LHS is true, so RHS must be false.
        diagnoseWellFormedUnsatisfiedConstraintExpr(S, BO->getRHS(), First);
        return;
      }
      // LHS is false
      diagnoseWellFormedUnsatisfiedConstraintExpr(S, BO->getLHS(), First);

      // RHS might also be false
      bool RHSSatisfied;
      BO->getRHS()->EvaluateAsBooleanCondition(RHSSatisfied, S.Context);
      if (!RHSSatisfied)
        diagnoseWellFormedUnsatisfiedConstraintExpr(S, BO->getRHS(),
                                                    /*First=*/false);
      return;
    case BO_GE:
    case BO_LE:
    case BO_GT:
    case BO_LT:
    case BO_EQ:
    case BO_NE:
      if (BO->getLHS()->getType()->isIntegerType() &&
          BO->getRHS()->getType()->isIntegerType()) {
        Expr::EvalResult SimplifiedLHS;
        Expr::EvalResult SimplifiedRHS;
        BO->getLHS()->EvaluateAsInt(SimplifiedLHS, S.Context);
        BO->getRHS()->EvaluateAsInt(SimplifiedRHS, S.Context);
        if (!SimplifiedLHS.Diag && ! SimplifiedRHS.Diag) {
          S.Diag(SubstExpr->getBeginLoc(),
                 diag::note_atomic_constraint_evaluated_to_false_elaborated)
              << (int)First << SubstExpr
              << SimplifiedLHS.Val.getInt().toString(10)
              << BinaryOperator::getOpcodeStr(BO->getOpcode())
              << SimplifiedRHS.Val.getInt().toString(10);
          return;
        }
      }
      break;

    default:
      break;
    }
  } else if (ParenExpr *PE = dyn_cast<ParenExpr>(SubstExpr)) {
    diagnoseWellFormedUnsatisfiedConstraintExpr(S, PE->getSubExpr(), First);
    return;
  } else if (auto *CSE = dyn_cast<ConceptSpecializationExpr>(SubstExpr)) {
    if (CSE->getTemplateArgsAsWritten()->NumTemplateArgs == 1) {
      S.Diag(
          CSE->getSourceRange().getBegin(),
          diag::
          note_single_arg_concept_specialization_constraint_evaluated_to_false)
          << (int)First
          << CSE->getTemplateArgsAsWritten()->arguments()[0].getArgument()
          << CSE->getNamedConcept();
    } else {
      S.Diag(SubstExpr->getSourceRange().getBegin(),
             diag::note_concept_specialization_constraint_evaluated_to_false)
          << (int)First << CSE;
    }
    S.DiagnoseUnsatisfiedConstraint(CSE->getSatisfaction());
    return;
  } else if (auto *RE = dyn_cast<RequiresExpr>(SubstExpr)) {
    for (Requirement *Req : RE->getRequirements())
      if (!Req->isDependent() && !Req->isSatisfied()) {
        Req->Diagnose(S, First);
        break;
      }
    return;
  }

  S.Diag(SubstExpr->getSourceRange().getBegin(),
         diag::note_atomic_constraint_evaluated_to_false)
      << (int)First << SubstExpr;
}

template<typename SubstitutionDiagnostic>
static void diagnoseUnsatisfiedConstraintExpr(
    Sema &S, const Expr *E,
    const llvm::PointerUnion<Expr *, SubstitutionDiagnostic *> &Record,
    bool First = true) {
  if (auto *Diag = Record.template dyn_cast<SubstitutionDiagnostic *>()){
    S.Diag(Diag->first, diag::note_substituted_constraint_expr_is_ill_formed)
        << Diag->second;
    return;
  }

  diagnoseWellFormedUnsatisfiedConstraintExpr(S,
      Record.template get<Expr *>(), First);
}

void
Sema::DiagnoseUnsatisfiedConstraint(const ConstraintSatisfaction& Satisfaction,
                                    bool First) {
  assert(!Satisfaction.IsSatisfied &&
         "Attempted to diagnose a satisfied constraint");
  for (auto &Pair : Satisfaction.Details) {
    diagnoseUnsatisfiedConstraintExpr(*this, Pair.first, Pair.second, First);
    First = false;
  }
}

void Sema::DiagnoseUnsatisfiedConstraint(
    const ASTConstraintSatisfaction &Satisfaction,
    bool First) {
  assert(!Satisfaction.IsSatisfied &&
         "Attempted to diagnose a satisfied constraint");
  for (auto &Pair : Satisfaction) {
    diagnoseUnsatisfiedConstraintExpr(*this, Pair.first, Pair.second, First);
    First = false;
  }
}

using NormalForm =
    llvm::SmallVector<llvm::SmallVector<AtomicConstraint *, 2>, 4>;

static NormalForm makeCNF(const NormalizedConstraint &Normalized) {
  if (Normalized.isAtomic())
    return {{Normalized.getAtomicConstraint()}};

  NormalForm LCNF = makeCNF(Normalized.getLHS());
  NormalForm RCNF = makeCNF(Normalized.getRHS());
  if (Normalized.getCompoundKind() == NormalizedConstraint::CCK_Conjunction) {
    LCNF.reserve(LCNF.size() + RCNF.size());
    while (!RCNF.empty())
      LCNF.push_back(std::move(RCNF.pop_back_val()));
    return LCNF;
  }

  // Disjunction
  NormalForm Res;
  Res.reserve(LCNF.size() * RCNF.size());
  for (auto &LDisjunction : LCNF)
    for (auto &RDisjunction : RCNF) {
      NormalForm::value_type Combined;
      Combined.reserve(LDisjunction.size() + RDisjunction.size());
      std::copy(LDisjunction.begin(), LDisjunction.end(),
                std::back_inserter(Combined));
      std::copy(RDisjunction.begin(), RDisjunction.end(),
                std::back_inserter(Combined));
      Res.emplace_back(Combined);
    }
  return Res;
}

static NormalForm makeDNF(const NormalizedConstraint &Normalized) {
  if (Normalized.isAtomic())
    return {{Normalized.getAtomicConstraint()}};

  NormalForm LDNF = makeDNF(Normalized.getLHS());
  NormalForm RDNF = makeDNF(Normalized.getRHS());
  if (Normalized.getCompoundKind() == NormalizedConstraint::CCK_Disjunction) {
    LDNF.reserve(LDNF.size() + RDNF.size());
    while (!RDNF.empty())
      LDNF.push_back(std::move(RDNF.pop_back_val()));
    return LDNF;
  }

  // Conjunction
  NormalForm Res;
  Res.reserve(LDNF.size() * RDNF.size());
  for (auto &LConjunction : LDNF) {
    for (auto &RConjunction : RDNF) {
      NormalForm::value_type Combined;
      Combined.reserve(LConjunction.size() + RConjunction.size());
      std::copy(LConjunction.begin(), LConjunction.end(),
                std::back_inserter(Combined));
      std::copy(RConjunction.begin(), RConjunction.end(),
                std::back_inserter(Combined));
      Res.emplace_back(Combined);
    }
  }
  return Res;
}

template<typename AtomicSubsumptionEvaluator>
static bool subsumes(Sema &S, NormalizedConstraint &PNormalized,
                     NormalizedConstraint &QNormalized, bool &DoesSubsume,
                     AtomicSubsumptionEvaluator E) {
  // C++ [temp.constr.order] p2
  //   In order to determine if a constraint P subsumes a constraint Q, P is
  //   transformed into disjunctive normal form, and Q is transformed into
  //   conjunctive normal form. [...]
  const NormalForm PDNF = makeDNF(PNormalized);
  const NormalForm QCNF = makeCNF(QNormalized);

  // C++ [temp.constr.order] p2
  //   Then, P subsumes Q if and only if, for every disjunctive clause Pi in the
  //   disjunctive normal form of P, Pi subsumes every conjunctive clause Qj in
  //   the conjuctive normal form of Q, where [...]
  for (const auto &Pi : PDNF) {
    for (const auto &Qj : QCNF) {
      // C++ [temp.constr.order] p2
      //   - [...] a disjunctive clause Pi subsumes a conjunctive clause Qj if
      //     and only if there exists an atomic constraint Pia in Pi for which
      //     there exists an atomic constraint, Qjb, in Qj such that Pia
      //     subsumes Qjb.
      bool Found = false;
      for (const AtomicConstraint *Pia : Pi) {
        for (const AtomicConstraint *Qjb : Qj) {
          if (E(*Pia, *Qjb)) {
            Found = true;
            break;
          }
        }
        if (Found)
          break;
      }
      if (!Found) {
        DoesSubsume = false;
        return false;
      }
    }
  }
  DoesSubsume = true;
  return false;
}

bool Sema::IsAtLeastAsConstrained(NamedDecl *D1, ArrayRef<const Expr *> AC1,
                                  NamedDecl *D2, ArrayRef<const Expr *> AC2,
                                  bool *Invalid) {
  if (Invalid)
      *Invalid = false;
  if (AC1.empty())
    return AC2.empty();
  if (AC2.empty())
    // TD1 has associated constraints and TD2 does not.
    return true;

  std::pair<NamedDecl *, NamedDecl *> Key{D1, D2};
  auto CacheEntry = SubsumptionCache.find(Key);
  if (CacheEntry != SubsumptionCache.end())
    return CacheEntry->second;

  MultiLevelTemplateArgumentList MLTAL1 =
      getTemplateInstantiationArgs(cast<Decl>(D1->getDeclContext()));
  MultiLevelTemplateArgumentList MLTAL2 =
      getTemplateInstantiationArgs(cast<Decl>(D2->getDeclContext()));
  bool Subsumes = IsAtLeastAsConstrained(D1, AC1, MLTAL1, D2, AC2, MLTAL2,
                                         Invalid);
  SubsumptionCache.try_emplace(Key, Subsumes);
  return Subsumes;
}

bool
Sema::IsAtLeastAsConstrained(NamedDecl *D1, ArrayRef<const Expr *> AC1,
                             const MultiLevelTemplateArgumentList &MLTAL1,
                             NamedDecl *D2, ArrayRef<const Expr *> AC2,
                             const MultiLevelTemplateArgumentList &MLTAL2,
                             bool *Invalid) {
  if (Invalid)
    *Invalid = false;

  if (AC1.empty())
    return AC2.empty();
  if (AC2.empty())
    // TD1 has associated constraints and TD2 does not.
    return true;

  auto Normalized1 = NormalizedConstraint::fromConstraintExprs(*this, D1, AC1,
                                                               MLTAL1);
  if (!Normalized1) {
    if (Invalid)
      *Invalid = true;
    return false;
  }

  auto Normalized2 = NormalizedConstraint::fromConstraintExprs(*this, D2, AC2,
                                                               MLTAL2);
  if (!Normalized2) {
    if (Invalid)
      *Invalid = true;
    return false;
  }

  bool Subsumes;
  if (subsumes(*this, *Normalized1, *Normalized2, Subsumes,
        [this] (const AtomicConstraint &A, const AtomicConstraint &B) {
          return A.subsumes(Context, B);
        })) {
    if (Invalid)
      *Invalid = true;
    return false;
  }
  return Subsumes;
}

bool Sema::MaybeEmitAmbiguousAtomicConstraintsDiagnostic(NamedDecl *D1,
    ArrayRef<const Expr *> AC1, const MultiLevelTemplateArgumentList &MLTAL1,
    NamedDecl *D2, ArrayRef<const Expr *> AC2,
    const MultiLevelTemplateArgumentList &MLTAL2) {
  if (AC1.empty() || AC2.empty())
    return false;

  auto NormalExprEvaluator =
      [this] (const AtomicConstraint &A, const AtomicConstraint &B) {
        return A.subsumes(Context, B);
      };

  const Expr *AmbiguousAtomic1 = nullptr, *AmbiguousAtomic2 = nullptr;
  auto IdenticalExprEvaluator =
      [&] (const AtomicConstraint &A, const AtomicConstraint &B) {
        if (!A.hasMatchingParameterMapping(Context, B))
          return false;
        const Expr *EA = A.ConstraintExpr, *EB = B.ConstraintExpr;
        if (EA == EB)
          return true;

        // Not the same source level expression - are the expressions
        // identical?
        llvm::FoldingSetNodeID IDA, IDB;
        EA->Profile(IDA, Context, /*Cannonical=*/true);
        EB->Profile(IDB, Context, /*Cannonical=*/true);
        if (IDA != IDB)
          return false;

        AmbiguousAtomic1 = EA;
        AmbiguousAtomic2 = EB;
        return true;
      };

  {
    // The subsumption checks might cause diagnostics
    SFINAETrap Trap(*this);

    auto Normalized1 = NormalizedConstraint::fromConstraintExprs(*this, D1, AC1,
                                                                 MLTAL1);
    if (!Normalized1)
      // Program is ill-formed at this point.
      return false;

    auto Normalized2 = NormalizedConstraint::fromConstraintExprs(*this, D2, AC2,
                                                                 MLTAL2);
    if (!Normalized2)
      // Program is ill-formed at this point.
      return false;

    bool Is1AtLeastAs2Normally, Is2AtLeastAs1Normally;
    if (subsumes(*this, *Normalized1, *Normalized2, Is1AtLeastAs2Normally,
                 NormalExprEvaluator))
      return false;
    if (subsumes(*this, *Normalized2, *Normalized1, Is2AtLeastAs1Normally,
                 NormalExprEvaluator))
      return false;
    bool Is1AtLeastAs2, Is2AtLeastAs1;
    if (subsumes(*this, *Normalized1, *Normalized2, Is1AtLeastAs2,
                 IdenticalExprEvaluator))
      return false;
    if (subsumes(*this, *Normalized2, *Normalized1, Is2AtLeastAs1,
                 IdenticalExprEvaluator))
      return false;
    if (Is1AtLeastAs2 == Is1AtLeastAs2Normally &&
        Is2AtLeastAs1 == Is2AtLeastAs1Normally)
      // Same result - no ambiguity was caused by identical atomic expressions.
      return false;
  }

  // A different result! Some ambiguous atomic constraint(s) caused a difference
  assert(AmbiguousAtomic1 && AmbiguousAtomic2);

  Diag(AmbiguousAtomic1->getBeginLoc(), diag::note_ambiguous_atomic_constraints)
      << const_cast<Expr *>(AmbiguousAtomic1)
      << AmbiguousAtomic1->getSourceRange();
  Diag(AmbiguousAtomic2->getBeginLoc(),
       diag::note_ambiguous_atomic_constraints_second)
      << AmbiguousAtomic2->getSourceRange();
  return true;
}

bool Sema::MaybeEmitAmbiguousAtomicConstraintsDiagnostic(NamedDecl *D1,
    ArrayRef<const Expr *> AC1, NamedDecl *D2, ArrayRef<const Expr *> AC2) {
  if (AC1.empty() || AC2.empty())
    return false;
  MultiLevelTemplateArgumentList MLTAL1 =
      getTemplateInstantiationArgs(cast<Decl>(D1->getDeclContext()));
  MultiLevelTemplateArgumentList MLTAL2 =
      getTemplateInstantiationArgs(cast<Decl>(D2->getDeclContext()));
  return MaybeEmitAmbiguousAtomicConstraintsDiagnostic(D1, AC1, MLTAL1, D2, AC2,
                                                       MLTAL2);
}


ExprRequirement::ExprRequirement(Sema &S, Expr *E, bool IsSimple,
                                 SourceLocation NoexceptLoc,
                                 ReturnTypeRequirement Req) :
    Requirement(IsSimple ? RK_Simple : RK_Compound,
                E->isInstantiationDependent() || Req.isDependent(),
                E->containsUnexpandedParameterPack() ||
                Req.containsUnexpandedParameterPack(), false), Value(E),
    NoexceptLoc(NoexceptLoc), TypeReq(Req) {
  assert((!IsSimple || (Req.isEmpty() && NoexceptLoc.isInvalid())) &&
         "Simple requirement must not have a return type requirement or a "
         "noexcept specification");
  if (isDependent()) {
    Status = SS_Dependent;
    return;
  }
  if (NoexceptLoc.isValid() && S.canThrow(E) == CanThrowResult::CT_Can) {
    Status = SS_NoexceptNotMet;
    setSatisfied(false);
    return;
  }
  Status = TypeReq.calculateSatisfaction(S, E);
  setSatisfied(Status == SS_Satisfied);
}

ExprRequirement::ExprRequirement(Expr *E, bool IsSimple,
                                 SourceLocation NoexceptLoc,
                                 ReturnTypeRequirement Req,
                                 SatisfactionStatus Status) :
    Requirement(IsSimple ? RK_Simple : RK_Compound, Status == SS_Dependent,
                Status == SS_Dependent &&
                (E->containsUnexpandedParameterPack() ||
                 Req.containsUnexpandedParameterPack()),
                Status == SS_Satisfied), Value(E), NoexceptLoc(NoexceptLoc),
    TypeReq(Req), Status(Status) {
  assert((!IsSimple || (Req.isEmpty() && NoexceptLoc.isInvalid())) &&
         "Simple requirement must not have a return type requirement or a "
         "noexcept specification");
}

ExprRequirement::ExprRequirement(SubstitutionDiagnostic *ExprSubstDiag,
                                 bool IsSimple, SourceLocation NoexceptLoc,
                                 ReturnTypeRequirement Req) :
    Requirement(IsSimple ? RK_Simple : RK_Compound, Req.isDependent(),
                Req.containsUnexpandedParameterPack(), /*IsSatisfied=*/false),
    Value(ExprSubstDiag), NoexceptLoc(NoexceptLoc), TypeReq(Req),
    Status(SS_ExprSubstitutionFailure) {
  assert((!IsSimple || (Req.isEmpty() && NoexceptLoc.isInvalid())) &&
         "Simple requirement must not have a return type requirement or a "
         "noexcept specification");
}

ExprRequirement::ReturnTypeRequirement::ReturnTypeRequirement(ASTContext &C,
    TypeSourceInfo *ExpectedType) :
    Dependent(ExpectedType->getType()->isInstantiationDependentType()),
    ContainsUnexpandedParameterPack(
        ExpectedType->getType()->containsUnexpandedParameterPack()),
    Value(ExpectedType) {}

ExprRequirement::ReturnTypeRequirement::ReturnTypeRequirement(ASTContext &C,
    TemplateParameterList *TPL, ConceptSpecializationExpr *CSE) :
    Dependent(false), ContainsUnexpandedParameterPack(false),
    Value(new (C) TypeConstraintRequirement(TPL, CSE)) {
  assert(TPL->size() == 1);
  const TypeConstraint *TC =
      cast<TemplateTypeParmDecl>(TPL->getParam(0))->getTypeConstraint();
  assert(TC &&
         "TPL must have a template type parameter with a type constraint");
  auto *Constraint =
      cast_or_null<ConceptSpecializationExpr>(
          TC->getImmediatelyDeclaredConstraint());
  ContainsUnexpandedParameterPack =
      Constraint->containsUnexpandedParameterPack();
  for (auto &ArgLoc :
       Constraint->getTemplateArgsAsWritten()->arguments().drop_front(1))
    if (ArgLoc.getArgument().isDependent()) {
      Dependent = true;
      break;
    }
}

ExprRequirement::SatisfactionStatus
ExprRequirement::ReturnTypeRequirement::calculateSatisfaction(Sema &S,
    Expr *E) {
  if (!Value)
    return SS_Satisfied;
  if (Value.is<SubstitutionDiagnostic *>())
    return SS_TypeRequirementSubstitutionFailure;
  if (auto *TypeReq = Value.dyn_cast<TypeSourceInfo *>()) {
    InitializedEntity InventedEntity =
        InitializedEntity::InitializeResult(TypeReq->getTypeLoc().getBeginLoc(),
                                            TypeReq->getType(), /*NRVO=*/false);
    InitializationSequence Seq(S, InventedEntity,
        InitializationKind::CreateCopy(E->getBeginLoc(),
                                       TypeReq->getTypeLoc().getBeginLoc()), E);
    if (Seq.isAmbiguous())
      return SS_ImplicitConversionAmbiguous;
    if (Seq.Failed())
      return SS_NoImplicitConversionExists;
    return SS_Satisfied;
  }
  auto *TypeConstr = Value.get<TypeConstraintRequirement *>();
  TemplateParameterList *TPL = TypeConstr->first;

  // C++2a [expr.prim.req]p1.3.3
  //     The immediately-declared constraint ([temp]) of decltype((E)) shall be
  //     satisfied.
  QualType MatchedType =
      S.BuildDecltypeType(E, E->getBeginLoc()).getCanonicalType();
  llvm::SmallVector<TemplateArgument, 1> Args;
  Args.push_back(TemplateArgument(MatchedType));
  TemplateArgumentList TAL(TemplateArgumentList::OnStack, Args);
  MultiLevelTemplateArgumentList MLTAL(TAL);
  for (unsigned I = 0; I < TPL->getDepth(); ++I)
    MLTAL.addOuterRetainedLevel();
  ExprResult Constraint =
      S.SubstExpr(cast<TemplateTypeParmDecl>(
          TPL->getParam(0))->getTypeConstraint()
          ->getImmediatelyDeclaredConstraint(), MLTAL);
  assert(!Constraint.isInvalid() && Constraint.isUsable() &&
         "Substitution cannot fail as it is simply putting a type template "
         "argument into a concept specialization expression's parameter.");

  auto *CSE = cast<ConceptSpecializationExpr>(Constraint.get());
  TypeConstr->second = CSE;
  if (!CSE->isSatisfied())
    return SS_ConstraintsNotSatisfied;
  return SS_Satisfied;
}

void ExprRequirement::Diagnose(Sema &S, bool First) const {
  assert(!isSatisfied()
         && "Diagnose() can only be used on an unsatisfied requirement");
  switch (getSatisfactionStatus()) {
  case SS_Dependent:
    llvm_unreachable("Diagnosing a dependent requirement");
    break;
  case SS_ExprSubstitutionFailure: {
    auto *SubstDiag = getExprSubstitutionDiagnostic();
    if (!SubstDiag->DiagMessage.empty())
      S.Diag(SubstDiag->DiagLoc,
             diag::note_expr_requirement_expr_substitution_error) << (int)First
          << SubstDiag->SubstitutedEntity
          << SubstDiag->DiagMessage;
    else
      S.Diag(SubstDiag->DiagLoc,
             diag::note_expr_requirement_expr_unknown_substitution_error)
            << (int)First << SubstDiag->SubstitutedEntity;
    break;
  }
  case SS_NoexceptNotMet:
    S.Diag(getNoexceptLoc(), diag::note_expr_requirement_noexcept_not_met)
        << (int)First << getExpr();
    break;
  case SS_TypeRequirementSubstitutionFailure: {
    auto *SubstDiag = TypeReq.getSubstitutionDiagnostic();
    if (!SubstDiag->DiagMessage.empty())
      S.Diag(SubstDiag->DiagLoc,
             diag::note_expr_requirement_type_requirement_substitution_error)
          << (int)First << SubstDiag->SubstitutedEntity
          << SubstDiag->DiagMessage;
    else
      S.Diag(SubstDiag->DiagLoc,
        diag::note_expr_requirement_type_requirement_unknown_substitution_error)
          << (int)First << SubstDiag->SubstitutedEntity;
    break;
  }
  case SS_ImplicitConversionAmbiguous:
    S.Diag(TypeReq.getTrailingReturnTypeExpectedType()
               ->getTypeLoc().getBeginLoc(),
           diag::note_expr_requirement_ambiguous_conversion) << (int)First
        << getExpr()->getType()
        << TypeReq.getTrailingReturnTypeExpectedType()
            ->getType();
    break;
  case SS_NoImplicitConversionExists:
    S.Diag(TypeReq.getTrailingReturnTypeExpectedType()
               ->getTypeLoc().getBeginLoc(),
           diag::note_expr_requirement_no_implicit_conversion) << (int)First
        << getExpr()->getType()
        << TypeReq.getTrailingReturnTypeExpectedType()
            ->getType();
    break;
  case SS_ConstraintsNotSatisfied: {
    ConceptSpecializationExpr *ConstraintExpr =
        TypeReq.getTypeConstraintSubstitutedConstraintExpr();
    if (ConstraintExpr->getTemplateArgsAsWritten()->NumTemplateArgs == 1)
      // A simple case - expr type is the type being constrained and the concept
      // was not provided arguments.
      S.Diag(ConstraintExpr->getBeginLoc(),
           diag::note_expr_requirement_constraints_not_satisfied_simple)
          << (int)First << getExpr()->getType()
          << ConstraintExpr->getNamedConcept();
    else
      S.Diag(ConstraintExpr->getBeginLoc(),
             diag::note_expr_requirement_constraints_not_satisfied)
          << (int)First << ConstraintExpr;
    S.DiagnoseUnsatisfiedConstraint(ConstraintExpr->getSatisfaction());
    break;
  }
  case SS_Satisfied:
    llvm_unreachable("We checked this above");
  }
}

TypeRequirement::TypeRequirement(TypeSourceInfo *T) :
    Requirement(RK_Type, T->getType()->isDependentType(),
                T->getType()->containsUnexpandedParameterPack(),
                /*IsSatisfied=*/true // We reach this ctor with either dependent
                                     // types (in which IsSatisfied doesn't
                                     // matter) or with non-dependent type in
                                     // which the existance of the type
                                     // indicates satisfaction.
                ), Value(T),
    Status(T->getType()->isDependentType() ? SS_Dependent : SS_Satisfied) {}

void TypeRequirement::Diagnose(Sema &S, bool First) const {
  assert(!isSatisfied()
         && "Diagnose() can only be used on an unsatisfied requirement");
  switch (getSatisfactionStatus()) {
  case SS_Dependent:
    llvm_unreachable("Diagnosing a dependent requirement");
    return;
  case SS_SubstitutionFailure: {
    auto *SubstDiag = getSubstitutionDiagnostic();
    if (!SubstDiag->DiagMessage.empty())
      S.Diag(SubstDiag->DiagLoc,
             diag::note_type_requirement_substitution_error) << (int)First
          << SubstDiag->SubstitutedEntity << SubstDiag->DiagMessage;
    else
      S.Diag(SubstDiag->DiagLoc,
             diag::note_type_requirement_unknown_substitution_error)
          << (int)First << SubstDiag->SubstitutedEntity;
    return;
  }
  default:
    llvm_unreachable("Unknown satisfaction status");
    return;
  }
}

NestedRequirement::NestedRequirement(Sema &S, Expr *Constraint,
    const MultiLevelTemplateArgumentList &TemplateArgs) :
    Requirement(RK_Nested,
                /*(set below)Dependent=*/false,
                /*(set below)ContainsUnexpandedParameterPack=*/false,
                /*(set below)Satisfied=*/false), ConstraintExpr(Constraint) {
  ConstraintSatisfaction Satisfaction;
  if (TemplateArgs.getNumLevels() == 0)
    S.CheckConstraintSatisfaction(Constraint, Satisfaction);
  else {
    bool IsDependent, ContainsUnexpandedParameterPack;
    S.CheckConstraintSatisfaction(this, Constraint, TemplateArgs,
        Satisfaction, IsDependent, ContainsUnexpandedParameterPack);
    setDependent(IsDependent);
    setContainsUnexpandedParameterPack(ContainsUnexpandedParameterPack);
  }
  this->Satisfaction = ASTConstraintSatisfaction::Create(S.Context,
      Satisfaction.IsSatisfied, Satisfaction.Details);
  setSatisfied(Satisfaction.IsSatisfied);
}

void NestedRequirement::Diagnose(Sema &S, bool First) const {
  S.DiagnoseUnsatisfiedConstraint(*Satisfaction, First);
}

void
ConstraintSatisfaction::Profile(llvm::FoldingSetNodeID &ID, const ASTContext &C,
                                NamedDecl *ConstraintOwner,
                                ArrayRef<TemplateArgument> TemplateArgs) {
  ID.AddPointer(ConstraintOwner->getCanonicalDecl());
  for (const TemplateArgument &Arg : TemplateArgs)
    Arg.Profile(ID, C);
}

ConstraintSatisfaction::ConstraintSatisfaction(ConstraintSatisfaction &Other):
  IsSatisfied(Other.IsSatisfied) {
  for (UnsatisfiedConstraintRecord &Record : Details)
    if (auto *Diag = Record.second.dyn_cast<PartialDiagnosticAt *>())
      Details.emplace_back(Record.first, new PartialDiagnosticAt(*Diag));
    else
      Details.push_back(Record);
}

ConstraintSatisfaction::~ConstraintSatisfaction() {
  //for (UnsatisfiedConstraintRecord &Record : Details)
  //  if (auto *Diag = Record.second.dyn_cast<PartialDiagnosticAt *>())
  //    delete Diag;
}

llvm::Optional<NormalizedConstraint>
Sema::getNormalizedAssociatedConstraints(NamedDecl *TemplateLike) {
  assert(isa<TemplateDecl>(TemplateLike) ||
         isa<VarTemplatePartialSpecializationDecl>(TemplateLike) ||
         isa<ClassTemplatePartialSpecializationDecl>(TemplateLike));
  llvm::SmallVector<const Expr *, 3> AssociatedConstraints;
  if (auto *TD = dyn_cast<TemplateDecl>(TemplateLike))
    TD->getAssociatedConstraints(AssociatedConstraints);
  else if (auto *C =
               dyn_cast<ClassTemplatePartialSpecializationDecl>(TemplateLike))
    C->getAssociatedConstraints(AssociatedConstraints);
  else
    cast<VarTemplatePartialSpecializationDecl>(TemplateLike)
        ->getAssociatedConstraints(AssociatedConstraints);
  MultiLevelTemplateArgumentList TemplateArgs =
      getTemplateInstantiationArgs(cast<Decl>(TemplateLike->getDeclContext()));
  return NormalizedConstraint::fromConstraintExprs(*this, TemplateLike,
                                                   AssociatedConstraints,
                                                   TemplateArgs);
}

llvm::Optional<NormalizedConstraint>
NormalizedConstraint::fromConstraintExpr(Sema &S,
    NamedDecl *ConstrainedEntity, SourceLocation PointOfInstantiation,
    const Expr *E,
    const MultiLevelTemplateArgumentList &ParameterMapping) {
  assert(E != nullptr);

  // C++ [temp.constr.normal]p1.1
  // [...]
  // - The normal form of an expression (E) is the normal form of E.
  // [...]
  if (auto *P = dyn_cast<const ParenExpr>(E))
    return fromConstraintExpr(S, ConstrainedEntity, PointOfInstantiation,
                              P->getSubExpr(), ParameterMapping);
  if (auto *BO = dyn_cast<const BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr) {
      auto LHS = fromConstraintExpr(S, ConstrainedEntity,
                                    PointOfInstantiation, BO->getLHS(),
                                    ParameterMapping);
      if (!LHS)
        return llvm::Optional<NormalizedConstraint>{};
      auto RHS = fromConstraintExpr(S, ConstrainedEntity,
                                    PointOfInstantiation, BO->getRHS(),
                                    ParameterMapping);
      if (!RHS)
        return llvm::Optional<NormalizedConstraint>{};

      return NormalizedConstraint(
          S.Context, std::move(*LHS), std::move(*RHS),
          BO->getOpcode() == BO_LAnd ? CCK_Conjunction : CCK_Disjunction);
    }
  } else if (auto *CSE = dyn_cast<const ConceptSpecializationExpr>(E)) {
    // C++ [temp.constr.normal]p1.1
    // [...]
    // The normal form of an id-expression of the form C<A1, A2, ..., AN>,
    // where C names a concept, is the normal form of the
    // constraint-expression of C, after substituting A1, A2, ..., AN for C’s
    // respective template parameters in the parameter mappings in each atomic
    // constraint. If any such substitution results in an invalid type or
    // expression, the program is ill-formed; no diagnostic is required.
    // [...]
    const ASTTemplateArgumentListInfo *Mapping =
        CSE->getTemplateArgsAsWritten();

    // template<typename T1, typename T2>
    // concept C1 = true;
    //
    // template<typename T, typename U>
    // concept C2 = C1<U, T>; -> We are here.
    //                           Mapping is {T1=U, T2=T}
    //                           ParameterMapping is {T=X, U=Y}
    //
    // template<typename X, typename Y>
    // void foo() requires C2<X, Y> {}
    //
    // We would like to substitute ParameterMapping into Mapping, to get
    // ParameterMapping={T1=Y, T2=X} for the next level down.
    // Instead of doing the direct substitution of ParameterMapping into
    // Mapping, we instead substitute ParameterMapping into C1<U, T> and take
    // the substituted argument list as the ParameterMapping for the next
    // level down.

    llvm::Optional<Sema::InstantiatingTemplate> Inst;

    if (ParameterMapping.getNumLevels() != 0)
        Inst.emplace(S, PointOfInstantiation,
            Sema::InstantiatingTemplate::ConstraintNormalization{},
            ConstrainedEntity, ParameterMapping.getInnermost(),
            SourceRange(PointOfInstantiation));

    ExprResult Result = S.SubstExpr(
        const_cast<ConceptSpecializationExpr *>(CSE), ParameterMapping);
    if (!Result.isUsable() || Result.isInvalid())
      // C++ [temp.constr.normal]
      // If any such substitution results in an invalid type or
      // expression, the program is ill-formed; no diagnostic is required.
      return llvm::Optional<NormalizedConstraint>{};

    Mapping = cast<ConceptSpecializationExpr>(Result.get())
                         ->getTemplateArgsAsWritten();

    TemplateArgumentListInfo SubstTALI(Mapping->LAngleLoc,
                                       Mapping->RAngleLoc);
    for (auto &Arg : Mapping->arguments())
      SubstTALI.addArgument(Arg);
    llvm::SmallVector<TemplateArgument, 4> Converted;
    bool InstantiationDependent;
    bool Failure = S.CheckTemplateArgumentList(
        CSE->getNamedConcept(), CSE->getBeginLoc(), SubstTALI,
        /*PartialTemplateArgs=*/false, Converted,
        /*UpdateArgsWithConversions=*/true, &InstantiationDependent);
    MultiLevelTemplateArgumentList MLTAL;
    MLTAL.addOuterTemplateArguments(Converted);

    // The case is this:
    //
    // template<typename T, typename U>
    // concept C1 = true;
    //
    // template<typename... Ts>
    // concept C2 = C1<Ts...>; // After substituting Ts = {T}, the
    //                         // resulting argument list does not match
    //                         // the parameter list.
    //
    // template<typename T>
    // void foo() requires C2<T> {}
    //
    // This case should be checked when substituting into C1<Ts...>, and will
    // be caught by the if above.
    assert(!Failure &&
           "Template argument list match should have been checked during "
           "substitution.");
    if (InstantiationDependent)
      // The case is this:
      //
      // template<typename T, typename U>
      // concept C1 = true;
      //
      // template<typename... Us>
      // concept C2 = C1<Us...>; // After substituting Us = {Ts}, we cannot
      //                         // match arguments to parameters.
      //
      // template<typename... Ts>
      // void foo() requires C2<T...> {}
      //
      // Treat the CSE as an atomic expression.
      return NormalizedConstraint{new (S.Context)
                                      AtomicConstraint(E, Converted)};

    return fromConstraintExpr(S, CSE->getNamedConcept(), CSE->getBeginLoc(),
                              CSE->getNamedConcept()->getConstraintExpr(),
                              MLTAL);
  }
  return NormalizedConstraint{
      new (S.Context) AtomicConstraint(E,
          ParameterMapping.getNumLevels() != 0 ?
          ParameterMapping.getInnermost() : ArrayRef<TemplateArgument>{})};
}

llvm::Optional<NormalizedConstraint>
NormalizedConstraint::fromConstraintExprs(Sema &S,
    NamedDecl *ConstrainedEntity, ArrayRef<const Expr *> E,
    const MultiLevelTemplateArgumentList &ParameterMapping) {
  LocalInstantiationScope InstScope(S);
  if (auto *FD = dyn_cast<FunctionDecl>(ConstrainedEntity))
    if (FD->getTrailingRequiresClause() &&
        ParameterMapping.getNumLevels() != 0) {
      FunctionDecl *Pattern =
          FD->getDescribedFunctionTemplate() ?
          FD->getDescribedFunctionTemplate()
              ->getInstantiatedFromMemberTemplate()->getTemplatedDecl() :
          FD->getInstantiatedFromMemberFunction() ?
          FD->getInstantiatedFromMemberFunction() :
          FD->getPrimaryTemplate()->getTemplatedDecl();
      for (unsigned I = 0; I < FD->param_size(); ++I)
        InstScope.InstantiatedLocal(Pattern->getParamDecl(I),
                                    FD->getParamDecl(I));
    }

  assert(E.size() != 0);
  auto First = fromConstraintExpr(S, ConstrainedEntity,
                                  ConstrainedEntity->getLocation(), E[0],
                                  ParameterMapping);
  if (E.size() == 1)
    return First;
  auto Second = fromConstraintExpr(S, ConstrainedEntity,
                                   ConstrainedEntity->getLocation(), E[1],
                                   ParameterMapping);
  if (!Second)
    return llvm::Optional<NormalizedConstraint>{};
  llvm::Optional<NormalizedConstraint> Conjunction;
  Conjunction.emplace(S.Context, std::move(*First), std::move(*Second),
                      CCK_Conjunction);
  for (unsigned I = 2; I < E.size(); ++I) {
    auto Next = fromConstraintExpr(S, ConstrainedEntity,
                                   ConstrainedEntity->getLocation(), E[I],
                                   ParameterMapping);
    if (!Next)
      return llvm::Optional<NormalizedConstraint>{};
    NormalizedConstraint NewConjunction(S.Context, std::move(*Conjunction),
                                        std::move(*Next), CCK_Conjunction);
    *Conjunction = std::move(NewConjunction);
  }
  return Conjunction;
}