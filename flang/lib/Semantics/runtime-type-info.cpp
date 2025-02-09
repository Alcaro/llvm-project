//===-- lib/Semantics/runtime-type-info.cpp ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "flang/Semantics/runtime-type-info.h"
#include "mod-file.h"
#include "flang/Evaluate/fold-designator.h"
#include "flang/Evaluate/fold.h"
#include "flang/Evaluate/tools.h"
#include "flang/Evaluate/type.h"
#include "flang/Semantics/scope.h"
#include "flang/Semantics/tools.h"
#include <list>
#include <map>
#include <string>

namespace Fortran::semantics {

static int FindLenParameterIndex(
    const SymbolVector &parameters, const Symbol &symbol) {
  int lenIndex{0};
  for (SymbolRef ref : parameters) {
    if (&*ref == &symbol) {
      return lenIndex;
    }
    if (ref->get<TypeParamDetails>().attr() == common::TypeParamAttr::Len) {
      ++lenIndex;
    }
  }
  DIE("Length type parameter not found in parameter order");
  return -1;
}

class RuntimeTableBuilder {
public:
  RuntimeTableBuilder(SemanticsContext &, RuntimeDerivedTypeTables &);
  void DescribeTypes(Scope &scope);

private:
  const Symbol *DescribeType(Scope &);
  const Symbol &GetSchemaSymbol(const char *) const;
  const DeclTypeSpec &GetSchema(const char *) const;
  SomeExpr GetEnumValue(const char *) const;
  Symbol &CreateObject(const std::string &, const DeclTypeSpec &, Scope &);
  // The names of created symbols are saved in and owned by the
  // RuntimeDerivedTypeTables instance returned by
  // BuildRuntimeDerivedTypeTables() so that references to those names remain
  // valid for lowering.
  SourceName SaveObjectName(const std::string &);
  SomeExpr SaveNameAsPointerTarget(Scope &, const std::string &);
  const SymbolVector *GetTypeParameters(const Symbol &);
  evaluate::StructureConstructor DescribeComponent(const Symbol &,
      const ObjectEntityDetails &, Scope &, const std::string &distinctName,
      const SymbolVector *parameters);
  evaluate::StructureConstructor DescribeComponent(
      const Symbol &, const ProcEntityDetails &, Scope &);
  evaluate::StructureConstructor PackageIntValue(
      const SomeExpr &genre, std::int64_t = 0) const;
  SomeExpr PackageIntValueExpr(const SomeExpr &genre, std::int64_t = 0) const;
  std::vector<const Symbol *> CollectBindings(const Scope &dtScope) const;
  std::vector<evaluate::StructureConstructor> DescribeBindings(
      const Scope &dtScope, Scope &);
  void DescribeGeneric(
      const GenericDetails &, std::vector<evaluate::StructureConstructor> &);
  void DescribeSpecialProc(std::vector<evaluate::StructureConstructor> &,
      const Symbol &specificOrBinding, bool isAssignment, bool isFinal,
      std::optional<GenericKind::DefinedIo>);
  void IncorporateDefinedIoGenericInterfaces(
      std::vector<evaluate::StructureConstructor> &, SourceName,
      GenericKind::DefinedIo, const Scope *);

  // Instantiated for ParamValue and Bound
  template <typename A>
  evaluate::StructureConstructor GetValue(
      const A &x, const SymbolVector *parameters) {
    if (x.isExplicit()) {
      return GetValue(x.GetExplicit(), parameters);
    } else {
      return PackageIntValue(deferredEnum_);
    }
  }

  // Specialization for optional<Expr<SomeInteger and SubscriptInteger>>
  template <typename T>
  evaluate::StructureConstructor GetValue(
      const std::optional<evaluate::Expr<T>> &expr,
      const SymbolVector *parameters) {
    if (auto constValue{evaluate::ToInt64(expr)}) {
      return PackageIntValue(explicitEnum_, *constValue);
    }
    if (expr) {
      if (parameters) {
        if (const Symbol * lenParam{evaluate::ExtractBareLenParameter(*expr)}) {
          return PackageIntValue(
              lenParameterEnum_, FindLenParameterIndex(*parameters, *lenParam));
        }
      }
      context_.Say(location_,
          "Specification expression '%s' is neither constant nor a length "
          "type parameter"_err_en_US,
          expr->AsFortran());
    }
    return PackageIntValue(deferredEnum_);
  }

  SemanticsContext &context_;
  RuntimeDerivedTypeTables &tables_;
  std::map<const Symbol *, SymbolVector> orderedTypeParameters_;
  int anonymousTypes_{0};

  const DeclTypeSpec &derivedTypeSchema_; // TYPE(DerivedType)
  const DeclTypeSpec &componentSchema_; // TYPE(Component)
  const DeclTypeSpec &procPtrSchema_; // TYPE(ProcPtrComponent)
  const DeclTypeSpec &valueSchema_; // TYPE(Value)
  const DeclTypeSpec &bindingSchema_; // TYPE(Binding)
  const DeclTypeSpec &specialSchema_; // TYPE(SpecialBinding)
  SomeExpr deferredEnum_; // Value::Genre::Deferred
  SomeExpr explicitEnum_; // Value::Genre::Explicit
  SomeExpr lenParameterEnum_; // Value::Genre::LenParameter
  SomeExpr assignmentEnum_; // SpecialBinding::Which::Assignment
  SomeExpr
      elementalAssignmentEnum_; // SpecialBinding::Which::ElementalAssignment
  SomeExpr finalEnum_; // SpecialBinding::Which::Final
  SomeExpr elementalFinalEnum_; // SpecialBinding::Which::ElementalFinal
  SomeExpr assumedRankFinalEnum_; // SpecialBinding::Which::AssumedRankFinal
  SomeExpr readFormattedEnum_; // SpecialBinding::Which::ReadFormatted
  SomeExpr readUnformattedEnum_; // SpecialBinding::Which::ReadUnformatted
  SomeExpr writeFormattedEnum_; // SpecialBinding::Which::WriteFormatted
  SomeExpr writeUnformattedEnum_; // SpecialBinding::Which::WriteUnformatted
  parser::CharBlock location_;
};

RuntimeTableBuilder::RuntimeTableBuilder(
    SemanticsContext &c, RuntimeDerivedTypeTables &t)
    : context_{c}, tables_{t}, derivedTypeSchema_{GetSchema("derivedtype")},
      componentSchema_{GetSchema("component")}, procPtrSchema_{GetSchema(
                                                    "procptrcomponent")},
      valueSchema_{GetSchema("value")}, bindingSchema_{GetSchema("binding")},
      specialSchema_{GetSchema("specialbinding")}, deferredEnum_{GetEnumValue(
                                                       "deferred")},
      explicitEnum_{GetEnumValue("explicit")}, lenParameterEnum_{GetEnumValue(
                                                   "lenparameter")},
      assignmentEnum_{GetEnumValue("assignment")},
      elementalAssignmentEnum_{GetEnumValue("elementalassignment")},
      finalEnum_{GetEnumValue("final")}, elementalFinalEnum_{GetEnumValue(
                                             "elementalfinal")},
      assumedRankFinalEnum_{GetEnumValue("assumedrankfinal")},
      readFormattedEnum_{GetEnumValue("readformatted")},
      readUnformattedEnum_{GetEnumValue("readunformatted")},
      writeFormattedEnum_{GetEnumValue("writeformatted")},
      writeUnformattedEnum_{GetEnumValue("writeunformatted")} {}

void RuntimeTableBuilder::DescribeTypes(Scope &scope) {
  if (&scope == tables_.schemata) {
    return; // don't loop trying to describe a schema...
  }
  if (scope.IsDerivedType()) {
    DescribeType(scope);
  } else {
    for (Scope &child : scope.children()) {
      DescribeTypes(child);
    }
  }
}

// Returns derived type instantiation's parameters in declaration order
const SymbolVector *RuntimeTableBuilder::GetTypeParameters(
    const Symbol &symbol) {
  auto iter{orderedTypeParameters_.find(&symbol)};
  if (iter != orderedTypeParameters_.end()) {
    return &iter->second;
  } else {
    return &orderedTypeParameters_
                .emplace(&symbol, OrderParameterDeclarations(symbol))
                .first->second;
  }
}

static Scope &GetContainingNonDerivedScope(Scope &scope) {
  Scope *p{&scope};
  while (p->IsDerivedType()) {
    p = &p->parent();
  }
  return *p;
}

static const Symbol &GetSchemaField(
    const DerivedTypeSpec &derived, const std::string &name) {
  const Scope &scope{
      DEREF(derived.scope() ? derived.scope() : derived.typeSymbol().scope())};
  auto iter{scope.find(SourceName(name))};
  CHECK(iter != scope.end());
  return *iter->second;
}

static const Symbol &GetSchemaField(
    const DeclTypeSpec &derived, const std::string &name) {
  return GetSchemaField(DEREF(derived.AsDerived()), name);
}

static evaluate::StructureConstructorValues &AddValue(
    evaluate::StructureConstructorValues &values, const DeclTypeSpec &spec,
    const std::string &name, SomeExpr &&x) {
  values.emplace(GetSchemaField(spec, name), std::move(x));
  return values;
}

static evaluate::StructureConstructorValues &AddValue(
    evaluate::StructureConstructorValues &values, const DeclTypeSpec &spec,
    const std::string &name, const SomeExpr &x) {
  values.emplace(GetSchemaField(spec, name), x);
  return values;
}

static SomeExpr IntToExpr(std::int64_t n) {
  return evaluate::AsGenericExpr(evaluate::ExtentExpr{n});
}

static evaluate::StructureConstructor Structure(
    const DeclTypeSpec &spec, evaluate::StructureConstructorValues &&values) {
  return {DEREF(spec.AsDerived()), std::move(values)};
}

static SomeExpr StructureExpr(evaluate::StructureConstructor &&x) {
  return SomeExpr{evaluate::Expr<evaluate::SomeDerived>{std::move(x)}};
}

static int GetIntegerKind(const Symbol &symbol) {
  auto dyType{evaluate::DynamicType::From(symbol)};
  CHECK(dyType && dyType->category() == TypeCategory::Integer);
  return dyType->kind();
}

// Save a rank-1 array constant of some numeric type as an
// initialized data object in a scope.
template <typename T>
static SomeExpr SaveNumericPointerTarget(
    Scope &scope, SourceName name, std::vector<typename T::Scalar> &&x) {
  if (x.empty()) {
    return SomeExpr{evaluate::NullPointer{}};
  } else {
    ObjectEntityDetails object;
    if (const auto *spec{scope.FindType(
            DeclTypeSpec{NumericTypeSpec{T::category, KindExpr{T::kind}}})}) {
      object.set_type(*spec);
    } else {
      object.set_type(scope.MakeNumericType(T::category, KindExpr{T::kind}));
    }
    auto elements{static_cast<evaluate::ConstantSubscript>(x.size())};
    ArraySpec arraySpec;
    arraySpec.push_back(ShapeSpec::MakeExplicit(Bound{0}, Bound{elements - 1}));
    object.set_shape(arraySpec);
    object.set_init(evaluate::AsGenericExpr(evaluate::Constant<T>{
        std::move(x), evaluate::ConstantSubscripts{elements}}));
    const Symbol &symbol{
        *scope
             .try_emplace(
                 name, Attrs{Attr::TARGET, Attr::SAVE}, std::move(object))
             .first->second};
    return evaluate::AsGenericExpr(
        evaluate::Expr<T>{evaluate::Designator<T>{symbol}});
  }
}

// Save an arbitrarily shaped array constant of some derived type
// as an initialized data object in a scope.
static SomeExpr SaveDerivedPointerTarget(Scope &scope, SourceName name,
    std::vector<evaluate::StructureConstructor> &&x,
    evaluate::ConstantSubscripts &&shape) {
  if (x.empty()) {
    return SomeExpr{evaluate::NullPointer{}};
  } else {
    const auto &derivedType{x.front().GetType().GetDerivedTypeSpec()};
    ObjectEntityDetails object;
    DeclTypeSpec typeSpec{DeclTypeSpec::TypeDerived, derivedType};
    if (const DeclTypeSpec * spec{scope.FindType(typeSpec)}) {
      object.set_type(*spec);
    } else {
      object.set_type(scope.MakeDerivedType(
          DeclTypeSpec::TypeDerived, common::Clone(derivedType)));
    }
    if (!shape.empty()) {
      ArraySpec arraySpec;
      for (auto n : shape) {
        arraySpec.push_back(ShapeSpec::MakeExplicit(Bound{0}, Bound{n - 1}));
      }
      object.set_shape(arraySpec);
    }
    object.set_init(
        evaluate::AsGenericExpr(evaluate::Constant<evaluate::SomeDerived>{
            derivedType, std::move(x), std::move(shape)}));
    const Symbol &symbol{
        *scope
             .try_emplace(
                 name, Attrs{Attr::TARGET, Attr::SAVE}, std::move(object))
             .first->second};
    return evaluate::AsGenericExpr(
        evaluate::Designator<evaluate::SomeDerived>{symbol});
  }
}

static SomeExpr SaveObjectInit(
    Scope &scope, SourceName name, const ObjectEntityDetails &object) {
  const Symbol &symbol{*scope
                            .try_emplace(name, Attrs{Attr::TARGET, Attr::SAVE},
                                ObjectEntityDetails{object})
                            .first->second};
  CHECK(symbol.get<ObjectEntityDetails>().init().has_value());
  return evaluate::AsGenericExpr(
      evaluate::Designator<evaluate::SomeDerived>{symbol});
}

const Symbol *RuntimeTableBuilder::DescribeType(Scope &dtScope) {
  if (const Symbol * info{dtScope.runtimeDerivedTypeDescription()}) {
    return info;
  }
  const DerivedTypeSpec *derivedTypeSpec{dtScope.derivedTypeSpec()};
  const Symbol *dtSymbol{
      derivedTypeSpec ? &derivedTypeSpec->typeSymbol() : dtScope.symbol()};
  if (!dtSymbol) {
    return nullptr;
  }
  auto locationRestorer{common::ScopedSet(location_, dtSymbol->name())};
  // Check for an existing description that can be imported from a USE'd module
  std::string typeName{dtSymbol->name().ToString()};
  if (typeName.empty() || typeName[0] == '.') {
    return nullptr;
  }
  std::string distinctName{typeName};
  if (&dtScope != dtSymbol->scope()) {
    distinctName += "."s + std::to_string(anonymousTypes_++);
  }
  std::string dtDescName{".dt."s + distinctName};
  Scope &scope{GetContainingNonDerivedScope(dtScope)};
  if (distinctName == typeName && scope.IsModule()) {
    if (const Symbol * description{scope.FindSymbol(SourceName{dtDescName})}) {
      dtScope.set_runtimeDerivedTypeDescription(*description);
      return description;
    }
  }
  // Create a new description object before populating it so that mutual
  // references will work as pointer targets.
  Symbol &dtObject{CreateObject(dtDescName, derivedTypeSchema_, scope)};
  dtScope.set_runtimeDerivedTypeDescription(dtObject);
  evaluate::StructureConstructorValues dtValues;
  AddValue(dtValues, derivedTypeSchema_, "name"s,
      SaveNameAsPointerTarget(scope, typeName));
  bool isPDTdefinition{
      !derivedTypeSpec && dtScope.IsParameterizedDerivedType()};
  if (!isPDTdefinition) {
    auto sizeInBytes{static_cast<common::ConstantSubscript>(dtScope.size())};
    if (auto alignment{dtScope.alignment().value_or(0)}) {
      sizeInBytes += alignment - 1;
      sizeInBytes /= alignment;
      sizeInBytes *= alignment;
    }
    AddValue(
        dtValues, derivedTypeSchema_, "sizeinbytes"s, IntToExpr(sizeInBytes));
  }
  const Symbol *parentDescObject{nullptr};
  if (const Scope * parentScope{dtScope.GetDerivedTypeParent()}) {
    parentDescObject = DescribeType(*const_cast<Scope *>(parentScope));
  }
  if (parentDescObject) {
    AddValue(dtValues, derivedTypeSchema_, "parent"s,
        evaluate::AsGenericExpr(evaluate::Expr<evaluate::SomeDerived>{
            evaluate::Designator<evaluate::SomeDerived>{*parentDescObject}}));
  } else {
    AddValue(dtValues, derivedTypeSchema_, "parent"s,
        SomeExpr{evaluate::NullPointer{}});
  }
  bool isPDTinstantiation{derivedTypeSpec && &dtScope != dtSymbol->scope()};
  if (isPDTinstantiation) {
    // is PDT instantiation
    const Symbol *uninstDescObject{
        DescribeType(DEREF(const_cast<Scope *>(dtSymbol->scope())))};
    AddValue(dtValues, derivedTypeSchema_, "uninstantiated"s,
        evaluate::AsGenericExpr(evaluate::Expr<evaluate::SomeDerived>{
            evaluate::Designator<evaluate::SomeDerived>{
                DEREF(uninstDescObject)}}));
  } else {
    AddValue(dtValues, derivedTypeSchema_, "uninstantiated"s,
        SomeExpr{evaluate::NullPointer{}});
  }

  // TODO: compute typeHash

  using Int8 = evaluate::Type<TypeCategory::Integer, 8>;
  using Int1 = evaluate::Type<TypeCategory::Integer, 1>;
  std::vector<Int8::Scalar> kinds;
  std::vector<Int1::Scalar> lenKinds;
  const SymbolVector *parameters{GetTypeParameters(*dtSymbol)};
  if (parameters) {
    // Package the derived type's parameters in declaration order for
    // each category of parameter.  KIND= type parameters are described
    // by their instantiated (or default) values, while LEN= type
    // parameters are described by their INTEGER kinds.
    for (SymbolRef ref : *parameters) {
      const auto &tpd{ref->get<TypeParamDetails>()};
      if (tpd.attr() == common::TypeParamAttr::Kind) {
        auto value{evaluate::ToInt64(tpd.init()).value_or(0)};
        if (derivedTypeSpec) {
          if (const auto *pv{derivedTypeSpec->FindParameter(ref->name())}) {
            if (pv->GetExplicit()) {
              if (auto instantiatedValue{
                      evaluate::ToInt64(*pv->GetExplicit())}) {
                value = *instantiatedValue;
              }
            }
          }
        }
        kinds.emplace_back(value);
      } else { // LEN= parameter
        lenKinds.emplace_back(GetIntegerKind(*ref));
      }
    }
  }
  AddValue(dtValues, derivedTypeSchema_, "kindparameter"s,
      SaveNumericPointerTarget<Int8>(
          scope, SaveObjectName(".kp."s + distinctName), std::move(kinds)));
  AddValue(dtValues, derivedTypeSchema_, "lenparameterkind"s,
      SaveNumericPointerTarget<Int1>(
          scope, SaveObjectName(".lpk."s + distinctName), std::move(lenKinds)));
  // Traverse the components of the derived type
  if (!isPDTdefinition) {
    std::vector<evaluate::StructureConstructor> dataComponents;
    std::vector<evaluate::StructureConstructor> procPtrComponents;
    std::vector<evaluate::StructureConstructor> specials;
    for (const auto &pair : dtScope) {
      const Symbol &symbol{*pair.second};
      auto locationRestorer{common::ScopedSet(location_, symbol.name())};
      std::visit(
          common::visitors{
              [&](const TypeParamDetails &) {
                // already handled above in declaration order
              },
              [&](const ObjectEntityDetails &object) {
                dataComponents.emplace_back(DescribeComponent(
                    symbol, object, dtScope, distinctName, parameters));
              },
              [&](const ProcEntityDetails &proc) {
                if (IsProcedurePointer(symbol)) {
                  procPtrComponents.emplace_back(
                      DescribeComponent(symbol, proc, dtScope));
                }
              },
              [&](const ProcBindingDetails &) { // handled in a later pass
              },
              [&](const GenericDetails &generic) {
                DescribeGeneric(generic, specials);
              },
              [&](const auto &) {
                common::die(
                    "unexpected details on symbol '%s' in derived type scope",
                    symbol.name().ToString().c_str());
              },
          },
          symbol.details());
    }
    AddValue(dtValues, derivedTypeSchema_, "component"s,
        SaveDerivedPointerTarget(scope, SaveObjectName(".c."s + distinctName),
            std::move(dataComponents),
            evaluate::ConstantSubscripts{
                static_cast<evaluate::ConstantSubscript>(
                    dataComponents.size())}));
    AddValue(dtValues, derivedTypeSchema_, "procptr"s,
        SaveDerivedPointerTarget(scope, SaveObjectName(".p."s + distinctName),
            std::move(procPtrComponents),
            evaluate::ConstantSubscripts{
                static_cast<evaluate::ConstantSubscript>(
                    procPtrComponents.size())}));
    // Compile the "vtable" of type-bound procedure bindings
    std::vector<evaluate::StructureConstructor> bindings{
        DescribeBindings(dtScope, scope)};
    AddValue(dtValues, derivedTypeSchema_, "binding"s,
        SaveDerivedPointerTarget(scope, SaveObjectName(".v."s + distinctName),
            std::move(bindings),
            evaluate::ConstantSubscripts{
                static_cast<evaluate::ConstantSubscript>(bindings.size())}));
    // Describe "special" bindings to defined assignments, FINAL subroutines,
    // and user-defined derived type I/O subroutines.
    if (dtScope.symbol()) {
      for (const auto &pair :
          dtScope.symbol()->get<DerivedTypeDetails>().finals()) {
        DescribeSpecialProc(specials, *pair.second, false /*!isAssignment*/,
            true, std::nullopt);
      }
    }
    IncorporateDefinedIoGenericInterfaces(specials,
        SourceName{"read(formatted)", 15},
        GenericKind::DefinedIo::ReadFormatted, &scope);
    IncorporateDefinedIoGenericInterfaces(specials,
        SourceName{"read(unformatted)", 17},
        GenericKind::DefinedIo::ReadUnformatted, &scope);
    IncorporateDefinedIoGenericInterfaces(specials,
        SourceName{"write(formatted)", 16},
        GenericKind::DefinedIo::WriteFormatted, &scope);
    IncorporateDefinedIoGenericInterfaces(specials,
        SourceName{"write(unformatted)", 18},
        GenericKind::DefinedIo::WriteUnformatted, &scope);
    AddValue(dtValues, derivedTypeSchema_, "special"s,
        SaveDerivedPointerTarget(scope, SaveObjectName(".s."s + distinctName),
            std::move(specials),
            evaluate::ConstantSubscripts{
                static_cast<evaluate::ConstantSubscript>(specials.size())}));
  }
  dtObject.get<ObjectEntityDetails>().set_init(MaybeExpr{
      StructureExpr(Structure(derivedTypeSchema_, std::move(dtValues)))});
  return &dtObject;
}

static const Symbol &GetSymbol(const Scope &schemata, SourceName name) {
  auto iter{schemata.find(name)};
  CHECK(iter != schemata.end());
  const Symbol &symbol{*iter->second};
  return symbol;
}

const Symbol &RuntimeTableBuilder::GetSchemaSymbol(const char *name) const {
  return GetSymbol(
      DEREF(tables_.schemata), SourceName{name, std::strlen(name)});
}

const DeclTypeSpec &RuntimeTableBuilder::GetSchema(
    const char *schemaName) const {
  Scope &schemata{DEREF(tables_.schemata)};
  SourceName name{schemaName, std::strlen(schemaName)};
  const Symbol &symbol{GetSymbol(schemata, name)};
  CHECK(symbol.has<DerivedTypeDetails>());
  CHECK(symbol.scope());
  CHECK(symbol.scope()->IsDerivedType());
  const DeclTypeSpec *spec{nullptr};
  if (symbol.scope()->derivedTypeSpec()) {
    DeclTypeSpec typeSpec{
        DeclTypeSpec::TypeDerived, *symbol.scope()->derivedTypeSpec()};
    spec = schemata.FindType(typeSpec);
  }
  if (!spec) {
    DeclTypeSpec typeSpec{
        DeclTypeSpec::TypeDerived, DerivedTypeSpec{name, symbol}};
    spec = schemata.FindType(typeSpec);
  }
  if (!spec) {
    spec = &schemata.MakeDerivedType(
        DeclTypeSpec::TypeDerived, DerivedTypeSpec{name, symbol});
  }
  CHECK(spec->AsDerived());
  return *spec;
}

template <int KIND> static SomeExpr IntExpr(std::int64_t n) {
  return evaluate::AsGenericExpr(
      evaluate::Constant<evaluate::Type<TypeCategory::Integer, KIND>>{n});
}

SomeExpr RuntimeTableBuilder::GetEnumValue(const char *name) const {
  const Symbol &symbol{GetSchemaSymbol(name)};
  auto value{evaluate::ToInt64(symbol.get<ObjectEntityDetails>().init())};
  CHECK(value.has_value());
  return IntExpr<1>(*value);
}

Symbol &RuntimeTableBuilder::CreateObject(
    const std::string &name, const DeclTypeSpec &type, Scope &scope) {
  ObjectEntityDetails object;
  object.set_type(type);
  auto pair{scope.try_emplace(SaveObjectName(name),
      Attrs{Attr::TARGET, Attr::SAVE}, std::move(object))};
  CHECK(pair.second);
  Symbol &result{*pair.first->second};
  return result;
}

SourceName RuntimeTableBuilder::SaveObjectName(const std::string &name) {
  return *tables_.names.insert(name).first;
}

SomeExpr RuntimeTableBuilder::SaveNameAsPointerTarget(
    Scope &scope, const std::string &name) {
  CHECK(!name.empty());
  CHECK(name.front() != '.');
  ObjectEntityDetails object;
  auto len{static_cast<common::ConstantSubscript>(name.size())};
  if (const auto *spec{scope.FindType(DeclTypeSpec{CharacterTypeSpec{
          ParamValue{len, common::TypeParamAttr::Len}, KindExpr{1}}})}) {
    object.set_type(*spec);
  } else {
    object.set_type(scope.MakeCharacterType(
        ParamValue{len, common::TypeParamAttr::Len}, KindExpr{1}));
  }
  using Ascii = evaluate::Type<TypeCategory::Character, 1>;
  using AsciiExpr = evaluate::Expr<Ascii>;
  object.set_init(evaluate::AsGenericExpr(AsciiExpr{name}));
  const Symbol &symbol{
      *scope
           .try_emplace(SaveObjectName(".n."s + name),
               Attrs{Attr::TARGET, Attr::SAVE}, std::move(object))
           .first->second};
  return evaluate::AsGenericExpr(
      AsciiExpr{evaluate::Designator<Ascii>{symbol}});
}

evaluate::StructureConstructor RuntimeTableBuilder::DescribeComponent(
    const Symbol &symbol, const ObjectEntityDetails &object, Scope &scope,
    const std::string &distinctName, const SymbolVector *parameters) {
  evaluate::StructureConstructorValues values;
  auto &foldingContext{context_.foldingContext()};
  auto typeAndShape{evaluate::characteristics::TypeAndShape::Characterize(
      symbol, foldingContext)};
  CHECK(typeAndShape.has_value());
  auto dyType{typeAndShape->type()};
  const auto &shape{typeAndShape->shape()};
  AddValue(values, componentSchema_, "name"s,
      SaveNameAsPointerTarget(scope, symbol.name().ToString()));
  AddValue(values, componentSchema_, "category"s,
      IntExpr<1>(static_cast<int>(dyType.category())));
  if (dyType.IsUnlimitedPolymorphic() ||
      dyType.category() == TypeCategory::Derived) {
    AddValue(values, componentSchema_, "kind"s, IntExpr<1>(0));
  } else {
    AddValue(values, componentSchema_, "kind"s, IntExpr<1>(dyType.kind()));
  }
  AddValue(values, componentSchema_, "offset"s, IntExpr<8>(symbol.offset()));
  // CHARACTER length
  auto len{typeAndShape->LEN()};
  if (const semantics::DerivedTypeSpec * pdtInstance{scope.derivedTypeSpec()}) {
    auto restorer{foldingContext.WithPDTInstance(*pdtInstance)};
    len = Fold(foldingContext, std::move(len));
  }
  if (dyType.category() == TypeCategory::Character && len) {
    AddValue(values, componentSchema_, "characterlen"s,
        evaluate::AsGenericExpr(GetValue(len, parameters)));
  } else {
    AddValue(values, componentSchema_, "characterlen"s,
        PackageIntValueExpr(deferredEnum_));
  }
  // Describe component's derived type
  std::vector<evaluate::StructureConstructor> lenParams;
  if (dyType.category() == TypeCategory::Derived &&
      !dyType.IsUnlimitedPolymorphic()) {
    const DerivedTypeSpec &spec{dyType.GetDerivedTypeSpec()};
    Scope *derivedScope{const_cast<Scope *>(
        spec.scope() ? spec.scope() : spec.typeSymbol().scope())};
    const Symbol *derivedDescription{DescribeType(DEREF(derivedScope))};
    AddValue(values, componentSchema_, "derived"s,
        evaluate::AsGenericExpr(evaluate::Expr<evaluate::SomeDerived>{
            evaluate::Designator<evaluate::SomeDerived>{
                DEREF(derivedDescription)}}));
    // Package values of LEN parameters, if any
    if (const SymbolVector * specParams{GetTypeParameters(spec.typeSymbol())}) {
      for (SymbolRef ref : *specParams) {
        const auto &tpd{ref->get<TypeParamDetails>()};
        if (tpd.attr() == common::TypeParamAttr::Len) {
          if (const ParamValue * paramValue{spec.FindParameter(ref->name())}) {
            lenParams.emplace_back(GetValue(*paramValue, parameters));
          } else {
            lenParams.emplace_back(GetValue(tpd.init(), parameters));
          }
        }
      }
    }
  } else {
    // Subtle: a category of Derived with a null derived type pointer
    // signifies CLASS(*)
    AddValue(values, componentSchema_, "derived"s,
        SomeExpr{evaluate::NullPointer{}});
  }
  // LEN type parameter values for the component's type
  if (!lenParams.empty()) {
    AddValue(values, componentSchema_, "lenvalue"s,
        SaveDerivedPointerTarget(scope,
            SaveObjectName(
                ".lv."s + distinctName + "."s + symbol.name().ToString()),
            std::move(lenParams),
            evaluate::ConstantSubscripts{
                static_cast<evaluate::ConstantSubscript>(lenParams.size())}));
  } else {
    AddValue(values, componentSchema_, "lenvalue"s,
        SomeExpr{evaluate::NullPointer{}});
  }
  // Shape information
  int rank{evaluate::GetRank(shape)};
  AddValue(values, componentSchema_, "rank"s, IntExpr<1>(rank));
  if (rank > 0 && !IsAllocatable(symbol) && !IsPointer(symbol)) {
    std::vector<evaluate::StructureConstructor> bounds;
    evaluate::NamedEntity entity{symbol};
    for (int j{0}; j < rank; ++j) {
      bounds.emplace_back(GetValue(std::make_optional(evaluate::GetLowerBound(
                                       foldingContext, entity, j)),
          parameters));
      bounds.emplace_back(GetValue(
          evaluate::GetUpperBound(foldingContext, entity, j), parameters));
    }
    AddValue(values, componentSchema_, "bounds"s,
        SaveDerivedPointerTarget(scope,
            SaveObjectName(
                ".b."s + distinctName + "."s + symbol.name().ToString()),
            std::move(bounds), evaluate::ConstantSubscripts{2, rank}));
  } else {
    AddValue(
        values, componentSchema_, "bounds"s, SomeExpr{evaluate::NullPointer{}});
  }
  // Default component initialization
  bool hasDataInit{false};
  if (IsAllocatable(symbol)) {
    AddValue(values, componentSchema_, "genre"s, GetEnumValue("allocatable"));
  } else if (IsPointer(symbol)) {
    AddValue(values, componentSchema_, "genre"s, GetEnumValue("pointer"));
    hasDataInit = object.init().has_value();
    if (hasDataInit) {
      AddValue(values, componentSchema_, "initialization"s,
          SomeExpr{*object.init()});
    }
  } else if (IsAutomaticObject(symbol)) {
    AddValue(values, componentSchema_, "genre"s, GetEnumValue("automatic"));
  } else {
    AddValue(values, componentSchema_, "genre"s, GetEnumValue("data"));
    hasDataInit = object.init().has_value();
    if (hasDataInit) {
      AddValue(values, componentSchema_, "initialization"s,
          SaveObjectInit(scope,
              SaveObjectName(
                  ".di."s + distinctName + "."s + symbol.name().ToString()),
              object));
    }
  }
  if (!hasDataInit) {
    AddValue(values, componentSchema_, "initialization"s,
        SomeExpr{evaluate::NullPointer{}});
  }
  return {DEREF(componentSchema_.AsDerived()), std::move(values)};
}

evaluate::StructureConstructor RuntimeTableBuilder::DescribeComponent(
    const Symbol &symbol, const ProcEntityDetails &proc, Scope &scope) {
  evaluate::StructureConstructorValues values;
  AddValue(values, procPtrSchema_, "name"s,
      SaveNameAsPointerTarget(scope, symbol.name().ToString()));
  AddValue(values, procPtrSchema_, "offset"s, IntExpr<8>(symbol.offset()));
  if (auto init{proc.init()}; init && *init) {
    AddValue(values, procPtrSchema_, "initialization"s,
        SomeExpr{evaluate::ProcedureDesignator{**init}});
  } else {
    AddValue(values, procPtrSchema_, "initialization"s,
        SomeExpr{evaluate::NullPointer{}});
  }
  return {DEREF(procPtrSchema_.AsDerived()), std::move(values)};
}

evaluate::StructureConstructor RuntimeTableBuilder::PackageIntValue(
    const SomeExpr &genre, std::int64_t n) const {
  evaluate::StructureConstructorValues xs;
  AddValue(xs, valueSchema_, "genre"s, genre);
  AddValue(xs, valueSchema_, "value"s, IntToExpr(n));
  return Structure(valueSchema_, std::move(xs));
}

SomeExpr RuntimeTableBuilder::PackageIntValueExpr(
    const SomeExpr &genre, std::int64_t n) const {
  return StructureExpr(PackageIntValue(genre, n));
}

std::vector<const Symbol *> RuntimeTableBuilder::CollectBindings(
    const Scope &dtScope) const {
  std::vector<const Symbol *> result;
  std::map<SourceName, const Symbol *> localBindings;
  // Collect local bindings
  for (auto pair : dtScope) {
    const Symbol &symbol{*pair.second};
    if (symbol.has<ProcBindingDetails>()) {
      localBindings.emplace(symbol.name(), &symbol);
    }
  }
  if (const Scope * parentScope{dtScope.GetDerivedTypeParent()}) {
    result = CollectBindings(*parentScope);
    // Apply overrides from the local bindings of the extended type
    for (auto iter{result.begin()}; iter != result.end(); ++iter) {
      const Symbol &symbol{**iter};
      auto overridden{localBindings.find(symbol.name())};
      if (overridden != localBindings.end()) {
        *iter = overridden->second;
        localBindings.erase(overridden);
      }
    }
  }
  // Add remaining (non-overriding) local bindings in name order to the result
  for (auto pair : localBindings) {
    result.push_back(pair.second);
  }
  return result;
}

std::vector<evaluate::StructureConstructor>
RuntimeTableBuilder::DescribeBindings(const Scope &dtScope, Scope &scope) {
  std::vector<evaluate::StructureConstructor> result;
  for (const Symbol *symbol : CollectBindings(dtScope)) {
    evaluate::StructureConstructorValues values;
    AddValue(values, bindingSchema_, "proc"s,
        SomeExpr{evaluate::ProcedureDesignator{
            symbol->get<ProcBindingDetails>().symbol()}});
    AddValue(values, bindingSchema_, "name"s,
        SaveNameAsPointerTarget(scope, symbol->name().ToString()));
    result.emplace_back(DEREF(bindingSchema_.AsDerived()), std::move(values));
  }
  return result;
}

void RuntimeTableBuilder::DescribeGeneric(const GenericDetails &generic,
    std::vector<evaluate::StructureConstructor> &specials) {
  std::visit(common::visitors{
                 [&](const GenericKind::OtherKind &k) {
                   if (k == GenericKind::OtherKind::Assignment) {
                     for (auto ref : generic.specificProcs()) {
                       DescribeSpecialProc(specials, *ref, true,
                           false /*!final*/, std::nullopt);
                     }
                   }
                 },
                 [&](const GenericKind::DefinedIo &io) {
                   switch (io) {
                   case GenericKind::DefinedIo::ReadFormatted:
                   case GenericKind::DefinedIo::ReadUnformatted:
                   case GenericKind::DefinedIo::WriteFormatted:
                   case GenericKind::DefinedIo::WriteUnformatted:
                     for (auto ref : generic.specificProcs()) {
                       DescribeSpecialProc(
                           specials, *ref, false, false /*!final*/, io);
                     }
                     break;
                   }
                 },
                 [](const auto &) {},
             },
      generic.kind().u);
}

void RuntimeTableBuilder::DescribeSpecialProc(
    std::vector<evaluate::StructureConstructor> &specials,
    const Symbol &specificOrBinding, bool isAssignment, bool isFinal,
    std::optional<GenericKind::DefinedIo> io) {
  const auto *binding{specificOrBinding.detailsIf<ProcBindingDetails>()};
  const Symbol &specific{*(binding ? &binding->symbol() : &specificOrBinding)};
  if (auto proc{evaluate::characteristics::Procedure::Characterize(
          specific, context_.foldingContext())}) {
    std::uint8_t rank{0};
    std::uint8_t isArgDescriptorSet{0};
    int argThatMightBeDescriptor{0};
    MaybeExpr which;
    if (isAssignment) { // only type-bound asst's are germane to runtime
      CHECK(binding != nullptr);
      CHECK(proc->dummyArguments.size() == 2);
      which = proc->IsElemental() ? elementalAssignmentEnum_ : assignmentEnum_;
      if (binding && binding->passName() &&
          *binding->passName() == proc->dummyArguments[1].name) {
        argThatMightBeDescriptor = 1;
        isArgDescriptorSet |= 2;
      } else {
        argThatMightBeDescriptor = 2; // the non-passed-object argument
        isArgDescriptorSet |= 1;
      }
    } else if (isFinal) {
      CHECK(binding == nullptr); // FINALs are not bindings
      CHECK(proc->dummyArguments.size() == 1);
      if (proc->IsElemental()) {
        which = elementalFinalEnum_;
      } else {
        const auto &typeAndShape{
            std::get<evaluate::characteristics::DummyDataObject>(
                proc->dummyArguments.at(0).u)
                .type};
        if (typeAndShape.attrs().test(
                evaluate::characteristics::TypeAndShape::Attr::AssumedRank)) {
          which = assumedRankFinalEnum_;
          isArgDescriptorSet |= 1;
        } else {
          which = finalEnum_;
          rank = evaluate::GetRank(typeAndShape.shape());
          if (rank > 0) {
            argThatMightBeDescriptor = 1;
          }
        }
      }
    } else { // user defined derived type I/O
      CHECK(proc->dummyArguments.size() >= 4);
      if (binding) {
        isArgDescriptorSet |= 1;
      }
      switch (io.value()) {
      case GenericKind::DefinedIo::ReadFormatted:
        which = readFormattedEnum_;
        break;
      case GenericKind::DefinedIo::ReadUnformatted:
        which = readUnformattedEnum_;
        break;
      case GenericKind::DefinedIo::WriteFormatted:
        which = writeFormattedEnum_;
        break;
      case GenericKind::DefinedIo::WriteUnformatted:
        which = writeUnformattedEnum_;
        break;
      }
    }
    if (argThatMightBeDescriptor != 0 &&
        !proc->dummyArguments.at(argThatMightBeDescriptor - 1)
             .CanBePassedViaImplicitInterface()) {
      isArgDescriptorSet |= 1 << (argThatMightBeDescriptor - 1);
    }
    evaluate::StructureConstructorValues values;
    AddValue(
        values, specialSchema_, "which"s, SomeExpr{std::move(which.value())});
    AddValue(values, specialSchema_, "rank"s, IntExpr<1>(rank));
    AddValue(values, specialSchema_, "isargdescriptorset"s,
        IntExpr<1>(isArgDescriptorSet));
    AddValue(values, specialSchema_, "proc"s,
        SomeExpr{evaluate::ProcedureDesignator{specific}});
    specials.emplace_back(DEREF(specialSchema_.AsDerived()), std::move(values));
  }
}

void RuntimeTableBuilder::IncorporateDefinedIoGenericInterfaces(
    std::vector<evaluate::StructureConstructor> &specials, SourceName name,
    GenericKind::DefinedIo definedIo, const Scope *scope) {
  for (; !scope->IsGlobal(); scope = &scope->parent()) {
    if (auto asst{scope->find(name)}; asst != scope->end()) {
      const Symbol &generic{*asst->second};
      const auto &genericDetails{generic.get<GenericDetails>()};
      CHECK(std::holds_alternative<GenericKind::DefinedIo>(
          genericDetails.kind().u));
      CHECK(std::get<GenericKind::DefinedIo>(genericDetails.kind().u) ==
          definedIo);
      for (auto ref : genericDetails.specificProcs()) {
        DescribeSpecialProc(specials, *ref, false, false, definedIo);
      }
    }
  }
}

RuntimeDerivedTypeTables BuildRuntimeDerivedTypeTables(
    SemanticsContext &context) {
  ModFileReader reader{context};
  RuntimeDerivedTypeTables result;
  static const char schemataName[]{"__fortran_type_info"};
  SourceName schemataModule{schemataName, std::strlen(schemataName)};
  result.schemata = reader.Read(schemataModule);
  if (result.schemata) {
    RuntimeTableBuilder builder{context, result};
    builder.DescribeTypes(context.globalScope());
  }
  return result;
}
} // namespace Fortran::semantics
