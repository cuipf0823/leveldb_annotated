// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_H_

#include <stddef.h>
#include <stdint.h>
#include <iostream>
#include "leveldb/iterator.h"

namespace leveldb {

struct BlockContents;
class Comparator;
/*
	sstable的数据由一个个的block组成，当持久化时候，多份kv聚合成block一次写入，
	当读取时，也是以block单位做IO，sstable的索引信息中会保存符合key-range的block
	在文件中的offset/set

	sstable中的数据结构以block单位存储，整体如下图：
	| entry0 | entry1 | ... | restarts				 | num_of_restarts | trailer |
	|    	 |		  |	    |uin32*(num_of_restarts) |	uint32		   |		 |

	注：restart 每一轮key压缩的block offset保存在restarts中
		num_of_restarts 保存着总压缩的轮数

	entry的组成：
	shared-bytes  unshared-bytes  value-bytes  unshared-key-data  value-data
	varint        varint          varint       unshared_bytes     value_bytes

	trailer的组成：
	type(char) + crc(uint32)
																				
*/
class Block 
{
 public:
  // Initialize the block with the specified contents.
  explicit Block(const BlockContents& contents);

  ~Block();

  size_t size() const { return size_; }
  Iterator* NewIterator(const Comparator* comparator);

 private:
  /*
	从data中读取num_of_restarts实际的值
  */
  uint32_t NumRestarts() const;

  const char* data_;
  size_t size_;
  /*
	restart数组偏移量
  */
  uint32_t restart_offset_;     // Offset in data_ of restart array
  /*
	owned == True 需要调用者主动删除data_
  */
  bool owned_;                  // Block owns data_[]

  // No copying allowed
  Block(const Block&);
  void operator=(const Block&);

  class Iter;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_H_
