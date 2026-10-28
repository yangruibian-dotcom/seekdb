/*
 * Copyright (c) 2025 OceanBase.
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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/basic/file_sql_native.h"
#include <charconv>
#include <fast_float/fast_float.h>


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_, file_column_ids_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (MY_SPEC.value_expr_->type_ == T_VARCHAR) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file_sql;
    ret = open_file_sql();
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (file_state_ != nullptr) {
      try {
        std::string root;
        OZ (file_sql_access(ctx_.get_my_session(), root));
        if (OB_SUCC(ret)) {
          if (file_state_->scan) {
            file_sql::Input access(file_state_->binding.path, root, file_state_->binding.options);
            file_state_->scan->rescan();
          }
          else file_state_->files = file_sql::discover(file_state_->binding.path, root);
          file_state_->index = 0;
        }
      } catch (const std::exception &error) { ret = file_sql_error(error); }
    }
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  if (file_state_) {
    file_state_->~FileSqlState();
    ctx_.get_allocator().free(file_state_);
    file_state_ = nullptr;
  }
  return ret;
}

void ObFunctionTableOp::destroy()
{
  if (file_state_) {
    file_state_->~FileSqlState();
    ctx_.get_allocator().free(file_state_);
    file_state_ = nullptr;
  }
  ObOperator::destroy();
}

int ObFunctionTableOp::open_file_sql()
{
  int ret = OB_SUCCESS;
  std::string root;
  ObDatum *value = nullptr;
  if (OB_FAIL(file_sql_access(ctx_.get_my_session(), root))) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
  } else {
    try {
      void *memory = ctx_.get_allocator().alloc(sizeof(FileSqlState));
      if (!memory) return OB_ALLOCATE_MEMORY_FAILED;
      file_state_ = new(memory) FileSqlState();
      auto bytes = value->get_string();
      file_state_->binding = file_sql::Binding::decode(std::string(bytes.ptr(), bytes.length()));
      if (file_state_->binding.options.format == "glob") {
        file_state_->files = file_sql::discover(file_state_->binding.path, root);
      } else {
        file_state_->scan = std::make_unique<file_sql::Scan>(file_state_->binding, root);
        std::vector<size_t> projected;
        for (auto index : MY_SPEC.file_column_ids_) projected.push_back(index);
        file_state_->scan->project(projected);
      }
    } catch (const std::bad_alloc &) { ret = OB_ALLOCATE_MEMORY_FAILED; }
      catch (const std::exception &error) { ret = file_sql_error(error); }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_file_sql()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(ctx_.check_status())) return ret;
  try {
    bool found = false;
    if (file_state_->scan) found = file_state_->scan->next(file_state_->row);
    else if (file_state_->index < file_state_->files.size()) {
      file_state_->row = {{file_state_->files[file_state_->index++], file_sql::Type::Varchar}};
      found = true;
    }
    if (!found) return OB_ITER_END;
    for (int64_t i = 0; i < MY_SPEC.column_exprs_.count(); ++i) {
      size_t column = MY_SPEC.file_column_ids_.at(i);
      auto &cell = file_state_->row.at(column);
      auto type = file_state_->binding.columns.at(column).type;
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
      if (cell.type == file_sql::Type::Null) datum.set_null();
      else if (type == file_sql::Type::Varchar) datum.set_string(cell.text.data(), cell.text.size());
      else if (type == file_sql::Type::Bigint) {
        int64_t value = 0;
        auto r = std::from_chars(cell.text.data() + (cell.text[0] == '+'), cell.text.data() + cell.text.size(), value);
        if (r.ec != std::errc()) throw file_sql::Error("file SQL: BIGINT conversion failed");
        datum.set_int(value);
      } else if (type == file_sql::Type::Double) {
        double value = 0;
        auto r = fast_float::from_chars(cell.text.data() + (cell.text[0] == '+'), cell.text.data() + cell.text.size(), value);
        if (r.ec != std::errc()) throw file_sql::Error("file SQL: DOUBLE conversion failed");
        datum.set_double(value);
      } else if (type == file_sql::Type::Boolean) datum.set_bool(cell.text[0] == 't' || cell.text[0] == 'T');
      else if (type == file_sql::Type::Date) datum.set_date(file_sql::temporal_value(cell.text, type));
      else if (type == file_sql::Type::Datetime) datum.set_datetime(file_sql::temporal_value(cell.text, type));
      else datum.set_null();
      expr->set_evaluated_projected(eval_ctx_);
    }
  } catch (const std::bad_alloc &) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    catch (const std::exception &error) { ret = file_sql_error(error); }
  return ret;
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
