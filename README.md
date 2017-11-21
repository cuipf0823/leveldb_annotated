# 概述
leveldb是能够处理十亿级别的key-value型的数据持久存储的C++程序库；

## 特点
* leveldb在存储数据时候，是根据key值进行有序存储，key的大小比较函数可以由应用程序自定义；
* leveldb像大多数key-value系统一样，接口比较简单，基本的操作包含写记录、读记录、记录删除，以及针对多条操作的原子批量处理；
* leveldb也支持数据的快照功能，使得读取操作不受到写操作的影响，可以在读操作中始终看到一致的数据；
* leveldb也支持的数据的压缩，对于减小存储空间以及增快IO效率都有直接的帮助；

## 性能
官方声称随机写入速度可以高达40w/s，随机读取的速度6w/s；顺序读写操作的速度明显高于随机读写的数据的速度；leveldb的写入数据速度要大大快于读速度；

## 文件
leveldb在使用过程中会产生多种类型的文件, 如下:
* log文件 [0-9].log
>> log文件在leveldb中主要是系统故障恢复时候，能够保证不丢失数据。记录在写入内存memtable之前，会写入log文件，这样即使系统发生故障，memtable中的数据没有dump到sstable文件中， leveldb也可以根据log文件恢复内存的memtable数据结构内容，不会造成数据丢失。
leveldb的写流程是先记binlog，然后写sstable，该日志文件即是binlog。

* lock文件 LOCK
>> 一个db只能有一个db实例操作，通过对lock文件加文件锁实现主动保护。

* sstable文件 [0 - 9].sst
>> 保存数据的sstable文件 前面为FileNumber；

* MANIFEST文件（DescriptorFile）
>> manifest文件 为了重启db后可以恢复退出前的状态，需要将db的状态保存下来，这些状态信息就保存在该文件中。
每当db中的状态改变（VersionSet），会将这次的改变（VersionEdit）追加到descriptor文件中。

* CURRENT文件
>> current文件记录当前manifest文件的文件名，指出那个manifest才是我们关心的manifest；

* 临时文件 [0 - 9].dbtmp
>> 对db进行修复的时候，会产生临时文件，前缀为FileNumber

* db运行时打印的日志文件：LOG
>> db运行的时候，打印的info日志文件保存在log中，每次重新运行，如果已经存在log文件，会把LOG文件重新命名为LOG.old
