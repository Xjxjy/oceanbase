/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef _OB_TRANSFORM_MIX_MAX_H
#define _OB_TRANSFORM_MIX_MAX_H

#include "sql/rewrite/ob_transform_rule.h"
#include "objit/common/ob_item_type.h"
namespace oceanbase
{
namespace common
{
class ObIAllocator;
template <typename T>
class ObIArray;
}//common
namespace share
{
namespace schema
{
class ObTableSchema;
}
}
}//oceanbase

namespace oceanbase
{
namespace sql
{
class ObSelectStmt;
class ObDMLStmt;
class ObStmt;
class ObRawExpr;
class ObOpRawExpr;
class ObColumnRefRawExpr;
class ObAggFunRawExpr;

/* rewrite min or max aggr on index as a subquery which can table scan just one line.
 * eg:
 * select min(pk) from t1
 * -->
 * select min(v.c1) from (select pk from t1 where pk is not null order by pk limit 1)
 *
 * rewrite requests:
 * 1. max/min aggragate on a column of table, and this column is a index or the first nonconst column of index.
 * 2. select stmt is scalar group by and hasn't limit.
 * 3. just deal single table yet.
 */
class ObTransformMinMax : public ObTransformRule
{
public:
  explicit ObTransformMinMax(ObTransformerCtx *ctx);
  virtual ~ObTransformMinMax();
  virtual int transform_one_stmt(common::ObIArray<ObParentDMLStmt> &parent_stmts,
                                 ObDMLStmt *&stmt,
                                 bool &trans_happened) override;
private:

  int check_transform_validity(ObSelectStmt *stmt,
                               ObAggFunRawExpr *&aggr_expr,
                               bool &is_valid);

  int do_transform(ObSelectStmt *select_stmt, ObAggFunRawExpr *aggr_expr);

  int is_valid_index_column(const ObSelectStmt *stmt,
                            const ObRawExpr *expr,
                            bool &is_expected_index);

  int is_valid_having(const ObSelectStmt *stmt,
                      const ObAggFunRawExpr *column_aggr_expr,
                      bool &is_expected);

  int is_valid_aggr_expr(const ObSelectStmt &stmt,
                         const ObRawExpr *expr,
                         const ObAggFunRawExpr *aggr_expr,
                         bool &is_valid);

  int find_unexpected_having_expr(const ObAggFunRawExpr *aggr_expr,
                                  const ObRawExpr *cur_expr,
                                  bool &is_unexpected);

  int set_child_condition(ObSelectStmt *stmt, ObRawExpr *aggr_expr);

  int set_child_order_item(ObSelectStmt *stmt, ObRawExpr *aggr_expr);

  /**
   * @brief: check whether there is any valid select_item
   * request stmt has only one valid aggr expr, and select_items are exprs combainded const expr or that aggr_expr
   */
  int is_valid_select_list(const ObSelectStmt &stmt, const ObAggFunRawExpr *aggr_expr, bool &is_valid);
  DISALLOW_COPY_AND_ASSIGN(ObTransformMinMax);
};

} //namespace sql
} //namespace oceanbase
#endif
