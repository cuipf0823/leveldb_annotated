// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table_builder.h"

#include <assert.h>
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

struct TableBuilder::Rep 
{
  Options options;	            //data block的选项
  Options index_block_options;	//index block的选项
  WritableFile* file;           //sstable文件
  uint64_t offset;              //要写入data_block在sstable文件中的偏移 
  Status status;
  BlockBuilder data_block;	    //当前操作的data_block
  BlockBuilder index_block;		//sstable中的index_block
  std::string last_key;	        //当前data_block最后一个kv对的key值
  int64_t num_entries;          //当前data_block的个数 
  bool closed;					// Either Finish() or Abandon() has been called.
  FilterBlockBuilder* filter_block; //根据filter数据快速定位key是否在block中

  // We do not emit the index entry for a block until we have seen the
  // first key for the next data block.  This allows us to use shorter
  // keys in the index block.  For example, consider a block boundary
  // between the keys "the quick brown fox" and "the who".  We can use
  // "the r" as the key for the index block entry since it is >= all
  // entries in the first block and < all entries in subsequent
  // blocks.
  //
  // Invariant: r->pending_index_entry is true only if data_block is empty.
  bool pending_index_entry;
  BlockHandle pending_handle;  // Handle to add to index block	 添加到index block 的data block的信息（offset， size）

  std::string compressed_output;   //压缩之后的data block，用于临时存储，写后即被清空

  Rep(const Options& opt, WritableFile* f)
      : options(opt),
        index_block_options(opt),
        file(f),
        offset(0),
        data_block(&options),
        index_block(&index_block_options),
        num_entries(0),
        closed(false),
        filter_block(opt.filter_policy == NULL ? NULL : new FilterBlockBuilder(opt.filter_policy)),
        pending_index_entry(false) 
  {
    index_block_options.block_restart_interval = 1;
  }
};

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : rep_(new Rep(options, file)) 
{
  if (rep_->filter_block != NULL) 
  {
    rep_->filter_block->StartBlock(0);
  }
}

TableBuilder::~TableBuilder()
{
  assert(rep_->closed);  // Catch errors where caller forgot to call Finish()
  delete rep_->filter_block;
  delete rep_;
}

Status TableBuilder::ChangeOptions(const Options& options) 
{
  // Note: if more fields are added to Options, update
  // this function to catch changes that should not be allowed to
  // change in the middle of building a Table.
  if (options.comparator != rep_->options.comparator) 
  {
    return Status::InvalidArgument("changing comparator while building table");
  }

  // Note that any live BlockBuilders point to rep_->options and therefore
  // will automatically pick up the updated options.
  rep_->options = options;
  rep_->index_block_options = options;
  rep_->index_block_options.block_restart_interval = 1;
  return Status::OK();
}

void TableBuilder::Add(const Slice& key, const Slice& value) 
{
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;

  //如果已经插入过数据，要保证当前插入的key > 之前最后一次插入的key sstable必须是有序插入
  if (r->num_entries > 0)
  {
    assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
  }

  /*
	r->pending_index_entry == true 表示data_block为空, 初始化时该值为False
  */
  if (r->pending_index_entry) 
  {
    assert(r->data_block.empty());
	//获取大于last_key但是小于key的最小值
    r->options.comparator->FindShortestSeparator(&r->last_key, key);
    std::string handle_encoding;
    r->pending_handle.EncodeTo(&handle_encoding);
	//将找到的last_key和data block相关信息Encode后添加到index block中
    r->index_block.Add(r->last_key, Slice(handle_encoding));
    r->pending_index_entry = false;
  }

  if (r->filter_block != NULL) 
  {
    r->filter_block->AddKey(key);
  }

  r->last_key.assign(key.data(), key.size());
  r->num_entries++;
  r->data_block.Add(key, value);

  const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
  //插入大小达到预设block默认值（4k），将block写到数据文件中
  if (estimated_block_size >= r->options.block_size)  
  {
    Flush();
  }
}

void TableBuilder::Flush() 
{
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok())
  {
	return;
  }
  if (r->data_block.empty())
  {
	return;
  }
  assert(!r->pending_index_entry);
  WriteBlock(&r->data_block, &r->pending_handle);
  if (ok()) 
  {
    r->pending_index_entry = true;
	/*
	  刷文件内容到物理磁盘中
	*/
    r->status = r->file->Flush(); 
  }
  /*
	filter_block默认为NULL
  */
  if (r->filter_block != NULL) 
  {
    r->filter_block->StartBlock(r->offset);
  }
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) 
{
  // File format contains a sequence of blocks where each block has:
  //    block_data: uint8[n]
  //    type: uint8
  //    crc: uint32
  assert(ok());
  Rep* r = rep_;
  /*
	在block完成时候, 添加restarts集合中的元素和总个数添加的data_block的末尾;
  */
  Slice raw = block->Finish();

  Slice block_contents;
  CompressionType type = r->options.compression;
  // TODO(postrelease): Support more compression options: zlib?
  switch (type)
  {
    case kNoCompression:
      block_contents = raw;
      break;

    case kSnappyCompression: 
	{
      std::string* compressed = &r->compressed_output;
      if (port::Snappy_Compress(raw.data(), raw.size(), compressed) && compressed->size() < raw.size() - (raw.size() / 8u)) 
	  {
        block_contents = *compressed;
      } 
	  else 
	  {
        // Snappy not supported, or compressed less than 12.5%, so just
        // store uncompressed form
        block_contents = raw;
        type = kNoCompression;
      }
      break;
    }
  }
  WriteRawBlock(block_contents, type, handle);
  r->compressed_output.clear();
  block->Reset();
}

//block处理之后的数据写入文件中
void TableBuilder::WriteRawBlock(const Slice& block_contents, CompressionType type, BlockHandle* handle) 
{
  Rep* r = rep_;
  handle->set_offset(r->offset);
  handle->set_size(block_contents.size());
  r->status = r->file->Append(block_contents);	//block写入sstable文件中
  if (r->status.ok())
  {
    char trailer[kBlockTrailerSize];
    trailer[0] = type;
    uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size());
    crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type
    EncodeFixed32(trailer+1, crc32c::Mask(crc));
    r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));  //trailer写入sstable文件中

    if (r->status.ok())
	{
      r->offset += block_contents.size() + kBlockTrailerSize;
    }
  }
}

Status TableBuilder::status() const 
{
  return rep_->status;
}

Status TableBuilder::Finish() 
{
  Rep* r = rep_;
  Flush();
  assert(!r->closed);
  r->closed = true;

  BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

  // Write filter block
  if (ok() && r->filter_block != NULL) 
  {
    WriteRawBlock(r->filter_block->Finish(), kNoCompression, &filter_block_handle);
  }

  // Write metaindex block
  if (ok())
  {
	  //保存meta-block的索引信息；
    BlockBuilder meta_index_block(&r->options);
    if (r->filter_block != NULL)
	{
      // Add mapping from "filter.Name" to location of filter data
      std::string key = "filter.";
      key.append(r->options.filter_policy->Name());
      std::string handle_encoding;
      filter_block_handle.EncodeTo(&handle_encoding);
      meta_index_block.Add(key, handle_encoding);
    }

    // TODO(postrelease): Add stats and other meta blocks
    WriteBlock(&meta_index_block, &metaindex_block_handle);
  }

  // Write index block
  if (ok()) 
  {
    if (r->pending_index_entry)
	{
	  //获取比last_key大的最小值
      r->options.comparator->FindShortSuccessor(&r->last_key);
      std::string handle_encoding;
      r->pending_handle.EncodeTo(&handle_encoding);
      r->index_block.Add(r->last_key, Slice(handle_encoding));
      r->pending_index_entry = false;
    }
    WriteBlock(&r->index_block, &index_block_handle);
  }

  // Write footer
  if (ok()) 
  {
    Footer footer;
    footer.set_metaindex_handle(metaindex_block_handle);
    footer.set_index_handle(index_block_handle);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    r->status = r->file->Append(footer_encoding);
    if (r->status.ok()) 
	{
      r->offset += footer_encoding.size();
    }
  }
  return r->status;
}

void TableBuilder::Abandon() 
{
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;
}

uint64_t TableBuilder::NumEntries() const
{
  return rep_->num_entries;
}

uint64_t TableBuilder::FileSize() const 
{
  return rep_->offset;
}

}  // namespace leveldb
