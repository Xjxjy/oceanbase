// Copyright (c) 2021 OceanBase
// OceanBase is licensed under Mulan PubL v2.
// You can use this software according to the terms and conditions of the Mulan PubL v2.
// You may obtain a copy of Mulan PubL v2 at:
//          http://license.coscl.org.cn/MulanPubL-2.0
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PubL v2 for more details.

#include "lib/container/ob_bit_set.h"
#include "ob_dup_table_lease.h"
#include "ob_dup_table_tablets.h"
#include "ob_dup_table_ts_sync.h"
#include "ob_dup_table_util.h"
#include "storage/tablet/ob_tablet_iterator.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/tx/ob_tx_log_adapter.h"
#include "storage/tx_storage/ob_ls_service.h"

namespace oceanbase
{

using namespace storage;
using namespace common;
using namespace share;

namespace transaction
{

typedef ObSEArray<common::ObTabletID, 100> TabletIDArray;

//*************************************************************************************************************
//**** ObDupTabletScanTask
//*************************************************************************************************************
void ObDupTabletScanTask::reset()
{
  tenant_id_ = 0;
  dup_table_scan_timer_ = nullptr;
  dup_loop_worker_ = nullptr;
  last_execute_time_ = 0;
  max_execute_interval_ = 0;
}

int ObDupTabletScanTask::make(const int64_t tenant_id,
                              ObDupTableLeaseTimer *scan_timer,
                              ObDupTableLoopWorker *loop_worker)
{
  int ret = OB_SUCCESS;
  if (tenant_id <= 0 || OB_ISNULL(loop_worker) || OB_ISNULL(scan_timer)) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid arguments", K(ret), K(tenant_id), KP(loop_worker), KP(scan_timer));
  } else {
    tenant_id_ = tenant_id;
    dup_table_scan_timer_ = scan_timer;
    dup_loop_worker_ = loop_worker;
    // ObTransTask::make(ObTransRetryTaskType::DUP_TABLET_SCAN_TASK);
    // set_retry_interval_us(DUP_TABLET_SCAN_INTERVAL, DUP_TABLET_SCAN_INTERVAL);
  }
  return ret;
}

void ObDupTabletScanTask::runTimerTask()
{
  int tmp_ret = OB_SUCCESS;

  if (tenant_id_ <= 0 || OB_ISNULL(dup_loop_worker_) || OB_ISNULL(dup_table_scan_timer_)) {
    tmp_ret = OB_NOT_INIT;
    DUP_TABLE_LOG_RET(WARN, tmp_ret, "invalid arguments", K(tmp_ret), K(tenant_id_),
                      KP(dup_loop_worker_), KP(dup_table_scan_timer_));
  } else {
    if (OB_TMP_FAIL(execute_for_dup_ls_())) {
      DUP_TABLE_LOG_RET(WARN, tmp_ret, "execute dup ls scan failed", K(tmp_ret));
    }

    dup_table_scan_timer_->unregister_timeout_task(*this);
    dup_table_scan_timer_->register_timeout_task(*this, DUP_TABLET_SCAN_INTERVAL);
  }
}

int ObDupTabletScanTask::refresh_dup_tablet_schema_(
    bool need_refresh,
    ObTenantDupTabletSchemaHelper::TabletIDSet &tenant_dup_tablet_set,
    share::ObLSStatusInfo &dup_ls_status_info)
{
  int ret = OB_SUCCESS;
  bool has_dup_ls = false;
  if (need_refresh) {

    share::ObLSStatusOperator ls_status_op;
    if (OB_FAIL(ls_status_op.get_duplicate_ls_status_info(MTL_ID(), *GCTX.sql_proxy_,
                                                          dup_ls_status_info))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        DUP_TABLE_LOG(DEBUG, "no duplicate ls", K(dup_ls_status_info));
        ret = OB_SUCCESS;
      } else {
        DUP_TABLE_LOG(WARN, "get duplicate ls status info failed", K(ret), K(dup_ls_status_info));
      }
    } else {
      DUP_TABLE_LOG(INFO, "find a duplicate ls", K(ret), K(dup_ls_status_info));
    }

    if (OB_SUCC(ret) && dup_ls_status_info.is_duplicate_ls()) {
      if (OB_FAIL(ret)) {
        // do nothing
      } else if (!tenant_dup_tablet_set.created()) {
        if (OB_FAIL(tenant_dup_tablet_set.create(512))) {
          DUP_TABLE_LOG(WARN, "init dup tablet cache failed", K(ret));
        }
      }

      if (OB_FAIL(ret)) {
        // do nothing
      } else if (OB_FAIL(dup_schema_helper_.refresh_and_get_tablet_set(tenant_dup_tablet_set))) {
        DUP_TABLE_LOG(WARN, "refresh dup tablet set failed", K(ret));
      }
    }
  }
  return ret;
}

int ObDupTabletScanTask::execute_()
{
  int ret = OB_SUCCESS;
  int iter_ret = OB_SUCCESS;

  ObSharedGuard<ObLSIterator> ls_iter_guard;
  ObLSIterator *ls_iter_ptr = nullptr;
  ObLS *cur_ls_ptr = nullptr;
  TabletIDArray tablet_id_array;
  ObTenantDupTabletSchemaHelper::TabletIDSet tenant_dup_tablet_set;
  bool need_refreh_dup_schema = true;
  share::ObLSStatusInfo dup_ls_status_info;

  // compute scan task max execute interval
  const int64_t cur_time = ObTimeUtility::fast_current_time();
  if (cur_time - last_execute_time_ > 0) {
    if (0 != last_execute_time_) {
      max_execute_interval_ = max(max_execute_interval_, cur_time - last_execute_time_);
      last_execute_time_ = cur_time;
    } else {
      last_execute_time_ = ObTimeUtility::fast_current_time();
    }
  }

  if (OB_ISNULL(MTL(ObLSService *)) || OB_ISNULL(dup_loop_worker_)
      || (OB_FAIL(MTL(ObLSService *)->get_ls_iter(ls_iter_guard, ObLSGetMod::TRANS_MOD))
          || !ls_iter_guard.is_valid())) {
    if (OB_SUCC(ret)) {
      ret = OB_INVALID_ARGUMENT;
    }
    DUP_TABLE_LOG(WARN, "invalid arguments", K(ret));
  } else if (OB_ISNULL(ls_iter_ptr = ls_iter_guard.get_ptr())) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid arguments", K(ret));
  } else {
    iter_ret = OB_SUCCESS;
    cur_ls_ptr = nullptr;
    // const int64_t gc_time = ObTimeUtility::fast_current_time();
    while (OB_SUCCESS == (iter_ret = ls_iter_ptr->get_next(cur_ls_ptr))) {
      tablet_id_array.reset();

      ObRole ls_role;
      int64_t unused_proposal_id;

      if (OB_ISNULL(cur_ls_ptr)) {
        ret = OB_INVALID_ARGUMENT;
        DUP_TABLE_LOG(WARN, "invalid ls ptr", K(ret), KP(cur_ls_ptr));
      } else if (!cur_ls_ptr->get_dup_table_ls_handler()->is_master()) {
        // do nothing
        DUP_TABLE_LOG(DEBUG, "ls not leader", K(cur_ls_ptr->get_ls_id()));
      } else if (OB_FAIL(refresh_dup_tablet_schema_(need_refreh_dup_schema, tenant_dup_tablet_set, dup_ls_status_info))) {
        DUP_TABLE_LOG(INFO, "refresh dup table schema failed", K(ret));
      } else if (OB_FALSE_IT(need_refreh_dup_schema = false)) {
        // do nothing
      } else {
        // TODO
        // Only need all tablet_ids in LS.
        // No need to get tx data from tablet_meta
        storage::ObHALSTabletIDIterator ls_tablet_id_iter(cur_ls_ptr->get_ls_id(), true);
        if (OB_FAIL(cur_ls_ptr->build_tablet_iter(ls_tablet_id_iter))) {
          DUP_TABLE_LOG(WARN, "build ls tablet iter failed", K(cur_ls_ptr->get_ls_id()));
        } else if (!ls_tablet_id_iter.is_valid()) {
          DUP_TABLE_LOG(WARN, "invalid tablet id iterator", K(cur_ls_ptr->get_ls_id()));
        } else {
          ObTabletID tmp_tablet_id;
          bool is_dup_tablet = false;
          int64_t refresh_time = ObTimeUtility::fast_current_time();
          while (OB_SUCC(ls_tablet_id_iter.get_next_tablet_id(tmp_tablet_id))) {
            is_dup_tablet = false;
            ret = tenant_dup_tablet_set.exist_refactored(tmp_tablet_id);
            if (OB_HASH_EXIST == ret) {
              is_dup_tablet = true;
              ret = OB_SUCCESS;
            } else if (OB_HASH_NOT_EXIST == ret) {
              is_dup_tablet = false;
              ret = OB_SUCCESS;
            } else {
              DUP_TABLE_LOG(
                  WARN, "Failed to check whether the tablet exists in the tenant_dup_tablet_set",
                  K(ret), K(cur_ls_ptr->get_ls_id()), K(tmp_tablet_id));
            }

            if (!cur_ls_ptr->get_dup_table_ls_handler()->is_inited() && !is_dup_tablet) {
              // do nothing
            } else if (OB_FAIL(cur_ls_ptr->get_dup_table_ls_handler()->init(is_dup_tablet))
                       && OB_INIT_TWICE != ret) {
              DUP_TABLE_LOG(WARN, "init dup tablet ls handler", K(ret));
            } else if (OB_FAIL(cur_ls_ptr->get_dup_table_ls_handler()->refresh_dup_table_tablet(
                           tmp_tablet_id, is_dup_tablet, refresh_time))) {
              if (is_dup_tablet || OB_NOT_INIT != ret) {
                DUP_TABLE_LOG(WARN, "refresh ls dup table tablets failed", K(ret), K(tmp_tablet_id),
                              K(is_dup_tablet));
              } else {
                ret = OB_SUCCESS;
              }
            }
          }

          if (OB_ITER_END == ret) {
            // ret = OB_SUCCESS;
            if (OB_FAIL(cur_ls_ptr->get_dup_table_ls_handler()->gc_dup_tablets(
                    refresh_time, max_execute_interval_))) {
              DUP_TABLE_LOG(WARN, "ls gc dup_tablet failed", KR(ret), K(refresh_time),
                            K(max_execute_interval_));
            }
          }
        }
      }
      // refresh dup_table_ls on leader and follower

      if (!cur_ls_ptr->get_dup_table_ls_handler()->has_dup_tablet()) {
        // do nothing
      } else if (OB_FAIL(dup_loop_worker_->append_dup_table_ls(cur_ls_ptr->get_ls_id()))) {
        DUP_TABLE_LOG(WARN, "refresh dup_table ls failed", K(ret));
      }
    }
  }

  // DUP_TABLE_LOG(INFO, "scan all ls to find dup_tablet", KR(ret), K(tenant_dup_tablet_set.size()));
  if (tenant_dup_tablet_set.created()) {
    tenant_dup_tablet_set.destroy();
  }

  if (OB_FAIL(ret)) {
    DUP_TABLE_LOG(WARN, "scan all ls to find dup_tablet failed", KR(ret));
  }
  return ret;
}

int ObDupTabletScanTask::execute_for_dup_ls_()
{
  int ret = OB_SUCCESS;

  TabletIDArray tablet_id_array;
  ObTenantDupTabletSchemaHelper::TabletIDSet tenant_dup_tablet_set;
  bool need_refreh_dup_schema = true;
  ObLSHandle ls_handle;
  share::ObLSStatusInfo dup_ls_status_info;

  // compute scan task max execute interval
  const int64_t cur_time = ObTimeUtility::fast_current_time();
  if (cur_time - last_execute_time_ > 0) {
    if (0 != last_execute_time_) {
      max_execute_interval_ = max(max_execute_interval_, cur_time - last_execute_time_);
      last_execute_time_ = cur_time;
    } else {
      last_execute_time_ = ObTimeUtility::fast_current_time();
    }
  }

  if (OB_ISNULL(MTL(ObLSService *)) || OB_ISNULL(dup_loop_worker_)) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid arguments", K(ret));
  } else if (OB_FAIL(refresh_dup_tablet_schema_(need_refreh_dup_schema, tenant_dup_tablet_set,
                                                dup_ls_status_info))) {
    DUP_TABLE_LOG(WARN, "refresh dup table schema failed", K(ret));
  } else if (!dup_ls_status_info.is_duplicate_ls()) {
    // do nothing
  } else if (OB_FAIL(MTL(ObLSService *)
                         ->get_ls(dup_ls_status_info.ls_id_, ls_handle, ObLSGetMod::TRANS_MOD))) {
    DUP_TABLE_LOG(WARN, "get dup ls failed", K(ret), K(dup_ls_status_info));
  } else {

    ObLS *cur_ls_ptr = ls_handle.get_ls();
    if (OB_ISNULL(cur_ls_ptr)) {
      ret = OB_INVALID_ARGUMENT;
      DUP_TABLE_LOG(WARN, "invalid ls ptr", K(ret), KP(cur_ls_ptr));
    } else if (!cur_ls_ptr->get_dup_table_ls_handler()->is_master()) {
      // #ifndef NDEBUG
      DUP_TABLE_LOG(INFO,
                    "ls not leader",
                    K(cur_ls_ptr->get_ls_id()));
      // #endif
    } else if (OB_FAIL(refresh_dup_tablet_schema_(need_refreh_dup_schema, tenant_dup_tablet_set,
                                                  dup_ls_status_info))) {
      DUP_TABLE_LOG(INFO, "refresh dup table schema failed", K(ret));
    } else if (OB_FALSE_IT(need_refreh_dup_schema = false)) {
      // do nothing
    } else {
      storage::ObHALSTabletIDIterator ls_tablet_id_iter(cur_ls_ptr->get_ls_id(), true);
      if (OB_FAIL(cur_ls_ptr->build_tablet_iter(ls_tablet_id_iter))) {
        DUP_TABLE_LOG(WARN, "build ls tablet iter failed", K(cur_ls_ptr->get_ls_id()));
      } else if (!ls_tablet_id_iter.is_valid()) {
        DUP_TABLE_LOG(WARN, "invalid tablet id iterator", K(cur_ls_ptr->get_ls_id()));
      } else {
        ObTabletID tmp_tablet_id;
        bool is_dup_tablet = false;
        int64_t refresh_time = ObTimeUtility::fast_current_time();
        while (OB_SUCC(ls_tablet_id_iter.get_next_tablet_id(tmp_tablet_id))) {
          is_dup_tablet = false;
          ret = tenant_dup_tablet_set.exist_refactored(tmp_tablet_id);
          if (OB_HASH_EXIST == ret) {
            is_dup_tablet = true;
            ret = OB_SUCCESS;
          } else if (OB_HASH_NOT_EXIST == ret) {
            is_dup_tablet = false;
            ret = OB_SUCCESS;
          } else {
            DUP_TABLE_LOG(WARN,
                          "Failed to check whether the tablet exists in the tenant_dup_tablet_set",
                          K(ret), K(cur_ls_ptr->get_ls_id()), K(tmp_tablet_id));
          }

          if (!cur_ls_ptr->get_dup_table_ls_handler()->is_inited() && !is_dup_tablet) {
            // do nothing
          } else if (OB_FAIL(cur_ls_ptr->get_dup_table_ls_handler()->init(is_dup_tablet))
                     && OB_INIT_TWICE != ret) {
            DUP_TABLE_LOG(WARN, "init dup tablet ls handler", K(ret));
          } else if (OB_FAIL(cur_ls_ptr->get_dup_table_ls_handler()->refresh_dup_table_tablet(
                         tmp_tablet_id, is_dup_tablet, refresh_time))) {
            if (is_dup_tablet || OB_NOT_INIT != ret) {
              DUP_TABLE_LOG(WARN, "refresh ls dup table tablets failed", K(ret), K(tmp_tablet_id),
                            K(is_dup_tablet));
            } else {
              ret = OB_SUCCESS;
            }
          }
        }

        if (OB_ITER_END == ret) {
          // ret = OB_SUCCESS;
          if (OB_FAIL(cur_ls_ptr->get_dup_table_ls_handler()->gc_dup_tablets(
                  refresh_time, max_execute_interval_))) {
            DUP_TABLE_LOG(WARN, "ls gc dup_tablet failed", KR(ret), K(refresh_time),
                          K(max_execute_interval_));
          }
        }
      }
    }
    // refresh dup_table_ls on leader and follower

    if (!cur_ls_ptr->get_dup_table_ls_handler()->check_tablet_set_exist()) {
      // do nothing
    } else if (OB_FAIL(dup_loop_worker_->append_dup_table_ls(cur_ls_ptr->get_ls_id()))) {
      DUP_TABLE_LOG(WARN, "refresh dup_table ls failed", K(ret));
    }
  }

  if (tenant_dup_tablet_set.created()) {
    tenant_dup_tablet_set.destroy();
  }

  if (OB_FAIL(ret)) {
    DUP_TABLE_LOG(WARN, "scan dup ls to find dup_tablet failed", KR(ret));
  }
  return ret;
}

//*************************************************************************************************************
//**** ObDupTableLSHandler
//*************************************************************************************************************

int ObDupTableLSHandler::init(bool is_dup_table)
{
  int ret = OB_SUCCESS;
  if (is_dup_table) {
    if (is_inited()) {
      ret = OB_INIT_TWICE;
    } else {
      // init by dup_tablet_scan_task_.
      lease_mgr_ptr_ =
          static_cast<ObDupTableLSLeaseMgr *>(ob_malloc(sizeof(ObDupTableLSLeaseMgr), "DupTable"));
      ts_sync_mgr_ptr_ = static_cast<ObDupTableLSTsSyncMgr *>(
          ob_malloc(sizeof(ObDupTableLSTsSyncMgr), "DupTable"));
      tablets_mgr_ptr_ =
          static_cast<ObLSDupTabletsMgr *>(ob_malloc(sizeof(ObLSDupTabletsMgr), "DupTable"));

      if (OB_ISNULL(lease_mgr_ptr_) || OB_ISNULL(ts_sync_mgr_ptr_) || OB_ISNULL(tablets_mgr_ptr_)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        DUP_TABLE_LOG(WARN, "alloc memory in ObDupTableLSHandler::init failed", K(ret),
                      KP(lease_mgr_ptr_), KP(ts_sync_mgr_ptr_), KP(tablets_mgr_ptr_));
      } else {
        new (lease_mgr_ptr_) ObDupTableLSLeaseMgr();
        new (ts_sync_mgr_ptr_) ObDupTableLSTsSyncMgr();
        new (tablets_mgr_ptr_) ObLSDupTabletsMgr();

        if (OB_FAIL(lease_mgr_ptr_->init(this))) {
          DUP_TABLE_LOG(WARN, "init lease_mgr failed", K(ret));
        } else if (OB_FAIL(ts_sync_mgr_ptr_->init(this))) {
          DUP_TABLE_LOG(WARN, "init ts_sync_mgr failed", K(ret));
        } else if (OB_FAIL(tablets_mgr_ptr_->init(this))) {
          DUP_TABLE_LOG(WARN, "init tablets_mgr failed", K(ret));
        } else if (ls_state_helper_.is_leader() && OB_FAIL(leader_takeover_(true /*is_resume*/))) {
          DUP_TABLE_LOG(WARN, "leader takeover in init failed", K(ret));
        } else {
          ATOMIC_STORE(&is_inited_, true);
        }
      }

      if (OB_FAIL(ret)) {
        if (OB_NOT_NULL(lease_mgr_ptr_)) {
          lease_mgr_ptr_->destroy();
          ob_free(lease_mgr_ptr_);
        }
        if (OB_NOT_NULL(ts_sync_mgr_ptr_)) {
          ts_sync_mgr_ptr_->destroy();
          ob_free(ts_sync_mgr_ptr_);
        }
        if (OB_NOT_NULL(tablets_mgr_ptr_)) {
          tablets_mgr_ptr_->destroy();
          ob_free(tablets_mgr_ptr_);
        }
        lease_mgr_ptr_ = nullptr;
        ts_sync_mgr_ptr_ = nullptr;
        tablets_mgr_ptr_ = nullptr;
      }
      DUP_TABLE_LOG(INFO, "ls handler init", K(ret), KPC(this));
    }
  }
  return ret;
}

void ObDupTableLSHandler::start()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  ObDupTableLSRoleStateContainer restore_state_container;
  if (OB_FAIL(ls_state_helper_.prepare_state_change(ObDupTableLSRoleState::LS_START_SUCC,
                                                    restore_state_container))) {
    DUP_TABLE_LOG(WARN, "prepare state change failed", K(ret), KPC(this));
    if (OB_NO_NEED_UPDATE == ret) {
      ret = OB_SUCCESS;
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(ls_state_helper_.state_change_succ(ObDupTableLSRoleState::LS_START_SUCC,
                                                   restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "change ls role state error", K(ret), KPC(this));
    }
  }
}

void ObDupTableLSHandler::stop()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  ObDupTableLSRoleStateContainer restore_state_container;
  if (OB_FAIL(ls_state_helper_.prepare_state_change(ObDupTableLSRoleState::LS_STOP_SUCC,
                                                    restore_state_container))) {
    DUP_TABLE_LOG(WARN, "prepare state change failed", K(ret), KPC(this));
    if (OB_NO_NEED_UPDATE == ret) {
      ret = OB_SUCCESS;
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(ls_state_helper_.state_change_succ(ObDupTableLSRoleState::LS_STOP_SUCC,
                                                   restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "change ls role state error", K(ret), KPC(this));
    }
  }
}

int ObDupTableLSHandler::offline()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  ObDupTableLSRoleStateContainer restore_state_container;
  if (OB_FAIL(ls_state_helper_.prepare_state_change(ObDupTableLSRoleState::LS_OFFLINE_SUCC,
                                                    restore_state_container))) {
    DUP_TABLE_LOG(WARN, "prepare state change failed", K(ret), KPC(this));
    if (OB_NO_NEED_UPDATE == ret) {
      ret = OB_SUCCESS;
    }
  } else if (OB_NOT_NULL(log_operator_) && log_operator_->is_busy()) {
    ret = OB_EAGAIN;
    DUP_TABLE_LOG(WARN, "wait log synced before offline", K(ret), KPC(this));
  } else if (OB_NOT_NULL(tablets_mgr_ptr_) && OB_FAIL(tablets_mgr_ptr_->offline())) {
    DUP_TABLE_LOG(WARN, "dup tablets mgr offline failed", K(ret), KPC(this));
  } else if (OB_NOT_NULL(lease_mgr_ptr_) && OB_FAIL(lease_mgr_ptr_->offline())) {
    DUP_TABLE_LOG(WARN, "dup lease mgr offline failed", K(ret), KPC(this));
  } else if (OB_NOT_NULL(ts_sync_mgr_ptr_) && OB_FAIL(ts_sync_mgr_ptr_->offline())) {
    DUP_TABLE_LOG(WARN, "dup ts mgr offline failed", K(ret), KPC(this));
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(ls_state_helper_.state_change_succ(ObDupTableLSRoleState::LS_OFFLINE_SUCC,
                                                   restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "change ls role state error", K(ret), KPC(this));
    }
  }
  return ret;
}

int ObDupTableLSHandler::online()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  ObDupTableLSRoleStateContainer restore_state_container;
  if (OB_FAIL(ls_state_helper_.prepare_state_change(ObDupTableLSRoleState::LS_ONLINE_SUCC,
                                                    restore_state_container))) {
    DUP_TABLE_LOG(WARN, "prepare state change failed", K(ret), KPC(this));
    if (OB_NO_NEED_UPDATE == ret) {
      ret = OB_SUCCESS;
    }
  } else if (OB_FAIL(dup_ls_ckpt_.online())) {
    DUP_TABLE_LOG(WARN, "dup table checkpoint online failed", K(ret), KPC(this));
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(ls_state_helper_.state_change_succ(ObDupTableLSRoleState::LS_ONLINE_SUCC,
                                                   restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "change ls role state error", K(ret), KPC(this));
    }
  }

  return ret;
}

int ObDupTableLSHandler::safe_to_destroy(bool &is_dup_table_handler_safe)
{
  int ret = OB_SUCCESS;
  is_dup_table_handler_safe = false;
  if (OB_NOT_NULL(log_operator_) && log_operator_->is_busy()) {
    ret = OB_EAGAIN;
    DUP_TABLE_LOG(WARN, "wait log synced before destroy", K(ret), KPC(this));
  }

  if (OB_SUCC(ret)) {
    is_dup_table_handler_safe = true;
  }

  return ret;
}

void ObDupTableLSHandler::destroy() { reset(); }

// int ObDupTableLSHandler::offline()
// {
//   int ret = OB_SUCCESS;
//   }
//   return ret;
// }
//
// int ObDupTableLSHandler::online() {}

void ObDupTableLSHandler::reset()
{
  // ATOMIC_STORE(&is_inited_, false);
  is_inited_ = false;
  ls_state_helper_.reset();

  dup_ls_ckpt_.reset();

  if (OB_NOT_NULL(lease_mgr_ptr_)) {
    lease_mgr_ptr_->destroy();
    ob_free(lease_mgr_ptr_);
  }
  if (OB_NOT_NULL(ts_sync_mgr_ptr_)) {
    ts_sync_mgr_ptr_->destroy();
    ob_free(ts_sync_mgr_ptr_);
  }
  if (OB_NOT_NULL(tablets_mgr_ptr_)) {
    tablets_mgr_ptr_->destroy();
    ob_free(tablets_mgr_ptr_);
  }
  if (OB_NOT_NULL(log_operator_)) {
    log_operator_->reset();
    share::mtl_free(log_operator_);
  }

  lease_mgr_ptr_ = nullptr;
  ts_sync_mgr_ptr_ = nullptr;
  tablets_mgr_ptr_ = nullptr;
  log_operator_ = nullptr;

  total_block_confirm_ref_ = 0;
  self_max_replayed_scn_.reset();
  committing_dup_trx_cnt_ = 0;

  interface_stat_.reset();
  for (int i = 0; i < DupTableDiagStd::TypeIndex::MAX_INDEX; i++) {
    last_diag_info_print_us_[i] = 0;
  }
}

// bool ObDupTableLSHandler::is_master()
// {
//   bool sub_master = true;
//   if (OB_NOT_NULL(ts_sync_mgr_ptr_)) {
//     sub_master = sub_master && ts_sync_mgr_ptr_->is_master();
//   }
//   if (OB_NOT_NULL(lease_mgr_ptr_)) {
//     sub_master = sub_master && lease_mgr_ptr_->is_master();
//   }
//   if (OB_NOT_NULL(tablets_mgr_ptr_)) {
//     sub_master = sub_master && tablets_mgr_ptr_->is_master();
//   }
//
//   return (ATOMIC_LOAD(&is_master_)) && sub_master;
// }

// bool ObDupTableLSHandler::is_follower()
// {
//   bool sub_not_master = true;
//   if (OB_NOT_NULL(ts_sync_mgr_ptr_)) {
//     sub_not_master = sub_not_master && !ts_sync_mgr_ptr_->is_master();
//   }
//   if (OB_NOT_NULL(lease_mgr_ptr_)) {
//     sub_not_master = sub_not_master && !lease_mgr_ptr_->is_master();
//   }
//   if (OB_NOT_NULL(tablets_mgr_ptr_)) {
//     sub_not_master = sub_not_master && !tablets_mgr_ptr_->is_master();
//   }
//
//   return (!ATOMIC_LOAD(&is_master_)) && sub_not_master;
// }

bool ObDupTableLSHandler::is_inited()
{
  return ATOMIC_LOAD(&is_inited_);
}

bool ObDupTableLSHandler::is_master() { return ls_state_helper_.is_leader_serving(); }

// bool ObDupTableLSHandler::is_online()
// {
//   bool sub_online =  true;
//   if (OB_NOT_NULL(ts_sync_mgr_ptr_)) {
//     sub_online = sub_online && !ts_sync_mgr_ptr_->is_master();
//   }
//   if (OB_NOT_NULL(lease_mgr_ptr_)) {
//     sub_online = sub_online && !lease_mgr_ptr_->is_master();
//   }
//   if (OB_NOT_NULL(tablets_mgr_ptr_)) {
//     sub_online = sub_online && !tablets_mgr_ptr_->is_master();
//   }
//
// }

int ObDupTableLSHandler::ls_loop_handle()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  if (!is_inited() || OB_ISNULL(lease_mgr_ptr_) || OB_ISNULL(tablets_mgr_ptr_)
      || OB_ISNULL(ts_sync_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "dup table ls handle not init", K(ret));
  } else if (!ls_state_helper_.is_active_ls()) {
    ret = OB_LS_OFFLINE;
    DUP_TABLE_LOG(WARN, "the ls is not active", K(ret), KPC(this));
  } else if (!check_tablet_set_exist()) {
    // if tablet set not exist,
    // return OB_NO_TABLET and remove ls id form map
    // else do ls loop handle
    ret = OB_NO_TABLET;
    DUP_TABLE_LOG(INFO, "no dup tablet, no need to do loop worker", K(ret),
                  KPC(tablets_mgr_ptr_));
  } else {
    if (ls_state_helper_.is_leader()) {
      if (OB_ISNULL(log_operator_) || !log_operator_->is_busy()) {
        // handle lease request and collect follower info
        DupTableTsInfo min_lease_ts_info;
        if (OB_FAIL(get_min_lease_ts_info_(min_lease_ts_info))) {
          DUP_TABLE_LOG(WARN, "get min lease ts info failed", K(ret), K(min_lease_ts_info));
          // try confirm tablets and check tablet need log
        } else if (OB_FAIL(try_to_confirm_tablets_(min_lease_ts_info.max_replayed_scn_))) {
          DUP_TABLE_LOG(WARN, "try confirm tablets failed", K(ret), K(min_lease_ts_info));
        } else {
          // submit lease log
          if (OB_FAIL(prepare_log_operator_())) {
            DUP_TABLE_LOG(WARN, "prepare log operator failed", K(ret));
          } else if (OB_FAIL(log_operator_->submit_log_entry())) {
            DUP_TABLE_LOG(WARN, "submit dup table log entry failed", K(ret));
          }
        }
      }

      // update ts info cache
      if (OB_TMP_FAIL(ts_sync_mgr_ptr_->update_all_ts_info_cache())) {
        DUP_TABLE_LOG(WARN, "update all ts info cache failed", K(tmp_ret));
      }

    } else if (ls_state_helper_.is_follower()) {
      if (OB_FAIL(lease_mgr_ptr_->follower_handle())) {
        DUP_TABLE_LOG(WARN, "follower lease handle failed", K(ret));
      }
    } else {
      DUP_TABLE_LOG(INFO, "undefined role state for dup table ls, skip this loop", K(ret),
                    K(ls_id_), K(ls_state_helper_));
    }
    DUP_TABLE_LOG(DEBUG, "loop running : dup table ls handler", K(ret), K(ls_id_),
                  K(ls_state_helper_), KP(lease_mgr_ptr_), KP(tablets_mgr_ptr_),
                  KP(log_operator_));

    const int64_t fast_cur_time = ObTimeUtility::fast_current_time();
    const bool is_leader = ls_state_helper_.is_leader();

    if (fast_cur_time - last_diag_info_print_us_[DupTableDiagStd::TypeIndex::LEASE_INDEX]
        >= DupTableDiagStd::DUP_DIAG_PRINT_INTERVAL[DupTableDiagStd::TypeIndex::LEASE_INDEX]) {
      _DUP_TABLE_LOG(INFO, "[%sDup Interface Stat] tenant: %lu, ls: %lu, is_master: %s, %s",
                     DupTableDiagStd::DUP_DIAG_COMMON_PREFIX, MTL_ID(), ls_id_.id(),
                     to_cstring(is_leader), to_cstring(interface_stat_));
    }

    if (fast_cur_time - last_diag_info_print_us_[DupTableDiagStd::TypeIndex::LEASE_INDEX]
        >= DupTableDiagStd::DUP_DIAG_PRINT_INTERVAL[DupTableDiagStd::TypeIndex::LEASE_INDEX]) {
      lease_mgr_ptr_->print_lease_diag_info_log(is_leader);
      last_diag_info_print_us_[DupTableDiagStd::TypeIndex::LEASE_INDEX] = fast_cur_time;
    }

    if (fast_cur_time - last_diag_info_print_us_[DupTableDiagStd::TypeIndex::TABLET_SET_INDEX]
        >= DupTableDiagStd::DUP_DIAG_PRINT_INTERVAL[DupTableDiagStd::TypeIndex::TABLET_SET_INDEX]) {
      tablets_mgr_ptr_->print_tablet_diag_info_log(is_leader);
      last_diag_info_print_us_[DupTableDiagStd::TypeIndex::TABLET_SET_INDEX] = fast_cur_time;
    }

    if (fast_cur_time - last_diag_info_print_us_[DupTableDiagStd::TypeIndex::TS_SYNC_INDEX]
        >= DupTableDiagStd::DUP_DIAG_PRINT_INTERVAL[DupTableDiagStd::TypeIndex::TS_SYNC_INDEX]) {
      ts_sync_mgr_ptr_->print_ts_sync_diag_info_log(is_leader);
      last_diag_info_print_us_[DupTableDiagStd::TypeIndex::TS_SYNC_INDEX] = fast_cur_time;
    }
  }

  return ret;
}

int ObDupTableLSHandler::refresh_dup_table_tablet(common::ObTabletID tablet_id,
                                                  bool is_dup_table,
                                                  int64_t refresh_time)
{
  int ret = OB_SUCCESS;

  if (!is_inited() || OB_ISNULL(tablets_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    if (is_dup_table) {
      DUP_TABLE_LOG(WARN, "ObDupTableLSHandler not init", K(ret), K(is_inited_), K(is_dup_table),
                    KPC(this));
    }
  } else if (!ls_state_helper_.is_leader()) {
    ret = OB_NOT_MASTER;
    DUP_TABLE_LOG(WARN, "No need to refresh dup table schema", K(ret), K(tablet_id),
                  K(is_dup_table), KPC(this));
  } else if (OB_FAIL(tablets_mgr_ptr_->refresh_dup_tablet(tablet_id, is_dup_table, refresh_time))) {
    if (ret != OB_NOT_MASTER) {
      DUP_TABLE_LOG(WARN, "refresh dup table tablet failed", K(ret), K(tablet_id), K(is_dup_table),
                    KPC(this));
    } else {
      ret = OB_SUCCESS;
    }
  }

  return ret;
}

int ObDupTableLSHandler::recive_lease_request(const ObDupTableLeaseRequest &lease_req)
{
  int ret = OB_SUCCESS;
  if (!is_inited() || OB_ISNULL(lease_mgr_ptr_) || OB_ISNULL(ts_sync_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "DupTableLSHandler not init", K(ret), K(is_inited_), KP(lease_mgr_ptr_));
  } else if (!ls_state_helper_.is_leader_serving()) {
    ret = OB_NOT_MASTER;
    DUP_TABLE_LOG(WARN, "No need to recive lease request", K(ret), K(lease_req), KPC(this));
  } else if (OB_FAIL(ts_sync_mgr_ptr_->handle_ts_sync_response(lease_req))) {
    DUP_TABLE_LOG(WARN, "handle ts sync response failed", K(ret));
  } else if (OB_FAIL(lease_mgr_ptr_->recive_lease_request(lease_req))) {
    DUP_TABLE_LOG(WARN, "recive lease request failed", K(ret), K(lease_req));
  }
  return ret;
}

int ObDupTableLSHandler::handle_ts_sync_response(const ObDupTableTsSyncResponse &ts_sync_resp)
{
  int ret = OB_SUCCESS;

  if (!is_inited() || OB_ISNULL(ts_sync_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "DupTableLSHandler not init", K(ret), K(is_inited_), KP(ts_sync_mgr_ptr_));
  } else if (!ls_state_helper_.is_leader_serving()) {
    ret = OB_NOT_MASTER;
    DUP_TABLE_LOG(WARN, "No need to handle ts sync response", K(ret), K(ts_sync_resp), KPC(this));
  } else if (OB_FAIL(ts_sync_mgr_ptr_->handle_ts_sync_response(ts_sync_resp))) {
    DUP_TABLE_LOG(WARN, "handle ts sync response failed", K(ret));
  }

  return ret;
}

int ObDupTableLSHandler::handle_ts_sync_request(const ObDupTableTsSyncRequest &ts_sync_req)
{
  int ret = OB_SUCCESS;

  if (!is_inited() || OB_ISNULL(ts_sync_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "DupTableLSHandler not init", K(ret), K(is_inited_), KP(ts_sync_mgr_ptr_));
  } else if (!ls_state_helper_.is_follower_serving()) {
    ret = OB_NOT_FOLLOWER;
    DUP_TABLE_LOG(WARN, "No need to handle ts sync request", K(ret), K(ts_sync_req), KPC(this));
  } else if (OB_FAIL(ts_sync_mgr_ptr_->handle_ts_sync_request(ts_sync_req))) {
    DUP_TABLE_LOG(WARN, "handle ts sync request failed", K(ret));
  }

  return ret;
}

int ObDupTableLSHandler::check_redo_sync_completed(const ObTransID &tx_id,
                                                   const share::SCN &redo_completed_scn,
                                                   bool &redo_sync_finish,
                                                   share::SCN &total_max_read_version)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  LeaseAddrArray lease_addrs;
  redo_sync_finish = false;
  int64_t redo_sync_succ_cnt = 0;
  total_max_read_version.set_invalid();
  share::SCN tmp_max_read_version;
  tmp_max_read_version.set_invalid();

  const int64_t GET_GTS_TIMEOUT = 1 * 1000 * 1000; // 1s
  share::SCN before_prepare_gts;
  before_prepare_gts.set_invalid();
  int64_t start_us = OB_INVALID_TIMESTAMP;

  if (!is_inited() || OB_ISNULL(lease_mgr_ptr_) || OB_ISNULL(ts_sync_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "ObDupTableLSHandler not init", K(ret), K(is_inited_), KP(lease_mgr_ptr_),
                  KP(ts_sync_mgr_ptr_));
  } else if (OB_FAIL(lease_mgr_ptr_->get_lease_valid_array(lease_addrs))) {
    DUP_TABLE_LOG(WARN, "get lease valid array failed", K(ret));
  } else if (lease_addrs.count() == 0) {
    redo_sync_finish = true;
    total_max_read_version.set_min(); // min scn
    DUP_TABLE_LOG(INFO, "no follower with valid lease, redo sync finish", K(ret), K(tx_id),
                  K(ls_id_), K(redo_completed_scn), K(redo_sync_finish), K(total_max_read_version));
  } else {
    tmp_max_read_version.set_min();
    for (int i = 0; OB_SUCC(ret) && i < lease_addrs.count(); i++) {
      bool replay_all_redo = false;
      share::SCN max_read_version;
      max_read_version.set_invalid();
      if (OB_FAIL(ts_sync_mgr_ptr_->validate_replay_ts(lease_addrs[i], redo_completed_scn, tx_id,
                                                       replay_all_redo, max_read_version))) {
        DUP_TABLE_LOG(WARN, "validate replay ts failed", K(ret), K(lease_addrs[i]),
                      K(redo_completed_scn));
      } else if (replay_all_redo) {
        if (!max_read_version.is_valid()) {
          ret = OB_ERR_UNEXPECTED;
          DUP_TABLE_LOG(WARN, "unexpected max read version", K(ret), K(replay_all_redo),
                        K(max_read_version));
        } else if (tmp_max_read_version.is_valid()) {
          tmp_max_read_version = share::SCN::max(tmp_max_read_version, max_read_version);
        } else {
          tmp_max_read_version = max_read_version;
        }
        if (OB_SUCC(ret)) {
          redo_sync_succ_cnt++;
        }
      }

      // get_gts && retry to post before_prepare request
      if ((OB_SUCC(ret) && !replay_all_redo) || OB_FAIL(ret)) {
        int tmp_ret = OB_SUCCESS;
        if (!before_prepare_gts.is_valid()) {
          share::SCN tmp_gts;
          tmp_gts.set_invalid();
          start_us = ObTimeUtility::fast_current_time();
          MonotonicTs rts(0);
          do {
            const int64_t now = ObTimeUtility::fast_current_time();
            const MonotonicTs stc =
                MonotonicTs(now) - MonotonicTs(GCONF._ob_get_gts_ahead_interval);
            if (now >= start_us + GET_GTS_TIMEOUT) {
              tmp_ret = OB_TIMEOUT;
              DUP_TABLE_LOG(WARN, "wait gts for too long time", K(now), K(start_us),
                            K(before_prepare_gts));
            } else if (OB_TMP_FAIL(MTL(ObTransService *)
                                       ->get_ts_mgr()
                                       ->get_gts(MTL_ID(), stc, NULL, tmp_gts, rts))) {
              if (OB_EAGAIN == tmp_ret) {
                ob_usleep(1000);
              } else {
                DUP_TABLE_LOG(WARN, "get gts fail", K(tmp_ret), K(now));
              }
            } else if (OB_UNLIKELY(!tmp_gts.is_valid())) {
              tmp_ret = OB_ERR_UNEXPECTED;
              TRANS_LOG(WARN, "invalid snapshot from gts", K(tmp_gts), K(now));
            } else {
              // do nothing
            }
          } while (tmp_ret == OB_EAGAIN);

          if (OB_SUCCESS == tmp_ret) {
            before_prepare_gts = tmp_gts;
          }
        }

        if (OB_SUCCESS == tmp_ret && before_prepare_gts > redo_completed_scn) {
          const common::ObAddr self_addr = MTL(ObTransService *)->get_server();
          ObDupTableBeforePrepareRequest before_prepare_req(tx_id, before_prepare_gts);
          before_prepare_req.set_header(self_addr, lease_addrs[i], self_addr, ls_id_);
          if (OB_TMP_FAIL(MTL(ObTransService *)
                              ->get_dup_table_rpc_impl()
                              .post_msg(lease_addrs[i], before_prepare_req))) {
            DUP_TABLE_LOG(WARN, "post ts sync request failed", K(tmp_ret));
          }
        }
      }
    }

    if (OB_SUCC(ret) && redo_sync_succ_cnt == lease_addrs.count()) {
      redo_sync_finish = true;
      total_max_read_version = tmp_max_read_version;

      DUP_TABLE_LOG(INFO, "redo sync finish with lease valid follower", K(ret), K(ls_id_), K(tx_id),
                    K(redo_completed_scn), K(redo_sync_finish), K(total_max_read_version),
                    K(lease_addrs));
    }
  }

  if (redo_sync_finish) {
    interface_stat_.dup_table_redo_sync_succ_cnt_++;
  } else {
    interface_stat_.dup_table_redo_sync_fail_cnt_++;
  }

  return ret;
}

int ObDupTableLSHandler::block_confirm_with_dup_tablet_change_snapshot(
    share::SCN &dup_tablet_change_snapshot)
{
  int ret = OB_SUCCESS;

  ATOMIC_INC(&total_block_confirm_ref_);

  if (!is_inited()) {
    // do nothing
    ret = OB_SUCCESS;
  } else {
  }

  return ret;
}

int ObDupTableLSHandler::gc_dup_tablets(const int64_t gc_ts, const int64_t max_task_interval)
{
  int ret = OB_SUCCESS;

  if (!is_inited() || !ls_state_helper_.is_leader_serving()) {
    // do nothing
  } else if (OB_ISNULL(tablets_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "ObDupTableLSHandler not init", K(ret), K(is_inited_),
                  KP(tablets_mgr_ptr_));
  } else if (0 > gc_ts || 0 > max_task_interval) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid gc_time", K(ret), K(gc_ts), K(max_task_interval));
  } else if (OB_FAIL(tablets_mgr_ptr_->gc_dup_tablets(gc_ts, max_task_interval))) {
    DUP_TABLE_LOG(WARN, "lose dup tablet failed", KR(ret), K(gc_ts));
  }

  return ret;
}

int ObDupTableLSHandler::try_to_confirm_tablets_(const share::SCN &confirm_scn)
{
  int ret = OB_SUCCESS;

  if (!is_inited() || OB_ISNULL(tablets_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "ObDupTableLSHandler not init", K(ret), K(is_inited_),
                  KP(tablets_mgr_ptr_));
  } else if (!confirm_scn.is_valid() || share::SCN::max_scn() == confirm_scn) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid confrim_time", K(ret), K(confirm_scn));
  } else if (OB_FAIL(tablets_mgr_ptr_->try_to_confirm_tablets(confirm_scn))) {
    DUP_TABLE_LOG(WARN, "confirm tablets failed", K(ret), K(confirm_scn));
  }
  // for debug
  DUP_TABLE_LOG(DEBUG, "ls finish confirm tablets", K(ret), K(confirm_scn));
  return ret;
}

int ObDupTableLSHandler::unblock_confirm_with_prepare_scn(
    const share::SCN &dup_tablet_change_snapshot,
    const share::SCN &redo_scn)
{
  int ret = OB_SUCCESS;

  return ret;
}

int ObDupTableLSHandler::check_dup_tablet_in_redo(const ObTabletID &tablet_id,
                                                  bool &is_dup_tablet,
                                                  const share::SCN &base_snapshot,
                                                  const share::SCN &redo_scn)
{
  int ret = OB_SUCCESS;
  is_dup_tablet = false;

  if (!tablet_id.is_valid() || !base_snapshot.is_valid() || !redo_scn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid argument", K(ret), K(tablet_id), K(base_snapshot), K(redo_scn));
  } else if (OB_ISNULL(lease_mgr_ptr_) || OB_ISNULL(tablets_mgr_ptr_)) {
    is_dup_tablet = false;
  } else if (!has_dup_tablet()) {
    is_dup_tablet = false;
  } else if (OB_FAIL(tablets_mgr_ptr_->find_dup_tablet_in_set(tablet_id, is_dup_tablet,
                                                              base_snapshot, redo_scn))) {
    DUP_TABLE_LOG(WARN, "check dup tablet failed", K(ret), K(tablet_id), K(base_snapshot),
                  K(redo_scn));
  }
  return ret;
}

int ObDupTableLSHandler::check_dup_tablet_readable(const ObTabletID &tablet_id,
                                                   const share::SCN &read_snapshot,
                                                   const bool read_from_leader,
                                                   const share::SCN &max_replayed_scn,
                                                   bool &readable)
{
  int ret = OB_SUCCESS;

  share::SCN tmp_max_replayed_scn = max_replayed_scn;
  readable = false;
  if (!tablet_id.is_valid() || !read_snapshot.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid argument", K(ret), K(tablet_id), K(read_snapshot), K(readable));
  } else if (OB_ISNULL(lease_mgr_ptr_) || OB_ISNULL(tablets_mgr_ptr_)) {
    // no dup tablet in ls
    readable = false;
  } else if (!ls_state_helper_.is_active_ls()) {
    readable = false;
    DUP_TABLE_LOG(INFO, "the ls is not active", K(ret), KPC(this));
  } else if (!has_dup_tablet()) {
    readable = false;
    interface_stat_.dup_table_follower_read_tablet_not_exist_cnt_++;
    // use read_from_leader to validate lease;
    DUP_TABLE_LOG(INFO, "no dup tablet can be read", K(ret), KPC(tablets_mgr_ptr_),
                  K(read_from_leader), K(tmp_max_replayed_scn));
  } else if (!tmp_max_replayed_scn.is_valid()
             && (OB_ISNULL(log_handler_)
                 || OB_FAIL(log_handler_->get_max_decided_scn(tmp_max_replayed_scn)))) {
    DUP_TABLE_LOG(WARN, "get max replayed scn for dup table read failed", K(ret), K(ls_id_),
                  K(tablet_id), K(read_snapshot), KP(log_handler_), K(tmp_max_replayed_scn));
    // rewrite ret code when get max replayed scn failed to drive retry
    ret = OB_NOT_MASTER;
  } else if (OB_FAIL(check_and_update_max_replayed_scn(max_replayed_scn))) {
    DUP_TABLE_LOG(WARN, "invalid max_replayed_scn", K(ret), K(tablet_id), K(read_snapshot),
                  K(read_from_leader));
  } else if (false
             == lease_mgr_ptr_->check_follower_lease_serving(read_from_leader,
                                                             tmp_max_replayed_scn)) {
    readable = false;
    interface_stat_.dup_table_follower_read_lease_expired_cnt_++;
    DUP_TABLE_LOG(INFO, "lease is expired for read", K(ret), K(tablet_id), K(read_snapshot),
                  K(read_from_leader), K(tmp_max_replayed_scn));
  } else if (OB_FAIL(tablets_mgr_ptr_->check_readable(tablet_id, readable, read_snapshot,
                                                      interface_stat_))) {
    DUP_TABLE_LOG(WARN, "check dup tablet failed", K(ret), K(tablet_id), K(read_snapshot));
  }

  if (readable) {
    interface_stat_.dup_table_follower_read_succ_cnt_++;
  }

  return ret;
}

int64_t ObDupTableLSHandler::get_dup_tablet_count()
{
  int64_t dup_tablet_cnt = 0;

  if (OB_ISNULL(tablets_mgr_ptr_)) {
    dup_tablet_cnt = 0;
  } else {
    dup_tablet_cnt = tablets_mgr_ptr_->get_dup_tablet_count();
  }

  return dup_tablet_cnt;
}

bool ObDupTableLSHandler::has_dup_tablet()
{
  bool has_dup = false;
  if (OB_ISNULL(tablets_mgr_ptr_)) {
    has_dup = false;
  } else {
    has_dup = tablets_mgr_ptr_->has_dup_tablet();
  }
  return has_dup;
}

// if return false, there are no tablets and tablet set need log
bool ObDupTableLSHandler::check_tablet_set_exist()
{
  bool bool_ret = false;

  if (OB_ISNULL(tablets_mgr_ptr_)) {
    bool_ret = false;
  } else {
    int64_t readable_and_need_confirm_set_count =
              tablets_mgr_ptr_->get_readable_tablet_set_count()
              + tablets_mgr_ptr_->get_need_confirm_tablet_set_count();

    // if readable and need confirm set count > 0, return true
    if (readable_and_need_confirm_set_count > 0 ) {
      bool_ret = true;
    } else {
      // if changing new and removing set exist return true
      bool chaning_and_removing_tablet_exist =
          tablets_mgr_ptr_->check_changing_new_tablet_exist()
          || tablets_mgr_ptr_->check_removing_tablet_exist();
      if (chaning_and_removing_tablet_exist) {
        bool_ret = true;
      } else {
        bool_ret = false;
      }
    }
  }

  return bool_ret;
}

int ObDupTableLSHandler::get_local_ts_info(DupTableTsInfo &ts_info)
{
  int ret = OB_SUCCESS;

  if (!is_inited() || OB_ISNULL(ts_sync_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "DupTableLSHandler not init", K(ret), K(is_inited_), KP(ts_sync_mgr_ptr_));
  } else if (!ls_state_helper_.is_active_ls()) {
    ret = OB_LS_OFFLINE;
    DUP_TABLE_LOG(WARN, "the ls is not active", K(ret), KPC(this));
  } else if (OB_FAIL(ts_sync_mgr_ptr_->get_local_ts_info(ts_info))) {
    DUP_TABLE_LOG(WARN, "get local ts sync info failed", K(ret));
  }

  return ret;
}

int ObDupTableLSHandler::get_cache_ts_info(const common::ObAddr &addr, DupTableTsInfo &ts_info)
{
  int ret = OB_SUCCESS;

  if (!is_inited() || OB_ISNULL(ts_sync_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "DupTableLSHandler not init", K(ret), K(is_inited_), KP(ts_sync_mgr_ptr_));
  } else if (!ls_state_helper_.is_leader_serving()) {
    ret = OB_NOT_MASTER;
    DUP_TABLE_LOG(WARN, "we can not get cached ts info from a follower", K(ret), K(ts_info),
                  KPC(this));
  } else if (OB_FAIL(ts_sync_mgr_ptr_->get_cache_ts_info(addr, ts_info))) {
    DUP_TABLE_LOG(WARN, "get cache ts info failed", K(ret), K(addr), K(ts_info));
  }
  return ret;
}

int ObDupTableLSHandler::replay(const void *buffer,
                                const int64_t nbytes,
                                const palf::LSN &lsn,
                                const share::SCN &ts_ns)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  const bool no_dup_tablet_before_replay = !has_dup_tablet();

  // cover lease list and tablets list
  if (!is_inited() && OB_FAIL(init(true))) {
    DUP_TABLE_LOG(WARN, "init dup_ls_handle in replay failed", K(ret));
  } else if (OB_FAIL(prepare_log_operator_())) {
    DUP_TABLE_LOG(WARN, "init dup_table log operator failed", K(ret));
  } else if (OB_FAIL(
                 log_operator_->merge_replay_block(static_cast<const char *>(buffer), nbytes))) {
    if (OB_SUCCESS == ret) {
      DUP_TABLE_LOG(INFO, "merge replay buf success, may be completed", K(ret));
    } else if (OB_START_LOG_CURSOR_INVALID == ret) {
      DUP_TABLE_LOG(WARN, "start replay from the middle of log entry, skip this dup_table log",
                    K(ts_ns), K(lsn));
      // ret = OB_SUCCESS;
    } else {
      DUP_TABLE_LOG(WARN, "merge replay buf failed", K(ret));
    }
  } else if (OB_FAIL(log_operator_->deserialize_log_entry())) {
    DUP_TABLE_LOG(WARN, "deserialize log block failed", K(ret), K(ts_ns));
  } else if (OB_FAIL(lease_mgr_ptr_->follower_try_acquire_lease(ts_ns))) {
    DUP_TABLE_LOG(WARN, "acquire lease from lease log error", K(ret), K(ts_ns));
  } else {
    log_operator_->set_logging_scn(ts_ns);
    ret = log_operator_->replay_succ();
    DUP_TABLE_LOG(INFO, "replay dup_table log success", K(ret), K(nbytes), K(lsn), K(ts_ns),
                  KPC(tablets_mgr_ptr_), KPC(lease_mgr_ptr_));
    // log_operator_->reuse();V
  }

  // start require lease instantly
  if (OB_FAIL(ret)) {
    if (OB_NOT_NULL(log_operator_)) {
      log_operator_->reuse();
    }
  } else if (no_dup_tablet_before_replay && check_tablet_set_exist()
             && OB_TMP_FAIL(
                 MTL(ObTransService *)->get_dup_table_loop_worker().append_dup_table_ls(ls_id_))) {
    DUP_TABLE_LOG(WARN, "refresh dup table ls failed", K(tmp_ret), K(ls_id_), K(lsn), K(ts_ns));
  }

  return ret;
}

void ObDupTableLSHandler::switch_to_follower_forcedly()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  ObDupTableLSRoleStateContainer restore_state_container;
  if (OB_FAIL(ls_state_helper_.prepare_state_change(ObDupTableLSRoleState::LS_REVOKE_SUCC,
                                                    restore_state_container))) {
    DUP_TABLE_LOG(WARN, "prepare state change failed", K(ret), KPC(this));
    if (OB_NO_NEED_UPDATE == ret) {
      ret = OB_SUCCESS;
    }
  } else if (OB_FAIL(leader_revoke_(true /*forcedly*/))) {
    DUP_TABLE_LOG(ERROR, "switch to follower forcedly failed for dup table", K(ret), KPC(this));
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(ls_state_helper_.state_change_succ(ObDupTableLSRoleState::LS_REVOKE_SUCC,
                                                   restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "change ls role state error", K(ret), KPC(this));
    }
  }
}

int ObDupTableLSHandler::switch_to_follower_gracefully()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  ObDupTableLSRoleStateContainer restore_state_container;
  if (OB_FAIL(ls_state_helper_.prepare_state_change(ObDupTableLSRoleState::LS_REVOKE_SUCC,
                                                    restore_state_container))) {
    DUP_TABLE_LOG(WARN, "prepare state change failed", K(ret), KPC(this));
    if (OB_NO_NEED_UPDATE == ret) {
      ret = OB_SUCCESS;
    }
  } else if (OB_FAIL(leader_revoke_(false /*forcedly*/))) {
    DUP_TABLE_LOG(WARN, "switch to follower gracefully failed for dup table", K(ret), KPC(this));
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(ls_state_helper_.state_change_succ(ObDupTableLSRoleState::LS_REVOKE_SUCC,
                                                   restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "change ls role state error", K(ret), KPC(this));
    }
  } else {
    tmp_ret = OB_SUCCESS;

    if (OB_TMP_FAIL(leader_takeover_(true /*is_resume*/))) {
      DUP_TABLE_LOG(WARN, "resume leader failed", K(ret), K(tmp_ret), KPC(this));
    }
    if (OB_SUCCESS != tmp_ret) {
      ret = OB_LS_NEED_REVOKE;
      DUP_TABLE_LOG(WARN, "resume leader failed, need revoke", K(ret), K(tmp_ret), KPC(this));
    } else if (OB_TMP_FAIL(ls_state_helper_.restore_state(ObDupTableLSRoleState::LS_REVOKE_SUCC,
                                                          restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "restore ls role state error", K(ret), KPC(this));
    }
  }

  return ret;
}

int ObDupTableLSHandler::resume_leader()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  const bool is_resume = true;

  ObDupTableLSRoleStateContainer restore_state_container;
  if (OB_FAIL(ls_state_helper_.prepare_state_change(ObDupTableLSRoleState::LS_TAKEOVER_SUCC,
                                                    restore_state_container))) {
    DUP_TABLE_LOG(WARN, "prepare state change failed", K(ret), KPC(this));
    if (OB_NO_NEED_UPDATE == ret) {
      ret = OB_SUCCESS;
    }
  } else if (OB_FAIL(leader_takeover_(is_resume))) {
    DUP_TABLE_LOG(WARN, "resume leader failed for dup table", K(ret), KPC(this));
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(ls_state_helper_.state_change_succ(ObDupTableLSRoleState::LS_TAKEOVER_SUCC,
                                                   restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "change ls role state error", K(ret), KPC(this));
    }
  } else {
    tmp_ret = OB_SUCCESS;

    if (OB_TMP_FAIL(leader_revoke_(true /*forcedly*/))) {
      DUP_TABLE_LOG(WARN, "leader revoke failed", K(ret), K(tmp_ret), KPC(this));
    } else if (OB_TMP_FAIL(ls_state_helper_.restore_state(ObDupTableLSRoleState::LS_TAKEOVER_SUCC,
                                                          restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "restore ls role state error", K(ret), KPC(this));
    }
  }
  return ret;
}

int ObDupTableLSHandler::switch_to_leader()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  const bool is_resume = false;

  ObDupTableLSRoleStateContainer restore_state_container;
  if (OB_FAIL(ls_state_helper_.prepare_state_change(ObDupTableLSRoleState::LS_TAKEOVER_SUCC,
                                                    restore_state_container))) {
    DUP_TABLE_LOG(WARN, "prepare state change failed", K(ret), KPC(this));
    if (OB_NO_NEED_UPDATE == ret) {
      ret = OB_SUCCESS;
    }
  } else if (OB_FAIL(leader_takeover_(is_resume))) {
    DUP_TABLE_LOG(WARN, "leader takeover failed for dup table", K(ret), KPC(this));
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(ls_state_helper_.state_change_succ(ObDupTableLSRoleState::LS_TAKEOVER_SUCC,
                                                   restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "change ls role state error", K(ret), KPC(this));
    }
  } else {
    tmp_ret = OB_SUCCESS;

    if (OB_TMP_FAIL(leader_revoke_(true /*forcedly*/))) {
      DUP_TABLE_LOG(WARN, "leader revoke failed", K(ret), K(tmp_ret), KPC(this));
    } else if (OB_TMP_FAIL(ls_state_helper_.restore_state(ObDupTableLSRoleState::LS_TAKEOVER_SUCC,
                                                          restore_state_container))) {
      DUP_TABLE_LOG(ERROR, "restore ls role state error", K(ret), KPC(this));
    }
  }
  return ret;
}

int ObDupTableLSHandler::leader_revoke_(const bool is_forcedly)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;

  bool is_logging = false;
  if (OB_NOT_NULL(log_operator_)) {
    log_operator_->rlock_for_log();
    is_logging = log_operator_->check_is_busy_without_lock();
  }
  if (OB_NOT_NULL(tablets_mgr_ptr_) && OB_TMP_FAIL(tablets_mgr_ptr_->leader_revoke(is_logging))) {
    DUP_TABLE_LOG(WARN, "tablets_mgr switch to follower failed", K(ret), K(tmp_ret), KPC(this));
    if (!is_forcedly) {
      ret = tmp_ret;
    }
  }
  if (OB_NOT_NULL(log_operator_)) {
    log_operator_->unlock_for_log();
  }

  if (OB_SUCC(ret) && OB_NOT_NULL(ts_sync_mgr_ptr_)) {
    if (OB_TMP_FAIL(ts_sync_mgr_ptr_->leader_revoke())) {
      DUP_TABLE_LOG(WARN, "ts_sync_mgr switch to follower failed", K(ret), K(tmp_ret), KPC(this));
      if (!is_forcedly) {
        ret = tmp_ret;
      }
    }
  }

  if (OB_SUCC(ret) && OB_NOT_NULL(lease_mgr_ptr_)) {
    if (OB_TMP_FAIL(lease_mgr_ptr_->leader_revoke())) {
      DUP_TABLE_LOG(WARN, "lease_mgr switch to follower failed", K(ret), K(tmp_ret), KPC(this));
      if (!is_forcedly) {
        ret = tmp_ret;
      }
    }
  }

  interface_stat_.reset();
  DUP_TABLE_LOG(INFO, "Leader Revoke", K(ret), K(is_forcedly), KPC(this));
  return ret;
}

int ObDupTableLSHandler::leader_takeover_(const bool is_resume)
{
  int ret = OB_SUCCESS;

  // clean ts info cache
  if (OB_NOT_NULL(ts_sync_mgr_ptr_)) {
    ts_sync_mgr_ptr_->leader_takeover();
  }
  // extend lease_expired_time
  if (OB_NOT_NULL(lease_mgr_ptr_)) {
    lease_mgr_ptr_->leader_takeover(is_resume);
  }

  if (OB_NOT_NULL(tablets_mgr_ptr_)) {
    if (OB_FAIL(tablets_mgr_ptr_->leader_takeover(
            is_resume, dup_ls_ckpt_.contain_all_readable_on_replica()))) {
      DUP_TABLE_LOG(WARN, "clean unreadable tablet set failed", K(ret));
    }
  }

  interface_stat_.reset();
  DUP_TABLE_LOG(INFO, "Leader Takeover", K(ret), K(is_resume), KPC(this));
  return ret;
}

int ObDupTableLSHandler::prepare_log_operator_()
{
  int ret = OB_SUCCESS;

  // need release in reset()
  if (OB_ISNULL(log_operator_)) {
    if (OB_ISNULL(log_operator_ = static_cast<ObDupTableLogOperator *>(
                      share::mtl_malloc(sizeof(ObDupTableLogOperator), "DUP_LOG_OP")))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      DUP_TABLE_LOG(WARN, "malloc log operator failed", K(ret));
    } else {
      new (log_operator_) ObDupTableLogOperator(ls_id_, log_handler_, &dup_ls_ckpt_, lease_mgr_ptr_,
                                                tablets_mgr_ptr_);
    }
  }

  return ret;
}

int ObDupTableLSHandler::set_dup_table_ls_meta(
    const ObDupTableLSCheckpoint::ObLSDupTableMeta &dup_ls_meta,
    bool need_flush_slog)
{
  int ret = OB_SUCCESS;

  DUP_TABLE_LOG(INFO, "try to recover dup table ls meta", K(ret), K(need_flush_slog),
                K(dup_ls_meta), KPC(this));

  if (OB_FAIL(dup_ls_ckpt_.set_dup_ls_meta(dup_ls_meta))) {
    DUP_TABLE_LOG(WARN, "set dup ls meta failed", K(ret), K(need_flush_slog), K(dup_ls_meta),
                  KPC(this));
  } else if (need_flush_slog && OB_FAIL(dup_ls_ckpt_.flush())) {
    DUP_TABLE_LOG(WARN, "flush slog failed", K(ret), K(need_flush_slog), K(dup_ls_meta), KPC(this));
  }

  return ret;
}

int ObDupTableLSHandler::flush(share::SCN &rec)
{
  int ret = OB_SUCCESS;

  if (!ls_state_helper_.is_started()) {
    ret = OB_LS_NOT_EXIST;
    DUP_TABLE_LOG(WARN, "this ls is not started", K(ret), K(rec), KPC(this));

  } else {
    ret = dup_ls_ckpt_.flush();
  }

  return ret;
}

int ObDupTableLSHandler::check_and_update_max_replayed_scn(const share::SCN &max_replayed_scn)
{
  int ret = OB_SUCCESS;
  if (!max_replayed_scn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid max_replayed_scn", K(ret), K(max_replayed_scn));
  } else if (!self_max_replayed_scn_.atomic_get().is_valid()) {
    self_max_replayed_scn_.atomic_set(max_replayed_scn);
    last_max_replayed_scn_update_ts_ = ObTimeUtility::fast_current_time();
  } else if (max_replayed_scn >= self_max_replayed_scn_.atomic_get()) {
    self_max_replayed_scn_.atomic_set(max_replayed_scn);
    last_max_replayed_scn_update_ts_ = ObTimeUtility::fast_current_time();
  } else if (max_replayed_scn < self_max_replayed_scn_.atomic_get()
             && self_max_replayed_scn_.atomic_get().convert_to_ts(true)
                        - max_replayed_scn.convert_to_ts(true)
                    > 100 * 1000) {
    // ret = OB_ERR_UNEXPECTED;
    DUP_TABLE_LOG(WARN, "the max_replayed_scn has been rollbacked", K(ret), K(ls_id_),
                  K(max_replayed_scn), K(self_max_replayed_scn_),
                  K(last_max_replayed_scn_update_ts_));
  }

  return ret;
}

int ObDupTableLSHandler::get_min_lease_ts_info_(DupTableTsInfo &min_ts_info)
{
  int ret = OB_SUCCESS;

  LeaseAddrArray lease_valid_array;
  min_ts_info.reset();

  if (!is_inited() || OB_ISNULL(lease_mgr_ptr_) || OB_ISNULL(ts_sync_mgr_ptr_)) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid arguments", K(is_inited_), KP(lease_mgr_ptr_),
                  KP(ts_sync_mgr_ptr_));
  } else if (OB_FAIL(ts_sync_mgr_ptr_->get_local_ts_info(min_ts_info))) {
    DUP_TABLE_LOG(WARN, "get local ts info failed", K(ret));
  } else if (!min_ts_info.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid local ts info", K(ret), K(min_ts_info));
  } else if (OB_FAIL(lease_mgr_ptr_->get_lease_valid_array(lease_valid_array))) {
    DUP_TABLE_LOG(WARN, "get lease valid array failed", K(ret));
  } else {
    DupTableTsInfo tmp_ts_info;
    for (int64_t i = 0; OB_SUCC(ret) && i < lease_valid_array.count(); i++) {
      if (OB_FAIL(ts_sync_mgr_ptr_->get_cache_ts_info(lease_valid_array[i], tmp_ts_info))) {
        DUP_TABLE_LOG(WARN, "get cache ts info failed", K(ret), K(lease_valid_array[i]));
      } else {
        min_ts_info.max_replayed_scn_ =
            share::SCN::min(min_ts_info.max_replayed_scn_, tmp_ts_info.max_replayed_scn_);
        min_ts_info.max_read_version_ =
            share::SCN::min(min_ts_info.max_read_version_, tmp_ts_info.max_read_version_);
        min_ts_info.max_commit_version_ =
            share::SCN::min(min_ts_info.max_commit_version_, tmp_ts_info.max_commit_version_);
      }
    }
  }

  if (OB_FAIL(ret)) {
    DUP_TABLE_LOG(INFO, "get min lease ts info failed", K(ret), K(min_ts_info),
                  K(lease_valid_array));
  }
  return ret;
}

int ObDupTableLSHandler::get_lease_mgr_stat(ObDupLSLeaseMgrStatIterator &collect_iter)
{
  int ret = OB_SUCCESS;
  FollowerLeaseMgrStatArr collect_arr;

  // collect all leader info
  if (ls_state_helper_.is_leader_serving()) {
    if (OB_ISNULL(lease_mgr_ptr_) || OB_ISNULL(ts_sync_mgr_ptr_)) {
      ret = OB_NOT_INIT;
      DUP_TABLE_LOG(WARN, "not init", K(ret), KPC(lease_mgr_ptr_), KP(ts_sync_mgr_ptr_));
    } else if(OB_FAIL(lease_mgr_ptr_->get_lease_mgr_stat(collect_arr))) {
      DUP_TABLE_LOG(WARN, "get lease mgr stat from lease_mgr failed", K(ret));
    } else if(OB_FAIL(ts_sync_mgr_ptr_->get_lease_mgr_stat(collect_iter, collect_arr))) {
      DUP_TABLE_LOG(WARN, "get lease mgr stat from ts_sync_mgr failed", K(ret));
    }
    DUP_TABLE_LOG(DEBUG, "get lease mgr stat", K(ret), K(collect_arr));
  }

  return ret;
}

int ObDupTableLSHandler::get_ls_tablets_stat(ObDupLSTabletsStatIterator &collect_iter)
{
  int ret = OB_SUCCESS;
  const share::ObLSID ls_id = ls_id_;

  if (OB_ISNULL(tablets_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "tablets_mgr not init", K(ret), KP(tablets_mgr_ptr_));
  } else if(OB_FAIL(tablets_mgr_ptr_->get_tablets_stat(collect_iter, ls_id_))) {
    DUP_TABLE_LOG(WARN, "get tablets stat failed", K(ret));
  }

  return ret;
}

int ObDupTableLSHandler::get_ls_tablet_set_stat(ObDupLSTabletSetStatIterator &collect_iter)
{
  int ret = OB_SUCCESS;
  const share::ObLSID ls_id = get_ls_id();

  if (OB_ISNULL(tablets_mgr_ptr_)) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "not init", K(ret), KPC(tablets_mgr_ptr_));
  } else if (OB_FAIL(tablets_mgr_ptr_->get_tablet_set_stat(collect_iter, ls_id))) {
    DUP_TABLE_LOG(WARN, "get tablet set stat failed", K(ret));
  }

  return ret;
}

//*************************************************************************************************************
//**** ObDupTableLoopWorker
//*************************************************************************************************************

int ObDupTableLoopWorker::init()
{
  int ret = OB_SUCCESS;

  if (is_inited_) {
    ret = OB_INIT_TWICE;
    DUP_TABLE_LOG(WARN, "init dup_loop_worker twice", K(ret));
  } else {
    if (OB_FAIL(dup_ls_id_set_.create(8, "DUP_LS_SET", "DUP_LS_ID", MTL_ID()))) {
      DUP_TABLE_LOG(WARN, "create dup_ls_map_ error", K(ret));
    } else {
      is_inited_ = true;
    }
  }
  DUP_TABLE_LOG(INFO, "init ObDupTableLoopWorker");
  return ret;
}

int ObDupTableLoopWorker::start()
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(WARN, "dup_loop_worker has not inited", K(ret));
  } else {
    lib::ThreadPool::set_run_wrapper(MTL_CTX());
    ret = lib::ThreadPool::start();
  }
  DUP_TABLE_LOG(INFO, "start ObDupTableLoopWorker", KR(ret));
  return ret;
}

void ObDupTableLoopWorker::stop()
{
  if (!has_set_stop()) {
    DUP_TABLE_LOG(INFO, "stop ObDupTableLoopWorker");
  }
  lib::ThreadPool::stop();
}

void ObDupTableLoopWorker::wait()
{
  lib::ThreadPool::wait();
  DUP_TABLE_LOG(INFO, "wait ObDupTableLoopWorker");
}

void ObDupTableLoopWorker::destroy()
{
  lib::ThreadPool::destroy();
  (void)dup_ls_id_set_.destroy();
  DUP_TABLE_LOG(INFO, "destroy ObDupTableLoopWorker");
}

void ObDupTableLoopWorker::reset()
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dup_ls_id_set_.clear())) {
    DUP_TABLE_LOG(WARN, "clear dup_ls_set failed", KR(ret));
  }
  is_inited_ = false;
}

void ObDupTableLoopWorker::run1()
{
  int ret = OB_SUCCESS;
  int64_t start_time = 0;
  int64_t time_used = 0;
  DupLSIDSet_Spin::iterator iter;
  ObSEArray<share::ObLSID, 2> remove_ls_list;

  lib::set_thread_name("DupLoop");
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    DUP_TABLE_LOG(ERROR, "dup_loop_worker has not inited", K(ret));
  } else {
    while (!has_set_stop()) {
      start_time = ObTimeUtility::current_time();

      remove_ls_list.reuse();

      for (iter = dup_ls_id_set_.begin(); iter != dup_ls_id_set_.end(); iter++) {
        const share::ObLSID cur_ls_id = iter->first;
        ObLSHandle ls_handle;

        if (OB_ISNULL(MTL(ObLSService *))
            || (OB_FAIL(MTL(ObLSService *)->get_ls(cur_ls_id, ls_handle, ObLSGetMod::TRANS_MOD))
                || !ls_handle.is_valid())) {
          if (OB_SUCC(ret)) {
            ret = OB_INVALID_ARGUMENT;
          }
          DUP_TABLE_LOG(WARN, "get ls error", K(ret), K(cur_ls_id));
        } else if (OB_FAIL(ls_handle.get_ls()->get_dup_table_ls_handler()->ls_loop_handle())) {
          DUP_TABLE_LOG(WARN, "ls loop handle error", K(ret), K(cur_ls_id));
        }

        if (OB_LS_NOT_EXIST == ret || OB_NOT_INIT == ret || OB_NO_TABLET == ret) {
          remove_ls_list.push_back(cur_ls_id);
          TRANS_LOG(INFO, "try to remove invalid dup ls id", K(ret), K(cur_ls_id),
                    K(remove_ls_list));
        }
      }

      for (int index = 0; index < remove_ls_list.count(); index++) {
        if (OB_FAIL(dup_ls_id_set_.erase_refactored(remove_ls_list[index]))) {
          DUP_TABLE_LOG(WARN, "remove from dup_ls_id_set_ failed", K(ret), K(index),
                        K(remove_ls_list[index]));
        }
      }

      time_used = ObTimeUtility::current_time() - start_time;
      if (time_used < LOOP_INTERVAL) {
        usleep(LOOP_INTERVAL - time_used);
      }
    }
  }
}

int ObDupTableLoopWorker::append_dup_table_ls(const share::ObLSID &ls_id)
{
  int ret = OB_SUCCESS;
  ObDupTableLSHandler *tmp_ls_handle = nullptr;

  if (!ls_id.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "invalid ls id", K(ls_id));
  } else if (OB_FAIL(dup_ls_id_set_.set_refactored(ls_id, 0))) {
    if (OB_HASH_EXIST == ret) {
      // do nothing
    } else {
      DUP_TABLE_LOG(WARN, "insert dup_ls_handle into hash_set failed", K(ret));
    }
  }

  if (OB_SUCC(ret)) {
    DUP_TABLE_LOG(INFO, "append dup table ls success", K(ret), K(ls_id));
  } else if (OB_HASH_EXIST == ret) {
    ret = OB_SUCCESS;
  }

  if (OB_SUCC(ret) && !dup_ls_id_set_.empty() && has_set_stop()) {
    start();
  }

  return ret;
}

// trans service -> dup worker -> ls service -> dup ls handler -> iterate
int ObDupTableLoopWorker::iterate_dup_ls(ObDupLSLeaseMgrStatIterator &collect_iter)
{
  int ret = OB_SUCCESS;
  DupLSIDSet_Spin::iterator iter;
  ObLSService *ls_service = MTL(ObLSService *);

  if (OB_ISNULL(ls_service)) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "get ls service failed", K(ret), KP(ls_service));
  } else {
    for (iter = dup_ls_id_set_.begin(); iter != dup_ls_id_set_.end(); iter++) {
      const share::ObLSID cur_ls_id = iter->first;
      ObDupTableLSHandler *cur_dup_ls_handler = nullptr;
      ObLSHandle ls_handle;

      if (OB_FAIL(ls_service->get_ls(cur_ls_id, ls_handle, ObLSGetMod::TRANS_MOD))) {
        DUP_TABLE_LOG(WARN, "get ls handler error", K(ret), K(cur_ls_id), KPC(ls_service));
      } else if (!ls_handle.is_valid()) {
        ret = OB_INVALID_ARGUMENT;
        DUP_TABLE_LOG(WARN, "ls handler not valid", K(ret), K(cur_ls_id), KPC(ls_service));
      } else {
        cur_dup_ls_handler = ls_handle.get_ls()->get_dup_table_ls_handler();
        if (OB_ISNULL(cur_dup_ls_handler) || !cur_dup_ls_handler->is_inited()) {
          ret = OB_NOT_INIT;
          DUP_TABLE_LOG(WARN, "dup ls handler not init", K(ret), K(cur_ls_id),
                        KPC(cur_dup_ls_handler));
        } else if (OB_FAIL(cur_dup_ls_handler->get_lease_mgr_stat(collect_iter))) {
          DUP_TABLE_LOG(WARN, "collect lease mgr stat failed", K(ret), K(cur_ls_id),
                        KPC(cur_dup_ls_handler));
        }
      }
    }
  }

  return ret;
}

int ObDupTableLoopWorker::iterate_dup_ls(ObDupLSTabletSetStatIterator &collect_iter)
{
  int ret = OB_SUCCESS;
  DupLSIDSet_Spin::iterator iter;
  ObLSService *ls_service = MTL(ObLSService *);

  if (OB_ISNULL(ls_service)) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "get ls service failed", K(ret), KP(ls_service));
  } else {
    for (iter = dup_ls_id_set_.begin(); iter != dup_ls_id_set_.end(); iter++) {
      const share::ObLSID cur_ls_id = iter->first;
      ObDupTableLSHandler *cur_dup_ls_handler = nullptr;
      ObLSHandle ls_handle;

      if (OB_FAIL(ls_service->get_ls(cur_ls_id, ls_handle, ObLSGetMod::TRANS_MOD))) {
        DUP_TABLE_LOG(WARN, "get ls handler error", K(ret), K(cur_ls_id), KPC(ls_service));
      } else if (!ls_handle.is_valid()) {
        ret = OB_INVALID_ARGUMENT;
        DUP_TABLE_LOG(WARN, "ls handler not valid", K(ret), K(cur_ls_id), KPC(ls_service));
      } else {
        cur_dup_ls_handler = ls_handle.get_ls()->get_dup_table_ls_handler();
        if (OB_ISNULL(cur_dup_ls_handler) || !cur_dup_ls_handler->is_inited()) {
          ret = OB_NOT_INIT;
          DUP_TABLE_LOG(WARN, "dup ls handler not init", K(ret), K(cur_ls_id),
                        KPC(cur_dup_ls_handler));
        } else if (OB_FAIL(cur_dup_ls_handler->get_ls_tablet_set_stat(collect_iter))) {
          DUP_TABLE_LOG(WARN, "collect lease mgr stat failed", K(ret), K(cur_ls_id),
                        KPC(cur_dup_ls_handler));
        }
        DUP_TABLE_LOG(WARN, "iter dup ls handler", K(ret), K(cur_ls_id),
              KPC(cur_dup_ls_handler));
      }
    }
  }

  return ret;
}

int ObDupTableLoopWorker::iterate_dup_ls(ObDupLSTabletsStatIterator &collect_iter)
{
  int ret = OB_SUCCESS;
  DupLSIDSet_Spin::iterator iter;
  ObLSService *ls_service = MTL(ObLSService *);

  if (OB_ISNULL(ls_service)) {
    ret = OB_INVALID_ARGUMENT;
    DUP_TABLE_LOG(WARN, "get ls service failed", K(ret), KP(ls_service));
  } else {
    for (iter = dup_ls_id_set_.begin(); iter != dup_ls_id_set_.end(); iter++) {
      const share::ObLSID cur_ls_id = iter->first;
      ObDupTableLSHandler *cur_dup_ls_handler = nullptr;
      ObLSHandle ls_handle;

      if (OB_FAIL(ls_service->get_ls(cur_ls_id, ls_handle, ObLSGetMod::TRANS_MOD))) {
        DUP_TABLE_LOG(WARN, "get ls handler error", K(ret), K(cur_ls_id), KPC(ls_service));
      } else if (!ls_handle.is_valid()) {
        ret = OB_INVALID_ARGUMENT;
        DUP_TABLE_LOG(WARN, "ls handler not valid", K(ret), K(cur_ls_id), KPC(ls_service));
      } else {
        cur_dup_ls_handler = ls_handle.get_ls()->get_dup_table_ls_handler();
        if (OB_ISNULL(cur_dup_ls_handler) || !cur_dup_ls_handler->is_inited()) {
          ret = OB_NOT_INIT;
          DUP_TABLE_LOG(WARN, "dup ls handler not init", K(ret), K(cur_ls_id),
                        KPC(cur_dup_ls_handler));
        } else if (OB_FAIL(cur_dup_ls_handler->get_ls_tablets_stat(collect_iter))) {
          DUP_TABLE_LOG(WARN, "collect lease mgr stat failed", K(ret), K(cur_ls_id),
                        KPC(cur_dup_ls_handler));
        }
      }
    }
  }

  return ret;
}
} // namespace transaction
} // namespace oceanbase
