// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_DB_IMPL_H_
#define STORAGE_LEVELDB_DB_DB_IMPL_H_

#include <deque>
#include <set>
#include "dbformat.h"
#include "log_writer.h"
#include "snapshot.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "./port/port.h"
#include "port/thread_annotations.h"

namespace leveldb {

class MemTable;
class TableCache;
class Version;
class VersionEdit;
class VersionSet;

class DBImpl : public DB
{
 public:
  DBImpl(const Options& options, const std::string& dbname);
  virtual ~DBImpl();

  // Implementations of the DB interface
  virtual Status Put(const WriteOptions&, const Slice& key, const Slice& value);
  virtual Status Delete(const WriteOptions&, const Slice& key);
  virtual Status Write(const WriteOptions& options, WriteBatch* updates);
  virtual Status Get(const ReadOptions& options, const Slice& key, std::string* value);
  virtual Iterator* NewIterator(const ReadOptions&);
  virtual const Snapshot* GetSnapshot();
  virtual void ReleaseSnapshot(const Snapshot* snapshot);
  virtual bool GetProperty(const Slice& property, std::string* value);
  virtual void GetApproximateSizes(const Range* range, int n, uint64_t* sizes);
  virtual void CompactRange(const Slice* begin, const Slice* end);

  // Extra methods (for testing) that are not in the public DB interface

  // Compact any files in the named level that overlap [*begin,*end]
  void TEST_CompactRange(int level, const Slice* begin, const Slice* end);

  // Force current memtable contents to be compacted.
  Status TEST_CompactMemTable();

  // Return an internal iterator over the current state of the database.
  // The keys of this iterator are internal keys (see format.h).
  // The returned iterator should be deleted when no longer needed.
  Iterator* TEST_NewInternalIterator();

  // Return the maximum overlapping data (in bytes) at next level for any
  // file at a level >= 1.
  int64_t TEST_MaxNextLevelOverlappingBytes();

  // Record a sample of bytes read at the specified internal key.
  // Samples are taken approximately once every config::kReadBytesPeriod
  // bytes.
  void RecordReadSample(Slice key);

 private:
  friend class DB;
  struct CompactionState;
  struct Writer;

  Iterator* NewInternalIterator(const ReadOptions&,SequenceNumber* latest_snapshot, uint32_t* seed);
  Status NewDB();
  // Recover the descriptor from persistent storage.  May do a significant
  // amount of work to recover recently logged updates.  Any changes to
  // be made to the descriptor are added to *edit.
  Status Recover(VersionEdit* edit, bool* save_manifest) EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void MaybeIgnoreError(Status* s) const;
  // Delete any unneeded files and stale in-memory entries.
  void DeleteObsoleteFiles();
  // Compact the in-memory write buffer to disk.  Switches to a new
  // log-file/memtable and writes a new descriptor iff successful.
  // Errors are recorded in bg_error_.
  void CompactMemTable() EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Status RecoverLogFile(uint64_t log_number, bool last_log, bool* save_manifest, VersionEdit* edit, SequenceNumber* max_sequence) EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base) EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Status MakeRoomForWrite(bool force /* compact even if there is room? */) EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  WriteBatch* BuildBatchGroup(Writer** last_writer);
  void RecordBackgroundError(const Status& s);
  void MaybeScheduleCompaction() EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  static void BGWork(void* db);
  void BackgroundCall();
  void  BackgroundCompaction() EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void CleanupCompaction(CompactionState* compact) EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Status DoCompactionWork(CompactionState* compact) EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Status OpenCompactionOutputFile(CompactionState* compact);
  Status FinishCompactionOutputFile(CompactionState* compact, Iterator* input);
  Status InstallCompactionResults(CompactionState* compact) EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  
  /*
	系统文件,时间,进程相关处理类
  */
  Env* const env_;

  /*
    内部做key排序用到的比较方法
  */
  const InternalKeyComparator internal_comparator_;

  /*
	过滤策略类
  */
  const InternalFilterPolicy internal_filter_policy_;

  /*
	配置类 关于DB的相关配置
  */
  const Options options_;  // options_.comparator == &internal_comparator_

  /*
	是否自己提供了info_log, 一般都是false
  */
  bool owns_info_log_;

  /*
	是否用户提供了block_cache, 一般都是false
  */
  bool owns_cache_;

  /*
	DB数据库的名字
  */
  const std::string dbname_;

  // table_cache_ provides its own synchronization
  /*
	sstable table缓存用来同步
  */
  TableCache* table_cache_;

  // Lock over the persistent DB state.  Non-NULL iff successfully acquired.
  /*
	文件锁用来锁定lock文件, 保证仅能运行单个DB实例
  */
  FileLock* db_lock_;

  // State below is protected by mutex_
  port::Mutex mutex_;
  port::AtomicPointer shutting_down_;
  port::CondVar bg_cv_;          // Signalled when background work finishes
  /*
	可读可写内存
  */
  MemTable* mem_;
  /*
	只读内存
  */
  MemTable* imm_;                // Memtable being compacted
  port::AtomicPointer has_imm_;  // So bg thread can detect non-NULL imm_
  /*
	日志文件
  */
  WritableFile* logfile_;
  /*
	记录日志文件个数
  */
  uint64_t logfile_number_;
  log::Writer* log_;
  uint32_t seed_;                // For sampling.

  // Queue of writers.
  std::deque<Writer*> writers_;	//写队列
  WriteBatch* tmp_batch_;

  SnapshotList snapshots_;

  // Set of table files to protect from deletion because they are
  // part of ongoing compactions.
  /*
	待定table文件集合(这些文件部分正在被压缩, 保护该部分不被删除)
  */
  std::set<uint64_t> pending_outputs_;

  // Has a background compaction been scheduled or is running?
  bool bg_compaction_scheduled_;

  // Information for a manual compaction
  /*
	外部触发压缩的信息结构
  */
  struct ManualCompaction 
  {
    int level;	                //指定压缩的level
	bool done;					//为了避免外部指定的key - range过大, 一次compact过多的sstable文件,manual_compaction可能不会一次做完, 所以在manual_compaction结构中存在一个done标志来表示是否已经做完
    const InternalKey* begin;   //NULL means beginning of key range
    const InternalKey* end;     //NULL means end of key range
    InternalKey tmp_storage;    //tmp_storage保存上次compact的end_Key, 也就是下一次的start_key;
  };

  ManualCompaction* manual_compaction_;

  /*
	整个DB状态的管理者, 包括当前最新的Version,正在服务的Version链表, 全局的SequenceNumber，FileNumber, 当前的manifest_file_number, 
	封装的sstable的TableCache, 还有每个level中下一次compact要选取的start_key等等
  */
  VersionSet* versions_;

  // Have we encountered a background error in paranoid mode?
  Status bg_error_;

  // Per level compaction stats.  stats_[level] stores the stats for
  // compactions that produced data for the specified "level".
  struct CompactionStats
  {
    int64_t micros;
    int64_t bytes_read;
    int64_t bytes_written;

    CompactionStats() : micros(0), bytes_read(0), bytes_written(0) { }

    void Add(const CompactionStats& c)
    {
      this->micros += c.micros;
      this->bytes_read += c.bytes_read;
      this->bytes_written += c.bytes_written;
    }
  };
  CompactionStats stats_[config::kNumLevels];

  // No copying allowed
  DBImpl(const DBImpl&);
  void operator=(const DBImpl&);

  const Comparator* user_comparator() const 
  {
    return internal_comparator_.user_comparator();
  }
};

// Sanitize db options.  The caller should delete result.info_log if
// it is not equal to src.info_log.
/*
	调整用户传入的Option使其合法
*/
extern Options SanitizeOptions(const std::string& db,const InternalKeyComparator* icmp,const InternalFilterPolicy* ipolicy, const Options& src);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_DB_IMPL_H_
