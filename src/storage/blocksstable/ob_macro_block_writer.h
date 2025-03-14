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

#ifndef OCEANBASE_STORAGE_BLOCKSSTABLE_MACRO_BLOCK_WRITER_H_
#define OCEANBASE_STORAGE_BLOCKSSTABLE_MACRO_BLOCK_WRITER_H_
#include "share/io/ob_io_manager.h"
#include "encoding/ob_micro_block_decoder.h"
#include "encoding/ob_micro_block_encoder.h"
#include "lib/compress/ob_compressor.h"
#include "lib/container/ob_array_wrap.h"
#include "ob_block_manager.h"
#include "ob_index_block_row_struct.h"
#include "ob_macro_block_checker.h"
#include "ob_macro_block_reader.h"
#include "ob_macro_block.h"
#include "ob_macro_block_struct.h"
#include "ob_micro_block_encryption.h"
#include "ob_micro_block_reader.h"
#include "ob_micro_block_writer.h"
#include "share/schema/ob_table_schema.h"
#include "ob_bloom_filter_cache.h"
#include "ob_micro_block_reader_helper.h"
#include "share/cache/ob_kvcache_pre_warmer.h"

namespace oceanbase
{
namespace blocksstable
{
class ObDataIndexBlockBuilder;
class ObSSTableIndexBuilder;
class ObSSTableSecMetaIterator;
struct ObIndexBlockRowDesc;
struct ObMacroBlockDesc;
class ObIMacroBlockFlushCallback;
// macro block store struct
//  |- ObMacroBlockCommonHeader
//  |- ObSSTableMacroBlockHeader
//  |- column types
//  |- column orders
//  |- column checksum
//  |- MicroBlock 1
//  |- MicroBlock 2
//  |- MicroBlock N
class ObMicroBlockBufferHelper
{
public:
  ObMicroBlockBufferHelper();
  ~ObMicroBlockBufferHelper() = default;
  int open(
      ObDataStoreDesc &data_store_desc,
      const ObITableReadInfo &read_info,
      common::ObIAllocator &allocator);
  int compress_encrypt_micro_block(ObMicroBlockDesc &micro_block_desc);
  int dump_micro_block_writer_buffer(const char *buf, const int64_t size);
  void reset();
private:
  int prepare_micro_block_reader(
      const char *buf,
      const int64_t size,
      ObIMicroBlockReader *&micro_reader);
  int check_micro_block_checksum(
      const char *buf,
      const int64_t size,
      const int64_t checksum/*check this checksum*/);
  int check_micro_block(
      const char *compressed_buf,
      const int64_t compressed_size,
      const char *uncompressed_buf,
      const int64_t uncompressed_size,
      const ObMicroBlockDesc &micro_block_desc/*check for this micro block*/);
  void print_micro_block_row(ObIMicroBlockReader *micro_reader);

private:
  ObDataStoreDesc *data_store_desc_;
  int64_t micro_block_merge_verify_level_;
  ObMicroBlockCompressor compressor_;
  ObMicroBlockEncryption encryption_;
  ObMicroBlockReaderHelper check_reader_helper_;
  blocksstable::ObDatumRow check_datum_row_;
  ObArenaAllocator allocator_;
};

class ObMicroBlockAdaptiveSplitter
{
struct ObMicroCompressionInfo{
  ObMicroCompressionInfo();
  virtual ~ObMicroCompressionInfo() {};
  void update(const int64_t original_size, const int64_t compressed_size);
  inline void reset() { original_size_ = compressed_size_ = 0; compression_ratio_ = 100; }
  int64_t original_size_;
  int64_t compressed_size_;
  int64_t compression_ratio_;
};

public:
  ObMicroBlockAdaptiveSplitter();
  ~ObMicroBlockAdaptiveSplitter();
  int init(const int64_t macro_store_size, const bool is_use_adaptive);
  void reset();
  int check_need_split(const int64_t micro_size,
                       const int64_t micro_row_count,
                       const int64_t split_size,
                       const int64_t current_macro_size,
                       const bool is_keep_space,
                       bool &check_need_split) const;
int update_compression_info(const int64_t micro_row_count, const int64_t original_size, const int64_t compressed_size);
private:
  static const int64_t DEFAULT_MICRO_ROW_COUNT = 16;
  static const int64_t MICRO_ROW_MIN_COUNT = 3;

private:
  int64_t macro_store_size_;
  bool is_use_adaptive_;
  ObMicroCompressionInfo compression_infos_[DEFAULT_MICRO_ROW_COUNT + 1]; //compression_infos_[0] for total compression info
};

class ObMacroBlockWriter
{
public:
  ObMacroBlockWriter();
  virtual ~ObMacroBlockWriter();
  virtual void reset();
  virtual int open(
      ObDataStoreDesc &data_store_desc,
      const ObMacroDataSeq &start_seq,
      ObIMacroBlockFlushCallback *callback = nullptr);
  virtual int append_macro_block(const ObMacroBlockDesc &macro_desc);
  virtual int append_micro_block(const ObMicroBlock &micro_block, const ObMacroBlockDesc *curr_macro_desc = nullptr);
  virtual int append_row(const ObDatumRow &row, const ObMacroBlockDesc *curr_macro_desc = nullptr);
  int append_index_micro_block(ObMicroBlockDesc &micro_block_desc);
  int check_data_macro_block_need_merge(const ObMacroBlockDesc &macro_desc, bool &need_merge);
  int close();
  void dump_block_and_writer_buffer();
  inline ObMacroBlocksWriteCtx &get_macro_block_write_ctx() { return block_write_ctx_; }
  inline int64_t get_last_macro_seq() const { return current_macro_seq_; } /* save our seq num */
  TO_STRING_KV(K_(block_write_ctx));
  static int build_micro_writer(ObDataStoreDesc *data_store_desc,
                                ObIAllocator &allocator,
                                ObIMicroBlockWriter *&micro_writer,
                                const int64_t verify_level = MICRO_BLOCK_MERGE_VERIFY_LEVEL::ENCODING_AND_COMPRESSION);

protected:
  virtual int build_micro_block();
  virtual int try_switch_macro_block();
  virtual bool is_keep_freespace() const {return false; }
  inline bool is_dirty() const { return macro_blocks_[current_index_].is_dirty() || 0 != micro_writer_->get_row_count(); }
  inline int64_t get_curr_micro_writer_row_count() const { return micro_writer_->get_row_count(); }
  inline int64_t get_macro_data_size() const { return macro_blocks_[current_index_].get_data_size() + micro_writer_->get_block_size(); }

private:
  int append_row(const ObDatumRow &row, const int64_t split_size);
  int check_order(const ObDatumRow &row);
  int init_hash_index_builder();
  int append_row_and_hash_index(const ObDatumRow &row);
  int build_micro_block_desc(
      const ObMicroBlock &micro_block,
      ObMicroBlockDesc &micro_block_desc,
      ObMicroBlockHeader &header_for_rewrite);
  int build_hash_index_block(ObMicroBlockDesc &micro_block_desc);
  int build_micro_block_desc_with_rewrite(
      const ObMicroBlock &micro_block,
      ObMicroBlockDesc &micro_block_desc,
      ObMicroBlockHeader &header);
  int build_micro_block_desc_with_reuse(const ObMicroBlock &micro_block, ObMicroBlockDesc &micro_block_desc);
  int write_micro_block(ObMicroBlockDesc &micro_block_desc);
  int check_micro_block_need_merge(const ObMicroBlock &micro_block, bool &need_merge);
  int merge_micro_block(const ObMicroBlock &micro_block);
  int flush_macro_block(ObMacroBlock &macro_block);
  int wait_io_finish(ObMacroBlockHandle &macro_handle);
  int alloc_block();
  int check_write_complete(const MacroBlockId &macro_block_id);
  int save_last_key(const ObDatumRow &row);
  int save_last_key(const ObDatumRowkey &last_key);
  int add_row_checksum(const ObDatumRow &row);
  int calc_micro_column_checksum(
      const int64_t column_cnt,
      ObIMicroBlockReader &reader,
      int64_t *column_checksum);
  int flush_reuse_macro_block(const ObDataMacroBlockMeta &macro_meta);
  int open_bf_cache_writer(const ObDataStoreDesc &desc, const int64_t bloomfilter_size);
  int flush_bf_to_cache(ObMacroBloomFilterCacheWriter &bf_cache_writer, const int32_t row_count);
  void dump_micro_block(ObIMicroBlockWriter &micro_writer);
  void dump_macro_block(ObMacroBlock &macro_block);
public:
  static const int64_t DEFAULT_MACRO_BLOCK_REWRTIE_THRESHOLD = 30;
private:
  static const int64_t DEFAULT_MACRO_BLOCK_COUNT = 128;
  typedef common::ObSEArray<MacroBlockId, DEFAULT_MACRO_BLOCK_COUNT> MacroBlockList;

protected:
  ObDataStoreDesc *data_store_desc_;

private:
  ObIMicroBlockWriter *micro_writer_;
  ObMicroBlockReaderHelper reader_helper_;
  ObMicroBlockHashIndexBuilder hash_index_builder_;
  ObMicroBlockBufferHelper micro_helper_;
  ObRowkeyReadInfo read_info_;
  ObMacroBlock macro_blocks_[2];
  ObMacroBloomFilterCacheWriter bf_cache_writer_[2];//associate with macro_blocks
  int64_t current_index_;
  int64_t current_macro_seq_;        // set by sstable layer;
  ObMacroBlocksWriteCtx block_write_ctx_;
  ObMacroBlockHandle macro_handles_[2];
  ObDatumRowkey last_key_;
  bool last_key_with_L_flag_;
  bool is_macro_or_micro_block_reused_;
  int64_t *curr_micro_column_checksum_;
  common::ObArenaAllocator allocator_;
  common::ObArenaAllocator rowkey_allocator_;
  blocksstable::ObMacroBlockReader macro_reader_;
  common::ObArray<uint32_t> micro_rowkey_hashs_;
  ObSSTableMacroBlockChecker macro_block_checker_;
  common::SpinRWLock lock_;
  blocksstable::ObDatumRow datum_row_;
  blocksstable::ObDatumRow check_datum_row_;
  ObIMacroBlockFlushCallback *callback_;
  ObDataIndexBlockBuilder *builder_;
  ObMicroBlockAdaptiveSplitter micro_block_adaptive_splitter_;
  ObDataBlockCachePreWarmer data_block_pre_warmer_;
};

}//end namespace blocksstable
}//end namespace oceanbase
#endif
