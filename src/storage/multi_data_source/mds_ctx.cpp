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
#include "mds_ctx.h"
#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"
#include "mds_table_handle.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace storage
{
namespace mds
{

MdsCtx::MdsCtx() : state_(TwoPhaseCommitState::STATE_INIT) {}

MdsCtx::MdsCtx(const MdsWriter &writer)
: writer_(writer),
state_(TwoPhaseCommitState::STATE_INIT) {}

MdsCtx::~MdsCtx()
{
  bool list_empty = false;
  {
    MdsWLockGuard lg(lock_);
    list_empty = write_list_.empty();
    if (!list_empty) {
      OB_ASSERT(state_ != TwoPhaseCommitState::ON_COMMIT);// if decided, list is empty
      MDS_LOG_RET(INFO, OB_SUCCESS, "nodes not commit or abort when mds ctx destroyed", K(*this));
    }
  }
  if (!list_empty) {
    on_abort(share::SCN::max_scn());
  }
}

int MdsCtx::assign(const MdsCtx &rhs) {
  MdsWLockGuard lg(lock_);
  writer_ = rhs.writer_;
  state_ = rhs.state_;
  return OB_SUCCESS;
}

const MdsWriter MdsCtx::get_writer() const { return writer_; }

void MdsCtx::set_writer(const MdsWriter &writer)
{
  MdsWLockGuard lg(lock_);
  if (state_ != TwoPhaseCommitState::STATE_INIT) {
    MDS_LOG_RET(ERROR, OB_INVALID_ARGUMENT, "not allow set writer in non-init state", K(*this), K(writer));
  } else {
    writer_.writer_type_ = writer.writer_type_;
    writer_.writer_id_ = writer.writer_id_;
  }
}

bool MdsCtx::can_write() const
{
  MdsRLockGuard lg(lock_);
  return state_ == TwoPhaseCommitState::STATE_INIT &&
          writer_.is_valid();
}

void MdsCtx::record_written_node(ListNode<MdsNode> *node)
{
  MdsWLockGuard lg(lock_);
  write_list_.append(node);
}

void MdsCtx::on_redo(const share::SCN &redo_scn)
{
  OB_ASSERT(writer_.writer_type_ == WriterType::TRANSACTION);// can only called by TRANS, or must call single_log_commit()
  do_while_retry_with_lock_until_success_for_all_([this, redo_scn]() {// if failed on any node, will release lock and try from first node again
    return for_each_node_try_([redo_scn](MdsNode &node) {// the operation tried on each node
      bool try_success = true;
      if (node.redo_scn_.is_max()) {// avoid try lock
        try_success = node.try_on_redo(redo_scn);
      }
      return try_success;
    });
  }, TwoPhaseCommitState::STATE_END);// no need advance state
}

void MdsCtx::before_prepare()
{
  OB_ASSERT(writer_.writer_type_ == WriterType::TRANSACTION);// can only called by TRANS, or must call single_log_commit()
  do_while_retry_with_lock_until_success_for_all_([this]() {// if failed on any node, will release lock and try from first node again
    return for_each_node_try_([](MdsNode &node) {// the operation tried on each node
      bool try_success = true;
      if (node.status_.get_state() < TwoPhaseCommitState::BEFORE_PREPARE) {// avoid try lock
        try_success = node.try_before_prepare();
      }
      return try_success;
    });
  }, TwoPhaseCommitState::BEFORE_PREPARE);
}

void MdsCtx::on_prepare(const share::SCN &prepare_version)
{
  OB_ASSERT(writer_.writer_type_ == WriterType::TRANSACTION);// can only called by TRANS, or must call single_log_commit()
  do_while_retry_with_lock_until_success_for_all_([this, prepare_version]() {// if failed on any node, will release lock and try from first node again
    return for_each_node_try_([prepare_version](MdsNode &node) {// the operation tried on each node
      bool try_success = true;
      if (node.status_.get_state() < TwoPhaseCommitState::ON_PREPARE) {// avoid try lock
        try_success = node.try_on_prepare(prepare_version);
      }
      return try_success;
    });
  }, TwoPhaseCommitState::ON_PREPARE);
}

void MdsCtx::on_commit(const share::SCN &commit_version, const share::SCN &commit_scn)
{
  OB_ASSERT(writer_.writer_type_ == WriterType::TRANSACTION);// can only called by TRANS, or must call single_log_commit()
  do_while_retry_with_lock_until_success_for_all_([this, commit_version, commit_scn]() {// if failed on any node, will release lock and try from first node again
    return for_each_node_fetch_to_try_([commit_version, commit_scn](MdsNode &node) {// the operation tried on each node, if failed, the fetched node will be insert to head again to rollback
      bool try_success = true;
      if (node.status_.get_state() < TwoPhaseCommitState::ON_COMMIT) {// avoid try lock
        try_success = node.try_on_commit(commit_version, commit_scn);
      }
      return try_success;
    });
  }, TwoPhaseCommitState::ON_COMMIT);
}

void MdsCtx::on_abort(const share::SCN &abort_scn)
{
  do_while_retry_with_lock_until_success_for_all_([this, abort_scn]() {// if failed on any node, will release lock and try from first node again
    return for_each_node_fetch_to_try_([abort_scn](MdsNode &node) {// the operation tried on each node, if failed, the fetched node will be insert to head again to rollback
      bool try_success = true;
      if (node.status_.get_state() < TwoPhaseCommitState::ON_ABORT) {// avoid try lock
        try_success = node.try_on_abort(abort_scn);
      }
      return try_success;
    });
  }, TwoPhaseCommitState::ON_ABORT);
}

void MdsCtx::single_log_commit(const share::SCN commit_version, const share::SCN commit_scn)
{
  do_while_retry_with_lock_until_success_for_all_([this, commit_version, commit_scn]() {// if failed on any node, will release lock and try from first node again
    return for_each_node_fetch_to_try_([commit_version, commit_scn](MdsNode &node) {// the operation tried on each node, if failed, the fetched node will be insert to head again to rollback
      bool try_success = true;
      if (node.status_.get_state() < TwoPhaseCommitState::ON_COMMIT) {// avoid try lock
        try_success = node.try_single_log_commit(commit_version, commit_scn);
      }
      return try_success;
    });
  }, TwoPhaseCommitState::ON_COMMIT);
}

void MdsCtx::single_log_abort()
{
  OB_ASSERT(writer_.writer_type_ != WriterType::TRANSACTION);// TRANSACTION use two-phase-commit
  do_while_retry_with_lock_until_success_for_all_([this]() {// if failed on any node, will release lock and try from first node again
    return for_each_node_fetch_to_try_([](MdsNode &node) {// the operation tried on each node, if failed, the fetched node will be insert to head again to rollback
      bool try_success = true;
      if (node.status_.get_state() < TwoPhaseCommitState::ON_ABORT) {// avoid try lock
        try_success = node.try_on_abort(share::SCN::max_scn());
      }
      return try_success;
    });
  }, TwoPhaseCommitState::ON_ABORT);
}

void MdsCtx::remove_node(ListNode<MdsNode> *node)
{
  MdsWLockGuard lg(lock_);
  write_list_.del(node);
}

}
}
}
