// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/basic/file_sql_native.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/ob_sql_context.h"
#include "sql/engine/ob_exec_context.h"
#include <set>

namespace oceanbase { namespace sql {
using namespace common;
namespace {
std::string node_string(const ParseNode *node)
{
  if (node && node->str_len_ == 0) return {};
  if (!node || !node->str_value_) throw file_sql::Error("file SQL: expected a string literal");
  return std::string(node->str_value_, node->str_len_);
}
std::string lower(std::string value)
{ for (char &c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return value; }
std::string string_option(const ParseNode *n)
{
  if (!n || (n->type_ != T_VARCHAR && n->type_ != T_CHAR)) throw file_sql::Error("file SQL: expected a string option");
  return node_string(n);
}
}
int ObDMLResolver::resolve_file_table_item(const ParseNode &node, TableItem *&table)
{
  int ret = OB_SUCCESS;
  std::string root;
  if (OB_FAIL(file_sql_access(session_info_, root))) return ret;
  try {
    if (node.num_child_ != 3 || !node.children_[0] || !node.children_[1])
      throw file_sql::Error("file SQL: malformed table function");
    std::string function = lower(node_string(node.children_[0]));
    file_sql::Options options;
    options.format = function == "glob" ? "glob" : function.substr(5);
    const ParseNode *args = node.children_[1];
    if (!args->num_child_) throw file_sql::Error("file SQL: missing path");
    std::string path = string_option(args->children_[0]);
    std::set<std::string> seen;
    for (int i = 1; i < args->num_child_; ++i) {
      auto *arg = args->children_[i];
      if (!arg || arg->type_ != T_FILE_SQL_OPTION || arg->num_child_ != 2)
        throw file_sql::Error("file SQL: options must be named");
      std::string name = lower(node_string(arg->children_[0]));
      if (name == "delim" || name == "sep") name = "delimiter";
      if (name == "null" || name == "null_string") name = "nullstr";
      if (!seen.insert(name).second) throw file_sql::Error("file SQL: duplicate option " + name);
      auto *v = arg->children_[1];
      if (name == "sample_size") {
        if (!v || v->type_ != T_INT || v->value_ != -1) throw file_sql::Error("file SQL: sample_size must be -1");
      } else if (name == "format" && (options.format == "json" || options.format == "ndjson")) {
        if (lower(string_option(v)) != "newline_delimited") throw file_sql::Error("file SQL: JSON format must be newline_delimited");
      } else if (name == "header") {
        if (!v || (v->type_ != T_BOOL && v->type_ != T_INT) || (v->value_ != 0 && v->value_ != 1))
          throw file_sql::Error("file SQL: header must be boolean");
        options.header = v->value_;
      } else if (name == "nullstr") options.nullstr = string_option(v);
      else if (name == "delimiter" || name == "quote" || name == "escape") {
        auto s = string_option(v);
        if (s.size() != 1 || uint8_t(s[0]) >= 128) throw file_sql::Error("file SQL: CSV characters must be one ASCII byte");
        if (name == "delimiter") options.delimiter = s[0];
        else if (name == "quote") options.quote = s[0];
        else options.escape = s[0];
      } else throw file_sql::Error("file SQL: unknown option " + name);
      if (options.format != "csv" && name != "sample_size" && name != "format") throw file_sql::Error("file SQL: CSV option on another format");
    }
    if (seen.count("quote") && !seen.count("escape")) options.escape = options.quote;
    options.validate();
    file_sql::Binding binding;
    if (options.format == "glob") {
      file_sql::discover(path, root);
      binding.path = path; binding.options = options;
      binding.columns.push_back({"file", file_sql::Type::Varchar, 4096});
    } else binding = file_sql::bind(path, root, options, [&]() {
      if (session_info_->get_cur_exec_ctx() && session_info_->get_cur_exec_ctx()->check_status() != OB_SUCCESS)
        throw file_sql::Error("file SQL: schema inference cancelled");
    });
    auto encoded = binding.encode();
    ObString descriptor;
    ObConstRawExpr *value = nullptr;
    TableItem *item = nullptr;
    // The descriptor is a binary constant owned by the statement allocator. Only
    // the dedicated parser node can create a constant FUNCTION TABLE expression.
    OZ (ob_write_string(*allocator_, ObString(encoded.size(), encoded.data()), descriptor));
    OZ (ObRawExprUtils::build_const_string_expr(*params_.expr_factory_, ObVarcharType, descriptor, CS_TYPE_BINARY, value));
    OZ (value->formalize(session_info_));
    if (OB_SUCC(ret)) {
      item = get_stmt()->create_table_item(*allocator_);
      if (!item) ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    if (OB_SUCC(ret)) {
      ObString name;
      auto *alias = node.children_[2];
      if (alias) name.assign_ptr(alias->str_value_, alias->str_len_);
      else name.assign_ptr(node.children_[0]->str_value_, node.children_[0]->str_len_);
      item->table_id_ = generate_table_id(); item->type_ = TableItem::FUNCTION_TABLE;
      item->table_name_ = name; item->alias_name_ = name; item->function_table_expr_ = value;
      OZ (get_stmt()->add_table_item(session_info_, item));
      ObSEArray<ColumnItem, 16> columns;
      OZ (resolve_file_table_columns(*item, columns));
      // Files have no schema-version dependency in the catalog. Rebind on each
      // execution, including prepared statements, instead of retaining stale metadata.
      get_stmt()->get_query_ctx()->query_hint_.get_global_hint().plan_cache_policy_ = OB_USE_PLAN_CACHE_NONE;
      get_stmt()->get_query_ctx()->query_hint_.get_global_hint().parallel_ = 1;
      table = item;
    }
  } catch (const std::bad_alloc &) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    catch (const std::exception &error) { ret = file_sql_error(error); }
  return ret;
}
int ObDMLResolver::resolve_file_table_columns(const TableItem &table, ObIArray<ColumnItem> &out)
{
  int ret = OB_SUCCESS;
  try {
    const auto &obj = static_cast<ObConstRawExpr *>(table.function_table_expr_)->get_value();
    auto bytes = obj.get_string();
    auto binding = file_sql::Binding::decode(std::string(bytes.ptr(), bytes.length()));
    for (size_t i = 0; OB_SUCC(ret) && i < binding.columns.size(); ++i) {
      uint64_t id = OB_APP_MIN_COLUMN_ID + i;
      ColumnItem *column = get_stmt()->get_column_item_by_id(table.table_id_, id);
      if (!column) {
        auto &c = binding.columns[i]; ObObjMeta meta; ObAccuracy accuracy;
        static const ObObjType types[] = {ObNullType, ObIntType, ObDoubleType, ObTinyIntType, ObVarcharType, ObDateType, ObDateTimeType};
        meta.set_type(types[int(c.type)]);
        if (c.type == file_sql::Type::Varchar) { meta.set_collation_type(CS_TYPE_UTF8MB4_BIN); meta.set_collation_level(CS_LEVEL_IMPLICIT); accuracy.set_length(std::max<uint32_t>(1,c.width)); }
        if (c.type == file_sql::Type::Datetime) accuracy.set_scale(6);
        ObString name;
        OZ (ob_write_string(*allocator_, ObString(c.name.size(), c.name.data()), name));
        OZ (resolve_function_table_column_item(table, meta, accuracy, name, id, column));
      }
      if (OB_SUCC(ret)) OZ (out.push_back(*column));
    }
  } catch (const std::bad_alloc &) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    catch (const std::exception &error) { ret = file_sql_error(error); }
  return ret;
}

int ObSelectResolver::resolve_file_copy(const ParseNode &node, ObSelectIntoItem &item)
{
  int ret = OB_SUCCESS;
  std::string root;
  if (OB_FAIL(file_sql_access(session_info_, root))) return ret;
  try {
    if (node.num_child_ != 2) throw file_sql::Error("file SQL: invalid COPY");
    file_sql::Binding binding;
    binding.path = string_option(node.children_[0]);
    std::set<std::string> seen;
    auto *args = node.children_[1];
    for (int i = 0; args && i < args->num_child_; ++i) {
      auto *a = args->children_[i];
      if (!a || a->num_child_ != 2) throw file_sql::Error("file SQL: invalid COPY option");
      auto key = lower(node_string(a->children_[0])); auto *v = a->children_[1];
      if (key == "null" || key == "null_string") key = "nullstr";
      if (key == "delim" || key == "sep") key = "delimiter";
      if (!seen.insert(key).second) throw file_sql::Error("file SQL: duplicate COPY option " + key);
      if (key == "header") {
        if (!v || (v->type_ != T_BOOL && v->type_ != T_INT) || v->value_ < 0 || v->value_ > 1)
          throw file_sql::Error("file SQL: HEADER requires boolean");
        binding.options.header = v->value_;
      } else if (key == "format") {
        if (lower(string_option(v)) != "csv") throw file_sql::Error("file SQL: COPY supports FORMAT CSV");
      } else if (key == "nullstr") binding.options.nullstr = string_option(v);
      else if (key == "delimiter" || key == "delim" || key == "quote" || key == "escape") {
        auto s = string_option(v);
        if (s.size() != 1 || uint8_t(s[0]) >= 128) throw file_sql::Error("file SQL: COPY character option must be one ASCII byte");
        if (key == "quote") binding.options.quote = s[0];
        else if (key == "escape") binding.options.escape = s[0];
        else binding.options.delimiter = s[0];
      } else throw file_sql::Error("file SQL: unknown COPY option " + key);
    }
    if (seen.count("quote") && !seen.count("escape")) binding.options.escape = binding.options.quote;
    binding.options.validate();
    for (int64_t i = 0; i < get_select_stmt()->get_select_item_size(); ++i) {
      const auto &selected = get_select_stmt()->get_select_item(i);
      auto name = selected.alias_name_.empty() ? selected.expr_name_ : selected.alias_name_;
      binding.columns.push_back({std::string(name.ptr(), name.length()),file_sql::Type::Varchar,0});
    }
    auto encoded = binding.encode(); ObString descriptor;
    OZ (ob_write_string(*allocator_, ObString(encoded.size(),encoded.data()),descriptor));
    item.external_properties_ = descriptor;
    item.is_single_ = true;
    get_stmt()->get_query_ctx()->query_hint_.get_global_hint().plan_cache_policy_ = OB_USE_PLAN_CACHE_NONE;
    get_stmt()->get_query_ctx()->query_hint_.get_global_hint().parallel_ = 1;
  } catch (const std::bad_alloc &) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    catch (const std::exception &error) { ret = file_sql_error(error); }
  return ret;
}

int ObSelectResolver::resolve_file_describe(const ParseNode &node)
{
  int ret = OB_SUCCESS;
  std::string root;
  if (OB_FAIL(file_sql_access(session_info_, root))) return ret;
  try {
    if (node.num_child_ != 1 || !node.children_[0]) return OB_INVALID_ARGUMENT;
    ObSelectResolver source(params_);
    OZ (source.resolve(*node.children_[0]));
    if (OB_FAIL(ret)) return ret;
    // Only metadata is turned into constant rows. The source query is resolved,
    // never executed; file values never leave the native query execution path.
    auto literal = [](const std::string &s) {
      static const char hex[] = "0123456789abcdef";
      std::string sql = "CONVERT(X'";
      for (uint8_t c : s) { sql += hex[c >> 4]; sql += hex[c & 15]; }
      return sql + "' USING utf8mb4)";
    };
    std::string sql;
    auto *stmt = source.get_select_stmt();
    for (int64_t i = 0; i < stmt->get_select_item_size(); ++i) {
      auto &selected = stmt->get_select_item(i);
      auto name = selected.alias_name_.empty() ? selected.expr_name_ : selected.alias_name_;
      auto &type = selected.expr_->get_result_type();
      std::string typename_ = ob_sql_type_str(type.get_type());
      bool nullable = !type.has_result_flag(NOT_NULL_FLAG);
      if (selected.expr_->is_column_ref_expr()) {
        auto *ref = static_cast<ObColumnRefRawExpr *>(selected.expr_);
        auto *table = stmt->get_table_item_by_id(ref->get_table_id());
        if (table && table->is_function_table() && table->function_table_expr_->is_const_expr()) {
          auto bytes = static_cast<ObConstRawExpr *>(table->function_table_expr_)->get_value().get_string();
          auto binding = file_sql::Binding::decode(std::string(bytes.ptr(),bytes.length()));
          auto index = ref->get_column_id() - OB_APP_MIN_COLUMN_ID;
          if (index < binding.columns.size()) {
            typename_ = file_sql::type_name(binding.columns[index].type);
            nullable = binding.columns[index].nullable;
          }
        }
      }
      for (char &c : typename_) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
      if (i) sql += " UNION ALL ";
      sql += "SELECT " + literal(std::string(name.ptr(),name.length())) + " AS `column_name`, "
          + literal(typename_) + " AS `column_type`, '" + (nullable ? "YES" : "NO")
          + "' AS `null`, NULL AS `key`, NULL AS `default`, NULL AS `extra`";
    }
    ParseResult parsed;
    ObParser parser(*allocator_, session_info_->get_sql_mode());
    OZ (parser.parse(ObString(sql.size(),sql.data()), parsed));
    if (OB_SUCC(ret)) {
      if (!parsed.result_tree_ || !parsed.result_tree_->children_[0]) ret = OB_ERR_UNEXPECTED;
      else ret = resolve(*parsed.result_tree_->children_[0]);
    }
    if (OB_SUCC(ret) && get_stmt())
      get_stmt()->get_query_ctx()->query_hint_.get_global_hint().plan_cache_policy_ = OB_USE_PLAN_CACHE_NONE;
  } catch (const std::bad_alloc &) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    catch (const std::exception &error) { ret = file_sql_error(error); }
  return ret;
}
}}
