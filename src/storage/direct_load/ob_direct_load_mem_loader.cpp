// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   suzhi.yt <>

#define USING_LOG_PREFIX STORAGE

#include "storage/direct_load/ob_direct_load_mem_loader.h"
#include "storage/direct_load/ob_direct_load_external_block_reader.h"
#include "storage/direct_load/ob_direct_load_external_table.h"
#include "storage/direct_load/ob_direct_load_mem_sample.h"

namespace oceanbase
{
namespace storage
{
using namespace common;
using namespace blocksstable;

/**
 * ObDirectLoadMemLoader
 */

ObDirectLoadMemLoader::ObDirectLoadMemLoader(ObDirectLoadMemContext *mem_ctx)
  : mem_ctx_(mem_ctx)
{
}

ObDirectLoadMemLoader::~ObDirectLoadMemLoader()
{
}

int ObDirectLoadMemLoader::add_table(ObIDirectLoadPartitionTable *table)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == table)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), KP(table));
  } else {
    ObDirectLoadExternalTable *external_table = nullptr;
    if (OB_ISNULL(external_table = dynamic_cast<ObDirectLoadExternalTable *>(table))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected table", KR(ret), KPC(table));
    } else if (OB_UNLIKELY(external_table->get_fragments().count() <= 0)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("files handle should have at least one handle",
          KR(ret), K(external_table->get_fragments().count()));
    } else if (OB_FAIL(fragments_.push_back(external_table->get_fragments()))) {
      LOG_WARN("fail to push back", KR(ret));
    }
  }
  return ret;
}

int ObDirectLoadMemLoader::work()
{
  typedef ObDirectLoadExternalBlockReader<ObDirectLoadExternalMultiPartitionRow> ExternalReader;
  int ret = OB_SUCCESS;
  const ObDirectLoadExternalMultiPartitionRow *external_row = nullptr;
  ChunkType *chunk = nullptr;
  RowType row;
  for (int64_t i = 0; OB_SUCC(ret) && i < fragments_.count(); i++) {
    ObDirectLoadExternalFragment &fragment = fragments_.at(i);
    ExternalReader external_reader;
    if (OB_FAIL(external_reader.init(mem_ctx_->table_data_desc_.external_data_block_size_,
                                     fragment.max_data_block_size_,
                                     mem_ctx_->table_data_desc_.compressor_type_))) {
      LOG_WARN("fail to init external reader", KR(ret));
    } else if (OB_FAIL(external_reader.open(fragment.file_handle_, 0, fragment.file_size_))) {
      LOG_WARN("fail to open file", KR(ret));
    }
    while (OB_SUCC(ret) && !(mem_ctx_->has_error_)) {
      if (external_row == nullptr) {
        if (OB_FAIL(external_reader.get_next_item(external_row))) {
          if (OB_UNLIKELY(OB_ITER_END != ret)) {
            LOG_WARN("fail to get next item", KR(ret));
          } else {
            ret = OB_SUCCESS;
            break;
          }
        }
      }
      if (OB_SUCC(ret) && chunk == nullptr) {
        //等待内存空出
        while (mem_ctx_->fly_mem_chunk_count_ >= mem_ctx_->table_data_desc_.max_mem_chunk_count_ &&
               !(mem_ctx_->has_error_)) {
          usleep(500000);
        }
        if (mem_ctx_->has_error_) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("some error ocurr", KR(ret));
        }
        if (OB_SUCC(ret)) {
          chunk = OB_NEW(ChunkType, ObMemAttr(MTL_ID(), "TLD_MemChunkVal"));
          if (chunk == nullptr) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_WARN("fail to allocate mem", KR(ret));
          } else {
            ATOMIC_AAF(&(mem_ctx_->fly_mem_chunk_count_), 1);
            if (OB_FAIL(chunk->init(MTL_ID(), mem_ctx_->table_data_desc_.mem_chunk_size_))) {
              LOG_WARN("fail to init external sort", KR(ret));
            }
          }
        }
      }

      if (OB_SUCC(ret)) {
        row = *external_row;
        ret = chunk->add_item(row);
        if (ret == OB_BUF_NOT_ENOUGH) {
          ret = OB_SUCCESS;
          if (OB_FAIL(close_chunk(chunk))) {
            LOG_WARN("fail to close chunk", KR(ret));
          }
        } else if (ret != OB_SUCCESS) {
          LOG_WARN("fail to add item", KR(ret));
        } else {
          external_row = nullptr;
        }
      }
    }
    if (OB_SUCC(ret)) {
      fragment.reset();
    }
  }

  if (OB_SUCC(ret)) {
    if (chunk != nullptr && OB_FAIL(close_chunk(chunk))) {
      LOG_WARN("fail to close chunk", KR(ret));
    }
  }

  if (chunk != nullptr) {
    chunk->~ChunkType();
    ob_free(chunk);
    chunk = nullptr;
  }

  return ret;
}

int ObDirectLoadMemLoader::close_chunk(ChunkType *&chunk)
{
  int ret = OB_SUCCESS;
  CompareType compare;
  if (OB_FAIL(compare.init(*(mem_ctx_->datum_utils_)))) {
    LOG_WARN("fail to init compare", KR(ret));
  } else if (OB_FAIL(chunk->sort(compare))) {
    LOG_WARN("fail to sort chunk", KR(ret));
  } else if (OB_FAIL(mem_ctx_->mem_chunk_queue_.push(chunk))) {
    LOG_WARN("fail to push", KR(ret));
  } else {
    chunk = nullptr;
  }

  if (chunk != nullptr) {
    chunk->~ChunkType();
    ob_free(chunk);
    chunk = nullptr;
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
