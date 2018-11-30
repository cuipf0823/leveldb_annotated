# 概述
leveldb是能够处理十亿级别的key-value型的数据持久存储的C++程序库；

## 特点
* leveldb在存储数据时候，是根据key值进行有序存储，key的大小比较函数可以由应用程序自定义；
* leveldb像大多数key-value系统一样，接口比较简单，基本的操作包含写记录、读记录、记录删除，以及针对多条操作的原子批量处理；
* leveldb也支持数据的快照功能，使得读取操作不受到写操作的影响，可以在读操作中始终看到一致的数据；
* leveldb也支持的数据的压缩，对于减小存储空间以及增快IO效率都有直接的帮助；

## 性能
官方声称随机写入速度可以高达40w/s，随机读取的速度6w/s；顺序读写操作的速度明显高于随机读写的数据的速度；

## 文件
leveldb在使用过程中会产生多种类型的文件, 如下:
* log文件 [0-9].log
> log文件在leveldb中主要是系统故障恢复时候，能够保证不丢失数据。记录在写入内存memtable之前，会写入log文件，这样即使系统发生故障，memtable中的数据没有dump到sstable文件中， leveldb也可以根据log文件恢复内存的memtable数据结构内容，不会造成数据丢失。

* lock文件 LOCK
> 一个db只能有一个db实例操作，通过对lock文件加文件锁实现主动保护。

* sstable文件 [0 - 9].sst
> 保存数据的sstable文件 前面为FileNumber；

* MANIFEST文件（DescriptorFile）
> manifest文件为了重启db后可以恢复退出前的状态，需要将db的状态保存下来，这些状态信息就保存在该文件中。每当db中的状态改变（VersionSet），会将这次的改变（VersionEdit）追加到descriptor文件中。

* CURRENT文件
> current文件记录当前manifest文件的文件名，指出那个manifest才是我们关心的manifest；

* 临时文件 [0 - 9].dbtmp
> 对db进行修复的时候，会产生临时文件，前缀为FileNumber

* db运行时打印的日志文件：LOG
> db运行的时候，打印的日志文件保存在log中，每次重新运行，如果已经存在log文件，会把LOG文件重新命名为LOG.old

# 结构
在了解leveldb数据结构前要先理解几个基本的结构和概念：
## 基础结构和概念
### ValueType  
leveldb更新（put/delete）某个key时不会直接修改db中的数据，每一次的操作都是直接新插入一份kv数据，具体的合并和清除有后台compact完成；每次put都会插入一份kv数据，即使key已经存在；ValueType正是为了区分真实的kv数据和删除操作的mock数据;
```c++
enum ValueType 
{
    kTypeDeletion = 0x0,
    kTypeValue = 0x1
};
```

### SequenceNumber
 leveldb的每次操作（put/delete）都会有一个操作序列号，全局唯一；key的排序，compact以及leveldb的快照都会依据此序列号；该值其实就是一个uint64_t; 结构如下：

![SequenceNumber结构图](http://cuipf0823.github.io/images/SequenceNum.png)
* 存储的时候一个SequenceNumber占64位(一个uint64_t), SequenceNumber只占用56bits，ValueType占用8bits；  
### UserKey
用户层面传入的key，使用Slice格式；

### ParsedInternalKey
leveldb内部使用key，在用户传入Userkey的基础上一些信息；
    
```c++
struct ParsedInternalKey
{
    Slice user_key;
    SequenceNumber sequence;
     ValueType type; 
};
```
### InternalKey
leveldb内部使用， 是一个class，为了易用；结构和ParsedInternalKey一样，也是UserKey加上SequenceNumber和ValueType；

### LookupKey
db内部为了查找memtable\sstable方便 包装使用的key结构，保存有userkey和SequenceNumber和ValueType
以及dump在内存的数据；结构如下：

![LookupKey结构图](http://cuipf0823.github.io/images/Lookupkey.png)
* memtable中进行lookup时使用的是[start, end]，对sstable文件 lookup时候使用的是[kstart, end];

### Comparator
Comparator类是leveldb中对key排序的时候使用的比较方法，leveldb中的key是升序；用户也可以自定义user key的comparator，作为option传入，默认采用bytes-compare(memcmp)；Comparator是一个抽象类；
comparator中两个重要成员函数：
* FindShortestSeparator：获取大于start但是小于limit的最小值；
* FindShortSuccessor：获取比start大的最小值；

### InternalKeyComparator
InternalKeyComparator继承于Comparator；db内部做key排序的时候使用，排序时，先使用usercomparator比较user-key，如果user-key相同的时候比较SequenceNumber，SequenceNumber大的为小，因为SequenceNumber在leveldb中是递增的。对于相同的user-key，最新更新的排在前面（SequenceNumber比较大），在查找的时候会被先找到。

## memtable
### 结构
leveldb数据在内存中存储格式；用户写入的数据首先被记录在内存中memtable中，当memtable达到阈值（write_buffer_size = 4MB）时候，会转化为自读的immutable memtable同时会再次生成一个新的memtable；后台有压缩线程会把immutable memtable dump成sstable；
内存中同时最多有一个memtable和immutable memtable；memtable和immutable memtable内存结构完全一样如下：

![memtable结构示意图](http://cuipf0823.github.io/images/memtable.png)

说明：
* memtable基本数据模型是skiplist；
* 注意结构中的key为InternalKey;
* 代码主要通过两个接口实现：Memtable::Add和Memtable::Get；
* memtable中内存申请使用的自身封装的arena；memtable有阈值的限制（write_buffer_size），为了便于统计内存的使用，以及内存的使用效率，arena每次按照kBlockSize（4096）单位向系统申请内存，提供地址对齐的内存，记录内存使用。当memtable申请内存时候，size不大于kBlockSize的四分之一，就在当前空闲的内存中分配，否则直接向系统malloc，这样可以有效的服务小内存的申请，避免个别大内存使用影响。

### memtable相关操作
* Memtable::Add 写入
    1. 将传入的key和value dump成memtable中存储的格式；
    2. 调用SkipList：：Insert插入到table_;
* Memtable::Get 读取
    1. 从传入的LookupKey中取得memtable中存储的key格式；
    2. 做MemtableIterator：：seek（）
    3. seek 失败，返回data not exist。 seek成功，判断ValueType:1. kTypeValue返回value的值； 2.kTypeDeletion，返回data not exist；

## sstable
### 结构
 sstable是leveldb中持久化数据的文件格式，整体上可以看出sstable是由数据（data）和元信息（meta/index）组成，数据和元信息统一以block为单位存储（除了文件末尾的footer元信息），读取时也采用统一的读取逻辑。结构示意图如下：
![sstable结构图](http://cuipf0823.github.io/images/sstable.png)

footer结构示意图：

![footer结构图](http://cuipf0823.github.io/images/footer.png)

**说明：**
1. data_block实际上存储的是key-value的数据；
2. 每一个data-block对应一个meta-block，保存data-block中的key-size、value-size、kv-counts之类的统计信息，当前的版本未实现；
3. metaindex-block：保存meta-block的索引信息，当前版本未实现；
4. index_block：保存每一个data-block的last-key及其在sstable文件中的索引；index_block中每个entry的shared_bytes都为0，unshared_key_data即为data_block的last_key，后面的value_data保存的为索引即使BlockHandle（offset/size）;
5. footer：文件末尾的固定长度的数据，保存着metaindex-block和index-block的索引信息，为了达到固定的长度添加了padding_bytes，当然最后包括8个bytes的magic的校验。目前版本padding_bytes = 0。


### block of sstable
sstable中的数据是以block单位存储的，有利于IO和解析的粒度。sstable中block的相关结构示意图如下：

![block结构图](http://cuipf0823.github.io/images/block.png)

entry的组成：

![block_entry结构图](http://cuipf0823.github.io/images/block_entry.png)

trailer的组成：

![block_trailer结构图](http://cuipf0823.github.io/images/block_trailer.png)

**说明：**
* entry：一份key-value数据作为block内的一个entry；leveldb对key的存储进行了前缀压缩即key存储压缩，每一个key记录与上一个key前缀相同的字节（shared_bytes）以及自己独有字节部分（unshared_bytes）。读取时候，对block进行遍历，每一个key根据前一个key以及shared_bytes/unshared_bytes可以构造出来。

* restarts：block内部的key压缩是分区段进行的，若干个（option::block_restart_interval）key做一次前缀压缩，之后重新开始下一轮，每一轮前缀压缩的block offset保存在restarts中。
* num_of_restarts：记录着总共压缩的论数。
* 每一个block的后面都会有一个5个字节的trailer，1个字节的type表示block内的数据是否进行了压缩（例如使用了snappy压缩），4个字节crc校验码，type如下:

    ```c++
    enum CompressionType
    {
        kNoCompression     = 0x0,
        kSnappyCompression = 0x1
    };
    ```

### sstable相关操作
#### 写入
代码主要由 *TableBuilder::Add()* 和 *TableBuilder::Finish()* 两部分完成;

**TableBuilder::Add()**

1. 如果是一个新block的开始，计算出上一个block的end-key（FinderShortestSeparator），连同BlockHandle添加到IndexBlock中；考虑到indexblock会load到内存，为了减少内存占用, 每一个Indexblock只保存每个data_block的end-key、offset、size；
2. 将key、value加入当前data_block(BlockBuilder::Add())。
3. 如果当前data_block达到设定的Option::block_size（4K），将data_block写入磁盘（TableBuilder::Flush-->BlockBuilder::WriteBlock）；
4. 调用BlockBuilder::Finish，在block末尾添加restarts数据段和num_of_restarts;
5. 对block的数据进行压缩，然后append到sstable文件中(TableBuilder::WriteBlock--->WriteRawBlock)；
6. 添加该block的trailer（type/crc），append到sstable文件中；
7. TableBuilder::Flush中调用file fflush保证block写到磁盘文件中；

**TableBuilder::Finish()**
1. 将filter block写入到磁盘； 
2. 将metaindex block写入到磁盘(当前代码未实现此部分, 所以meta_index_block为空)；
3. 计算出最后一个block的end-key，连同其他的BlockHandle添加到Index_block中，将index block写入到磁盘；
4. 构造footer，将footer写入到磁盘；

#### 读取
读取首先是FindTable, 然后使用接口table::Open(), 将数据从sstable文件(ldb，sst文件)中加载到Table对象; 调用成功会返回table对象; 

**TableCache**
* tablecache采用的是LRUCache缓存方法, 这样热的数据都保存在内存; 用于加速block的定位; 
* Key: sstable的FileNumber;
* Value: 封装了元信息的Table指针和sstable对应的Table文件指针;

**BlockCache**
* TableCache类中Options成员, 包含一个全局的BlockCache(NewLRUCache(8 << 20)), 同样是LRUCache算法, 用于保存最近使用的BlockCache;
* Key: 全局的CacheId 和 Block在sstable文件中的偏移量合成;
* Value: Block对象的指针;

**TableCache::FindTable**
1. 根据FileNumber合成的Key在TableCache中查找是否存在;
2. 如果TableCache中未查找到, 从后缀为 *.ldb* 的tablefile文件中查找;
3. 如果还未查找到, 从后缀为 *.sst* 的文件中查找;
4. 查找到之后, 通过Table::open打开Table文件, 获取Table对象;
5. 插入TableCache中;

**Table::Open()**
1. 根据传入的sstable size，首先读取文件末尾的footer（保存着metaindex-block和index-block的索引信息）；
2. 解析footer数据，校验magic，获得index_block 和 metaindex_block的blockhandle；
3. 根据index_block的BlockHandle，读取Index_block(保存每一个data-block的last-key及其在sstable文件中的索引);
4. 分配cacheID(加入TableCache时候, 需要一个全局的CacheID)；
5. 封装成Table；

**Table::InternalGet()**
1. 根据封装的Table对象中的IndexBlock, 在IndexBlock中的restarts中做二分查找; 找到key所属于的前缀压缩区间的偏移量;
2. 根据偏移量, 点位到block的entry; 然后依次遍历(ParseNextKey)找到相应的key的BlockHandle;
3. 根据Table中CacheID和BlockHandle中offset合成的key(BlockCache使用也是LRUCache缓存技术, 缓存中Key由CacheID和offset合成)查找BlockCache是否存在;
4. 如果在BlockCache缓存中没有找到, 直接Table对象对应的sstable文件中读取DataBlock(根据BlockHandle中的offset和size); 读取之后当然要插入BlockCache中, 因为这是最近使用的DataBlock; 但如果没有使用BlockCache缓存,就不需要插入了;
5. 读取到DataBlock, 直接查找用户传入的Key即可; 
6. 调用回调;


## log
### log作用
leveldb能够在系统故障恢复时，能够保证不会丢失数据。因为在数据写入内存的memtable之前，会先把相关的操作写入log文件，这样即使发生了故障，Memtable中的数据没有来得及Dump到磁盘的SStable文件，leveldb也可以根据log文件恢复内存的Memtable数据结构的内容，不会造成数据的丢失。

### log文件的物理结构
基本组成单元：32k大小的Block结构；每一次的读取是以一个block作为基本读取单位。写日志的时候，考虑到一致性，并没有按block为单位写，每一次的更新都会对log文件进行IO;

log文件的结构示意图：

![log结构图](http://cuipf0823.github.io/images/log_structure.png)

说明：
1. init_data：log文件开头添加的一些信息；读取和写入的时候会跳过这些数据；
2. block：实际的数据。
3. log和Manifest文件都使用了这种格式。

block of log结构示意图：

![block of log结构图](http://cuipf0823.github.io/images/log_block.png)

record组成示意图：

![Record结构图](http://cuipf0823.github.io/images/log_record.png)


说明：
1. 每一个更新写入作为一个record；
2. checksum记录的是type和data的crc校验；
3. 为了避免block内部碎片的产生，一份record可能会跨block，所以根据record中数据占更新写入数据的完整是否，type字段分为四种： kFullType， kFirstType， kMiddleType， kLastType；
4. data即为保存的数据，长度为length；
5. trailer：如果block最后剩余的部分不足record的头部长度（checksum、length、type共7bytes），直接填0，最为block的trailer。

### log相关操作
log的写入是顺序的写入，读取则只会在启动的时候发生，不会是性能的瓶颈，log中的数据没有经过任何的压缩。

**读取**
1. log的读取仅仅发生在db启动的时候，每次读取出来的为当时一次写入内容；
2. 相关读取log文件的函数是ReadRecord和ReadPhysicalRecord；
3. SkipToInitialBlock用于跳过log文件开始的init_data部分；

**写入**
1. 对log的写入作为record添加；
2. 如果当前的block剩余的size小于record的头部长度，填充trailer，开始下一个block；
3. 根据option指定的sync决定是否做log文件的强制sync；
