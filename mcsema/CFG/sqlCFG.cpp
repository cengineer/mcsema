/*
 * Copyright (c) 2019 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <memory>
#include <utility>
#include <type_traits>

#include <mcsema/CFG/Types.h>
#include <mcsema/CFG/Util.h>

#include <mcsema/CFG/sqlCFG.h>
#include <mcsema/CFG/Schema.h>
#include <mcsema/CFG/Context.h>

namespace mcsema {
namespace cfg {

using Database = decltype(Context::db);

using CtxPtr = std::shared_ptr<Context>;
using CtxR = Context *;

template<typename Ctx>
struct with_context {

  with_context(CtxPtr &shared_ctx) : _ctx(shared_ctx.get()) {}

  CtxR _ctx;
};

using has_context = with_context< Context >;

template<typename Concrete = SymtabEntry>
struct SymtabEntry_ : has_context,
                      id_based_ops_<SymtabEntry_<Concrete>>
{
  using has_context::has_context;
  using concrete_t = Concrete;


  static constexpr Query table_name = R"(symtabs)";

  constexpr static Query q_insert =
    R"(insert into symtabs(name, module_rowid, type_rowid) values (?1, ?2, ?3))";

  constexpr static Query q_get =
    R"(select name, type_rowid from symtabs where rowid = ?1)";

  constexpr static Query s_insert_module_rowid =
    R"(insert into symtabs(name, module_rowid, type_rowid) values (?1, #1, ?2))";

};

template<typename Concrete = MemoryRange>
struct MemoryRange_ : has_context,
                      id_based_ops_<MemoryRange_<Concrete>>,
                      has_ea<MemoryRange_<Concrete>> {

  using has_context::has_context;
  static constexpr Query table_name = R"(memory_ranges)";

  constexpr static Query q_insert =
      R"(insert into memory_ranges(module_rowid, ea, size, bytes)
      values (?1, ?2, ?3, ?4))";

  constexpr static Query q_data =
      R"(select bytes from memory_ranges where rowid = ?1)";

  constexpr static Query q_get =
      R"(SELECT ea, size
         FROM memory_ranges
         WHERE rowid = ?1)";
};

template< typename Self >
struct module_ops_mixin : id_based_ops_< Self > {};

template< typename Concrete = Module >
struct Module_ : has_context,
                 module_ops_mixin< Module_< Concrete > > {

  using has_context::has_context;
  static constexpr Query table_name = R"(modules)";

  constexpr static Query q_insert =
    R"(insert into modules(name) values (?1))";

  auto all_functions(int64_t id) {
    constexpr static Query q_data =
      R"(select ea, is_entrypoint from functions where module_rowid = ?1)";
    return _ctx->db.template query<q_data>(id);
  }

  auto all_functions_r(int64_t id) {
    constexpr static Query q_obj =
      R"(SELECT rowid
         FROM functions
         WHERE module_rowid = ?1)";
      return _ctx->db.template query<q_obj>(id);
  }

  auto all_symbols(int64_t id) {
    constexpr static Query q_data =
      R"(select name, type_rowid from symtabs where module_rowid = ?1)";
    return _ctx->db.template query<q_data>(id);
  }
};

template< typename Self >
struct func_ops_mixin :
  func_ops_< Self >,
  id_based_ops_< Self >,
  has_symtab_name< Self >
{};


template<typename Concrete = Function>
struct Function_ : has_context,
                   func_ops_mixin<Function_<Concrete>>,
                   has_ea<Function_<Concrete>>
{
  using has_context::has_context;
  static constexpr Query table_name = R"(functions)";
  static constexpr Query q_insert =
      R"(insert into functions(module_rowid, ea, is_entrypoint) values (?1, ?2, ?3))";

  static constexpr Query q_get =
    R"(select ea, is_entrypoint from functions)";
};


template<typename Self>
struct bb_mixin : id_based_ops_<Self>,
                  has_ea<Self>{};

template< typename Concrete = BasicBlock >
struct BasicBlock_: has_context,
                    bb_mixin< BasicBlock_< Concrete > >
{
  using has_context::has_context;
  constexpr static Query table_name = R"(blocks)";

  constexpr static Query q_insert =
    R"(insert into blocks(module_rowid, ea, size, memory_rowid)
        values (?1, ?2, ?3, ?4))";

  constexpr static Query q_get =
    R"(SELECT ea, size
       FROM blocks
       WHERE rowid = ?1)";

  constexpr static Query q_orphans =
    R"(SELECT bb.rowid
       FROM blocks AS bb
       WHERE bb.rowid NOT IN (SELECT bb_rowid
                              FROM function_to_block))";


  auto orphaned() {
    return _ctx->db.template query<q_orphans>();
  }

  std::string data(int64_t id) {
    // SUBSTR index starts from 0, therefore we need + 1
    constexpr static Query q_data =
      R"(SELECT SUBSTR(mr.bytes, bb.ea - mr.ea + 1) FROM
          blocks as bb JOIN
          memory_ranges as mr ON
          mr.rowid = bb.memory_rowid and bb.rowid = ?1)";
    sqlite::blob data_view;
    _ctx->db.template query<q_data>(id)(data_view);
    return std::move(data_view);
  }
};


template<typename Concrete = Segment>
struct Segment_ : has_context,
                  id_based_ops_<Segment_<Concrete>>,
                  has_ea<Segment_<Concrete>> {
  using has_context::has_context;

  constexpr static Query table_name = R"(segments)";

  Segment_() = default;

  constexpr static Query q_insert =
    R"(insert into segments(
        ea, size,
        read_only, is_external, is_exported, is_thread_local,
        variable_name, memory_rowid) values (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8))";

  constexpr static Query q_get =
    R"(SELECT ea, size read_only, is_external, is_exported
       FROM segments,
       WHERE rowid = ?1)";

  auto _insert(int64_t ea,
               int64_t size,
               const Segment::Flags &flags,
               const std::string &name,
               int64_t memory_rowid) {

    return this->insert(ea, size,
                        flags.read_only, flags.is_external, flags.is_exported,
                        flags.is_thread_local,
                        name, memory_rowid);
  }


  std::string_view data(int64_t id) {
    constexpr static Query q_data =
      R"(SELECT mr.rowid, s.ea - mr.ea, s.size FROM
          segments as s JOIN
          memory_ranges as mr ON
          mr.rowid = s.memory_rowid and s.rowid = ?1)";
    int64_t mr_rowid,
            offset,
            size;
    std::tie(mr_rowid, offset, size) =
      *this->_ctx->db.template query<q_data>(id)
                 .template Get<int64_t, int64_t, int64_t>();
    auto c_data = this->_ctx->cache
                  .template Find<Segment, MemoryRange_<MemoryRange>::q_data>(mr_rowid);
    return c_data.substr(offset, size);
  }

  void SetFlags(int64_t id, const Segment::Flags &flags) {
    constexpr static Query q_set_flags =
      R"(UPDATE segments SET
        (read_only, is_external, is_exported, is_thread_local) =
        (?2, ?3, ?4, ?5) WHERE rowid = ?1)";
    this->_ctx->db.template query<q_set_flags>(id,
                                    flags.read_only, flags.is_external,
                                    flags.is_exported, flags.is_thread_local);
  }
};


template<typename Concrete = CodeXref >
struct CodeXref_ : has_context,
                   has_symtab_name<CodeXref_<Concrete>>,
                   has_ea<CodeXref_<Concrete>>,
                   id_based_ops_<CodeXref_<Concrete>> {
  using has_context::has_context;
  constexpr static Query table_name = R"(code_references)";

  constexpr static Query q_insert =
    R"(insert into code_references(
         ea, target_ea, bb_rowid, operand_type_rowid, mask, symtab_rowid)
       values(?1, ?2, ?3, ?4, ?5, ?6))";

  constexpr static Query q_get_module_rowid =
    R"(SELECT mr.module_rowid FROM data_references as dr
                              JOIN segments as seg
                              JOIN memory_ranges as mr
                              ON dr.segment_rowid = seg.rowid
                                 and seg.memory_rowid = mr.rowid)";

  constexpr static Query q_get =
    R"(SELECT ea, target_ea, operand_type_rowid, mask
       FROM code_references
       WHERE rowid = ?1)";

};

template<typename Concrete = DataXref>
struct DataXref_ : has_context,
                   has_symtab_name<DataXref_<Concrete>>,
                   has_ea<DataXref_<Concrete>>,
                   id_based_ops_<DataXref_<Concrete>> {

  using has_context::has_context;
  constexpr static Query table_name = R"(data_references)";

  constexpr static Query q_insert =
    R"(insert into data_references(
          ea, width, target_ea, segment_rowid, fixup_kind_rowid, symtab_rowid)
       values(?1, ?2, ?3, ?4, ?5, ?6))";

  constexpr static Query q_get =
    R"(SELECT ea, width, target_ea, fixup_kind_rowid
       FROM data_references
       WHERE rowid = ?1)";

};

template<typename Concrete = ExternalFunction>
struct ExternalFunction_ : has_context,
                           has_symtab_name<ExternalFunction_<Concrete>>,
                           has_ea<ExternalFunction_<Concrete>>,
                           id_based_ops_<ExternalFunction_<Concrete>> {
  using has_context::has_context;
  constexpr static Query table_name = R"(external_functions)";

  constexpr static Query q_insert =
    R"(insert into external_functions(
        ea, calling_convention_rowid, symtab_rowid, module_rowid, has_return, is_weak)
        values (?1, ?2, ?3, ?4, ?5, ?6))";

  constexpr static Query q_get =
    R"(SELECT ea, calling_convention_rowid, has_return, is_weak
       FROM external_functions
       WHERE rowdid = ?1)";

};


/* Dispatch table used to implement generic interface traits for top level API */
template<typename T>
struct dispatch { using type = void;  };
template<>
struct dispatch<ExternalFunction> { using type = ExternalFunction_<ExternalFunction>; };
template<>
struct dispatch<CodeXref> { using type = CodeXref_<CodeXref>; };
template<>
struct dispatch<DataXref> { using type = DataXref_<DataXref>; };
template<>
struct dispatch<Function> { using type = Function_<Function>; };
template<>
struct dispatch<BasicBlock> { using type = BasicBlock_<BasicBlock>; };
template<>
struct dispatch<Segment> { using type = Segment_<Segment>; };
template<>
struct dispatch<MemoryRange> { using type = MemoryRange_<MemoryRange>; };
template<>
struct dispatch<SymtabEntry> {
  using type = SymtabEntry_<SymtabEntry>;
};

template<typename T>
using remove_cvp_t = typename std::remove_cv_t<std::remove_pointer_t<T>>;

template<typename T>
using impl_t = typename dispatch<remove_cvp_t<T>>::type;

#define ENABLE_IF(name) \
  typename std::enable_if_t< std::is_same_v< name::data_t, Data >, std::optional< Data > >


template<typename Data, typename Result>
auto Get(Result &result) -> ENABLE_IF(SymtabEntry) {
  auto out = result.template Get<std::string, SymtabEntryType>();
  return util::maybe_to_struct<Data>( std::move(out) );
}

#undef ENABLE_IF

/* Iterators */
namespace details {

struct DataIterator_impl {
  using Result_t = Context::Result_t;
  Result_t result;

  DataIterator_impl(Result_t &&r) : result(std::move(r)) {}

  template< typename Data >
  auto Fetch() {
    return Get<Data>(result) ;
  }
};


struct ObjectIterator_impl {
  using Result_t = Context::Result_t;
  Result_t result;
  CtxPtr ctx;

  ObjectIterator_impl(Result_t &&r, CtxPtr ctx_)
    : result(std::move(r)),
      ctx(std::move(ctx_))
  {}


  template<typename Object>
  auto Fetch() -> std::optional<Object> {
    return Build<Object>();
  }

  template<typename Object>
  auto Build() -> std::optional<Object> {
    if (auto id = result.template GetScalar<int64_t>()) {
      return { Object{ *id, ctx } };
    }
    return {};
  }

  #define ENABLE_IF(name) \
    typename std::enable_if_t<std::is_same_v<name, Data>, std::optional<Data>>
  #undef ENABLE_IF
};



} // namespace details

template<typename Entry>
WeakDataIterator<Entry>::WeakDataIterator(Impl_t &&init) : impl(std::move(init)) {}

template<typename Entry>
auto WeakDataIterator<Entry>::Fetch() -> maybe_data_t {
  return impl->Fetch<data_t>();
}

template<typename Entry>
WeakDataIterator<Entry>::~WeakDataIterator() {}

template<typename Entry>
WeakObjectIterator<Entry>::WeakObjectIterator(Impl_t &&init) : impl(std::move(init)) {}

template<typename Entry>
auto WeakObjectIterator<Entry>::Fetch() -> maybe_data_t {
  return impl->Fetch<data_t>();
}

template<typename Entry>
WeakObjectIterator<Entry>::~WeakObjectIterator() {}





/* Letter */

Letter::Letter(const std::string &name) : _ctx(std::make_shared<Context>(name)) {}

void Letter::CreateSchema()
{
  Schema::CreateSchema( *_ctx );
}

Module Letter::module(const std::string &name) {
  return { Module_{ _ctx }.insert(name), _ctx };
}

Function Letter::func(const Module &module, int64_t ea, bool is_entrypoint)
{
  return { Function_{ _ctx }.insert(module._id, ea, is_entrypoint), _ctx };
}

BasicBlock Letter::bb(const Module &module,
                      int64_t ea,
                      int64_t size,
                      const MemoryRange &range)
{
  return { BasicBlock_{ _ctx }.insert(module._id, ea, size, range._id), _ctx };
}

MemoryRange Letter::AddMemoryRange(const Module &module,
                                   int64_t ea,
                                   int64_t size,
                                   std::string_view data) {
  // TODO: Check if this copy to sqlite::blob is required
  return { MemoryRange_{ _ctx }.insert(module._id, ea, size,
                                 sqlite::blob( data.begin(), data.end() ) ),
            _ctx };
}

MemoryRange Letter::AddMemoryRange(const Module &module,
                                   int64_t ea,
                                   std::string_view data) {
  return AddMemoryRange(module, ea, data.size(), data);
}

/* Module */

Function Module::AddFunction(int64_t ea, bool is_entrypoint ) {
  return { Function_{ _ctx }.insert( _id, ea, is_entrypoint ), _ctx };
}

MemoryRange Module::AddMemoryRange(int64_t ea, int64_t size, std::string_view data) {
  // TODO: Check if this copy to sqlite::blob is required
  return { MemoryRange_{ _ctx }.insert(_id, ea, size,
                                 sqlite::blob( data.begin(), data.end() ) ),
           _ctx };
}

MemoryRange Module::AddMemoryRange(int64_t ea, std::string_view data) {
  return AddMemoryRange(ea, data.size(), data);
}

BasicBlock Module::AddBasicBlock(int64_t ea, int64_t size, const MemoryRange &mem) {
  return { BasicBlock_{ _ctx }.insert(_id, ea, size, mem._id), _ctx };
}

SymtabEntry Module::AddSymtabEntry(const std::string &name, SymtabEntryType type) {
  return { SymtabEntry_{ _ctx }.insert(name, _id, static_cast<unsigned char>(type)),
           _ctx };
}

ExternalFunction Module::AddExternalFunction(int64_t ea,
                                             const SymtabEntry &name,
                                             CC cc,
                                             bool has_return, bool is_weak) {
  return { ExternalFunction_{ _ctx }.insert(ea,
                                            static_cast<unsigned char>(cc),
                                            name._id,
                                            _id,
                                            has_return,
                                            is_weak),
           _ctx };
}

WeakObjectIterator<BasicBlock> Module::OrphanedBasicBlocks() {
  auto result = BasicBlock_{ _ctx }.orphaned();
  return { std::make_unique<details::ObjectIterator_impl>(std::move(result), _ctx) };
}


WeakDataIterator<SymtabEntry> Module::Symbols_d() {
  auto result = Module_{_ctx }.all_symbols(_id);
  return { std::make_unique<details::DataIterator_impl>(std::move(result)) };
}

WeakObjectIterator<Function> Module::Functions() {
  auto result = Module_{ _ctx }.all_functions_r(_id);
  return { std::make_unique<details::ObjectIterator_impl>(std::move(result), _ctx ) };
}

/* Function */

void Function::AttachBlock(const BasicBlock &bb) {
  Function_<Function>{ _ctx }.bind_bb(_id, bb._id);
}


/* BasicBlock */
std::string BasicBlock::Data() {
    return BasicBlock_{ _ctx }.data(_id);
}

CodeXref BasicBlock::AddXref(int64_t ea, int64_t target_ea, OperandType op_type) {
  return { CodeXref_{ _ctx }.insert(ea,
                                    target_ea,
                                    _id,
                                    static_cast<unsigned char>(op_type),
                                    NULL,
                                    NULL),
          _ctx };
}


CodeXref BasicBlock::AddXref(int64_t ea,
                             int64_t target_ea,
                             OperandType op_type,
                             const SymtabEntry &name,
                             std::optional<int64_t> mask) {
  return { CodeXref_{ _ctx }.insert(ea,
                                    target_ea,
                                    _id,
                                    static_cast<unsigned char>(op_type),
                                    (mask) ? *mask : NULL,
                                    name._id),
          _ctx };
}

/* Segment */

std::string_view Segment::Data() {
  return Segment_( _ctx ).data(_id);
}

void Segment::SetFlags(const Flags &flags) {
  return Segment_{ _ctx }.SetFlags(_id, flags);
}

DataXref Segment::AddXref(int64_t ea, int64_t target_ea, int64_t width, FixupKind fixup) {
  return { DataXref_{ _ctx }.insert(ea, width, target_ea, _id,
                                    static_cast<unsigned char>(fixup),
                                    NULL),
          _ctx };
}

DataXref Segment::AddXref(int64_t ea, int64_t target_ea,
                          int64_t width, FixupKind fixup, const SymtabEntry &name) {

  return { DataXref_{ _ctx }.insert(ea, width, target_ea, _id,
                                    static_cast<unsigned char>(fixup),
                                    name._id),
         _ctx };
}



/* MemoryRange */

Segment MemoryRange::AddSegment(int64_t ea,
                                 int64_t size,
                                 const Segment::Flags &flags,
                                 const std::string &name) {
  return { Segment_{ _ctx }._insert( ea, size, flags, name, _id ), _ctx };
}

/* CodeXref */

/* ExternalFunction */
std::string ExternalFunction::Name() const {
  return *impl_t<decltype(this)>{ _ctx }.GetName(_id);
}

/* operator*() */
SymtabEntry::data_t SymtabEntry::operator*() const {
  using self_t = remove_cvp_t<decltype(this)>;
  return impl_t<self_t>{_ctx}.c_get<typename self_t::data_t,
                                    std::string, SymtabEntryType>(_id);
}

ExternalFunction::data_t ExternalFunction::operator*() const {
  using self_t = remove_cvp_t<decltype(this)>;
  return impl_t<self_t>{_ctx}.c_get<typename self_t::data_t,
                                    int64_t, CC, bool, bool>(_id);
}

BasicBlock::data_t BasicBlock::operator*() const {
  using self_t = remove_cvp_t<decltype(this)>;
  return impl_t<self_t>{ _ctx }.c_get<typename self_t::data_t,
                                      int64_t, int64_t>(_id);
}

Function::data_t Function::operator*() const {
  using self_t = remove_cvp_t<decltype(this)>;
  return impl_t<self_t>{ _ctx }.c_get<typename self_t::data_t,
                                       int64_t, bool>(_id);
}

Segment::data_t Segment::operator*() const {
  using self_t = remove_cvp_t<decltype(this)>;
  return impl_t<self_t>{ _ctx }.c_get<typename self_t::data_t,
                                      int, int, bool, bool, bool, bool>(_id);
}

MemoryRange::data_t MemoryRange::operator*() const {
  using self_t = remove_cvp_t<decltype(this)>;
  return impl_t<self_t>{ _ctx }.c_get<typename self_t::data_t,
                                      int64_t, int64_t>(_id);

}

CodeXref::data_t CodeXref::operator*() const {
  using self_t = remove_cvp_t<decltype(this)>;
  return impl_t<self_t>{ _ctx }.c_get<typename self_t::data_t,
                                      int64_t, int64_t, OperandType,
                                      std::optional<int64_t>>(_id);

}


DataXref::data_t DataXref::operator*() const {
  using self_t = remove_cvp_t<decltype(this)>;
  return impl_t<self_t>{ _ctx }.c_get<typename self_t::data_t,
                                      int64_t, int64_t, int64_t, FixupKind>(_id);

}


/* Erasable */

#define DEF_ERASE(self) \
  void self::Erase() { \
    impl_t<decltype(this)>{_ctx}.erase(_id); \
  }

DEF_ERASE(SymtabEntry)
DEF_ERASE(ExternalFunction)
DEF_ERASE(Function)
DEF_ERASE(Segment)
DEF_ERASE(MemoryRange)
DEF_ERASE(CodeXref)
DEF_ERASE(DataXref)

#undef DEF_ERASE

/* Traits */

template<typename Self>
int64_t interface::HasEa<Self>::ea() {
  auto self = static_cast<Self *>(this);
  return impl_t<decltype(self)>{ self->_ctx }.get_ea( self->_id );
}

template<typename Self>
std::optional<std::string> interface::HasSymtabEntry<Self>::Name() {
  auto self = static_cast<Self *>(this);
  return impl_t<decltype(self)>{ self->_ctx }.GetName( self->_id );
}

template<typename Self>
void interface::HasSymtabEntry<Self>::Name(
    const SymtabEntry &name) {

  auto self = static_cast<Self *>(this);
  return impl_t<decltype(self)>{ self->_ctx }.Name( self->_id, name._id );
}

template<typename Self>
std::optional<SymtabEntry::data_t> interface::HasSymtabEntry<Self>::Symbol() {
  auto self = static_cast<Self *>(this);

  auto res = impl_t<decltype(self)>{self->_ctx}.Symbol(self->_id);
  return util::maybe_to_struct<SymtabEntry::data_t>(
      res.template Get<std::string, SymtabEntryType>());
}

namespace interface {

/* We must explicitly instantiate all templates */

template struct HasEa<DataXref>;
template struct HasSymtabEntry<DataXref>;

template struct HasEa<MemoryRange>;

template struct HasEa<Segment>;
// TODO: Implement
//template struct HasSymtabEntry<Segment>;

template struct HasEa<Function>;
template struct HasSymtabEntry<Function>;

template struct HasEa<BasicBlock>;

template struct HasEa<ExternalFunction>;
template struct HasSymtabEntry<ExternalFunction>;

template struct HasEa<CodeXref>;
template struct HasSymtabEntry<CodeXref>;

} // namespace interface

template struct WeakDataIterator<SymtabEntry>;
template struct WeakObjectIterator<Function>;
template struct WeakObjectIterator<BasicBlock>;
} // namespace cfg
} // namespace mcsema
