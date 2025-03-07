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

#define USING_LOG_PREFIX SHARE

#include <gtest/gtest.h>
#include "lib/container/ob_array_iterator.h"
#define private public
#include "share/backup/ob_backup_path.h"

using namespace oceanbase;
using namespace common;
using namespace share;

TEST(ObBackupUtils, check_is_tmp_file)
{
  bool is_tmp_file = false;
  ObString file_name_1 = "file_name.tmp.123456";
  ASSERT_EQ(OB_SUCCESS, ObBackupUtils::check_is_tmp_file(file_name_1, is_tmp_file));
  ASSERT_TRUE(is_tmp_file);
  ObString file_name_2 = "file_name.TMP.123456";
  ASSERT_EQ(OB_SUCCESS, ObBackupUtils::check_is_tmp_file(file_name_2, is_tmp_file));
  ASSERT_FALSE(is_tmp_file);
  ObString file_name_3 = "file_name.tmp";
  ASSERT_EQ(OB_SUCCESS, ObBackupUtils::check_is_tmp_file(file_name_3, is_tmp_file));
  ASSERT_FALSE(is_tmp_file);
  ObString file_name_4 = "file_name.tmp.";
  ASSERT_EQ(OB_SUCCESS, ObBackupUtils::check_is_tmp_file(file_name_4, is_tmp_file));
  ASSERT_FALSE(is_tmp_file);
  ObString file_name_5 = "file_name";
  ASSERT_EQ(OB_SUCCESS, ObBackupUtils::check_is_tmp_file(file_name_5, is_tmp_file));
  ASSERT_FALSE(is_tmp_file);
  ObString file_name_6 = "file_name.123456";
  ASSERT_EQ(OB_SUCCESS, ObBackupUtils::check_is_tmp_file(file_name_6, is_tmp_file));
  ASSERT_FALSE(is_tmp_file);
}

TEST(ObBackupDest, nfs)
{
  const char *backup_test = "file:///backup_dir/?&delete_mode=tagging";
  ObBackupDest dest;
  ObBackupDest dest1;
  char backup_dest_str[OB_MAX_BACKUP_DEST_LENGTH] = { 0 };
  char backup_path_str[OB_MAX_BACKUP_DEST_LENGTH] = { 0 };
  ASSERT_EQ(OB_SUCCESS, dest.set(backup_test));
  ASSERT_EQ(OB_SUCCESS, dest1.set(backup_test));
  LOG_INFO("dump backup dest", K(dest), K(dest.get_root_path()), K(*(dest.get_storage_info())));
  ASSERT_EQ(0, strcmp(dest.root_path_, "file:///backup_dir"));
  ASSERT_TRUE(dest.storage_info_->device_type_ == 1);
  ObString backup_str("file:///backup_dir/");
  ObBackupDest dest2;
  ASSERT_EQ(OB_SUCCESS, dest2.set(backup_str));
  ASSERT_EQ(0, strcmp(dest.root_path_, "file:///backup_dir"));

  ASSERT_EQ(OB_SUCCESS, dest.get_backup_dest_str(backup_dest_str, sizeof(backup_dest_str)));
  ASSERT_EQ(0, strcmp(backup_dest_str, "file:///backup_dir"));
  ASSERT_EQ(OB_SUCCESS, dest.get_backup_path_str(backup_path_str, sizeof(backup_path_str)));
  ASSERT_EQ(0, strcmp(backup_path_str, "file:///backup_dir")); 
  ASSERT_TRUE(dest.is_root_path_equal(dest1));
  bool is_equal = false;
  ASSERT_EQ(OB_SUCCESS, dest.is_backup_path_equal(dest1, is_equal));
  ASSERT_TRUE(is_equal);
  ASSERT_TRUE(dest == dest1);
  ASSERT_EQ(OB_SUCCESS, dest2.deep_copy(dest));
  ASSERT_EQ(0, strcmp(dest.root_path_, "file:///backup_dir"));
}


int main(int argc, char **argv)
{
  OB_LOGGER.set_log_level("INFO");
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
