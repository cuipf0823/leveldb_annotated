// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/two_level_iterator.h"

#include "leveldb/table.h"
#include "table/block.h"
#include "table/format.h"
#include "table/iterator_wrapper.h"

namespace leveldb {

namespace {

typedef Iterator* (*BlockFunction)(void*, const ReadOptions&, const Slice&);

class TwoLevelIterator: public Iterator 
{
 public:
  TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg, const ReadOptions& options);

  virtual ~TwoLevelIterator();

  virtual void Seek(const Slice& target);
  virtual void SeekToFirst();
  virtual void SeekToLast();
  virtual void Next();
  virtual void Prev();

  virtual bool Valid() const
  {
    return data_iter_.Valid();
  }
  virtual Slice key() const 
  {
    assert(Valid());
    return data_iter_.key();
  }
  virtual Slice value() const 
  {
    assert(Valid());
    return data_iter_.value();
  }
  virtual Status status() const 
  {
    // It'd be nice if status() returned a const Status& instead of a Status
    if (!index_iter_.status().ok()) 
	{
      return index_iter_.status();
    } 
	else if (data_iter_.iter() != NULL && !data_iter_.status().ok()) 
	{
      return data_iter_.status();
    } 
	else 
	{
      return status_;
    }
  }

 private:
  void SaveError(const Status& s) 
  {
    if (status_.ok() && !s.ok()) status_ = s;
  }
  void SkipEmptyDataBlocksForward();
  void SkipEmptyDataBlocksBackward();
  void SetDataIterator(Iterator* data_iter);
  void InitDataBlock();

  /*
	根据IndexBlock的Value值(BlockData对应的Index信息), 回调返回DataBlock的回调函数
  */
  BlockFunction block_function_;
  
  /*
	回调函数的参数
  */
  void* arg_;
  const ReadOptions options_;

  /*
	记录操作过程中的状态
  */
  Status status_;

  /*
	IndexBlock的Iterator, 可以根据key Seek到Key在DataBlock中的位置
  */
  IteratorWrapper index_iter_;
  
  /*
	DataBlock的Iterator 根据Key可以Seek到Key在DataBlock的位置, 进而获得对应的value;
  */
  IteratorWrapper data_iter_; // May be NULL
  
  // If data_iter_ is non-NULL, then "data_block_handle_" holds the
  // "index_value" passed to block_function_ to create the data_iter_.
  /*
	保存Index_value(data_block的索引信息)
  */
  std::string data_block_handle_;
};

TwoLevelIterator::TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg, const ReadOptions& options)
    : block_function_(block_function),
      arg_(arg),
      options_(options),
      index_iter_(index_iter),
      data_iter_(NULL) 
{

}

TwoLevelIterator::~TwoLevelIterator() 
{
}


void TwoLevelIterator::Seek(const Slice& target) 
{
  //获得index_iter_->value(), 即key所在BlockData的索引信息;
  index_iter_.Seek(target);
  
  //根据IndexBlock的handle信息, 调用回调函数, 获取data_iter;
  InitDataBlock();

  //根据data_iter, 定位找到key;
  if (data_iter_.iter() != NULL) data_iter_.Seek(target);
  
  //如果data_iter是无效的, 需要不断尝试下一个BlockData并定位到其开始的地方, 知道找到合法的BlockData
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToFirst() 
{
  index_iter_.SeekToFirst();
  InitDataBlock();
  if (data_iter_.iter() != NULL) data_iter_.SeekToFirst();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToLast() 
{
  index_iter_.SeekToLast();
  InitDataBlock();
  if (data_iter_.iter() != NULL) data_iter_.SeekToLast();
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::Next() 
{
  assert(Valid());
  data_iter_.Next();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::Prev() 
{
  assert(Valid());
  data_iter_.Prev();
  SkipEmptyDataBlocksBackward();
}


void TwoLevelIterator::SkipEmptyDataBlocksForward() 
{
  while (data_iter_.iter() == NULL || !data_iter_.Valid()) 
  {
    // Move to next block
    if (!index_iter_.Valid())
	{
      SetDataIterator(NULL);
      return;
    }
    index_iter_.Next();
    InitDataBlock();
    if (data_iter_.iter() != NULL) data_iter_.SeekToFirst();
  }
}

void TwoLevelIterator::SkipEmptyDataBlocksBackward() 
{
  while (data_iter_.iter() == NULL || !data_iter_.Valid()) 
  {
    // Move to next block
    if (!index_iter_.Valid()) 
	{
      SetDataIterator(NULL);
      return;
    }
    index_iter_.Prev();
    InitDataBlock();
    if (data_iter_.iter() != NULL) data_iter_.SeekToLast();
  }
}

void TwoLevelIterator::SetDataIterator(Iterator* data_iter)
{
  if (data_iter_.iter() != NULL) SaveError(data_iter_.status());
  data_iter_.Set(data_iter);
}

void TwoLevelIterator::InitDataBlock() 
{
  if (!index_iter_.Valid()) 
  {
    SetDataIterator(NULL);
  } 
  else
  {
    Slice handle = index_iter_.value();
    if (data_iter_.iter() != NULL && handle.compare(data_block_handle_) == 0) {
      // data_iter_ is already constructed with this iterator, so
      // no need to change anything
    } 
	else 
	{
      Iterator* iter = (*block_function_)(arg_, options_, handle);
      data_block_handle_.assign(handle.data(), handle.size());
      SetDataIterator(iter);
    }
  }
}

}  // namespace

Iterator* NewTwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg, const ReadOptions& options)
{
  return new TwoLevelIterator(index_iter, block_function, arg, options);
}

}  // namespace leveldb
