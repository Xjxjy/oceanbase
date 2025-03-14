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

#include "storage/multi_data_source/ob_mds_table_merge_task.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/ls/ob_ls_get_mod.h"
#include "storage/tablet/ob_tablet_common.h"
#include "storage/tablet/ob_tablet.h"
#include "storage/compaction/ob_tablet_merge_ctx.h"
#include "storage/compaction/ob_tablet_merge_task.h"
#include "storage/multi_data_source/ob_mds_table_merge_dag.h"

#define USING_LOG_PREFIX MDS

using namespace oceanbase::common;
using namespace oceanbase::compaction;

namespace oceanbase
{
namespace storage
{
namespace mds
{
ObMdsTableMergeTask::ObMdsTableMergeTask()
  : ObITask(ObITaskType::TASK_TYPE_MDS_TABLE_MERGE),
    is_inited_(false),
    mds_merge_dag_(nullptr)
{
}

int ObMdsTableMergeTask::init()
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(dag_)) {
    ret = OB_ERR_SYS;
    LOG_WARN("dag must not be null", K(ret));
  } else if (OB_UNLIKELY(ObDagType::ObDagTypeEnum::DAG_TYPE_MDS_TABLE_MERGE != dag_->get_type())) {
    ret = OB_ERR_SYS;
    LOG_ERROR("dag type not match", K(ret), KPC_(dag));
  } else {
    ObMdsTableMergeDag *mds_merge_dag = static_cast<ObMdsTableMergeDag *>(dag_);
    const ObTabletMergeDagParam &merge_dag_param = mds_merge_dag->get_param();
    if (OB_UNLIKELY(!merge_dag_param.is_valid())) {
      ret = OB_ERR_SYS;
      LOG_WARN("param is not valid", K(ret), "param", mds_merge_dag->get_param());
    } else {
      mds_merge_dag_ = mds_merge_dag;
      is_inited_ = true;
    }
  }

  return ret;
}

int ObMdsTableMergeTask::process()
{
  TIMEGUARD_INIT(STORAGE, 10_ms, 5_s);
  int ret = OB_SUCCESS;

  DEBUG_SYNC(AFTER_EMPTY_SHELL_TABLET_CREATE);

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret), K_(is_inited));
  } else {
    int tmp_ret = OB_SUCCESS;
    ObLS *ls = nullptr;
    ObTablet *tablet = nullptr;
    ObTabletMergeCtx &ctx = mds_merge_dag_->get_ctx();
    const share::ObLSID &ls_id = ctx.param_.ls_id_;
    const common::ObTabletID &tablet_id = ctx.param_.tablet_id_;
    const share::SCN &flush_scn = mds_merge_dag_->get_flush_scn();
    int64_t ls_rebuild_seq = -1;

    if (OB_ISNULL(ls = ctx.ls_handle_.get_ls())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ls is null", K(ret), K(ls_id), "ls_handle", ctx.ls_handle_);
    } else if (ls->is_offline()) {
      ret = OB_CANCELED;
      LOG_INFO("ls offline, skip merge", K(ret), K(ctx));
    } else if (FALSE_IT(ctx.time_guard_.click(ObCompactionTimeGuard::DAG_WAIT_TO_SCHEDULE))) {
    } else if (MDS_FAIL(ls->get_tablet(tablet_id, ctx.tablet_handle_, 0, ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
      LOG_WARN("failed to get tablet", K(ret), K(ls_id), K(tablet_id));
    } else if (OB_ISNULL(tablet = ctx.tablet_handle_.get_obj())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("tablet is null", K(ret), K(ls_id), K(tablet_id), "tablet_handle", ctx.tablet_handle_);
    } else if (FALSE_IT(ctx.time_guard_.click(ObCompactionTimeGuard::GET_TABLET))) {
    } else if (FALSE_IT(ls_rebuild_seq = ls->get_rebuild_seq())) {
    } else if (MDS_FAIL(ls->build_new_tablet_from_mds_table(ls_rebuild_seq, tablet_id, flush_scn))) {
      LOG_WARN("failed to build new tablet from mds table", K(ret), K(ls_id), K(tablet_id), K(ls_rebuild_seq), K(flush_scn));
    } else {
      ctx.time_guard_.click(ObCompactionTimeGuard::EXECUTE);
      share::dag_yield();
    }

    // always notify flush ret
    if (OB_NOT_NULL(tablet) && MDS_TMP_FAIL(tablet->notify_mds_table_flush_ret(flush_scn, ret))) {
      LOG_WARN("failed to notify mds table flush ret", K(tmp_ret), K(ls_id), K(tablet_id), K(flush_scn), "flush_ret", ret);
      if (OB_SUCC(ret)) {
        // ret equals to OB_SUCCESS, use tmp_ret as return value
        ret = tmp_ret;
      }
    }
    ctx.time_guard_.click(ObCompactionTimeGuard::DAG_FINISH);
    (void)ctx.collect_running_info();
  }

  return ret;
}
} // namespace mds
} // namespace storage
} // namespace oceanbase
