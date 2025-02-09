#ifndef MERT_H
#define MERT_H

#include <variant>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <string>
#include <functional>
#include <cstdint>
#include <optional>

// key的类型只能是string！键的类型也只能是string，给我输入都换成string，草！
// 我的代码我做主！

// =========================
// 1. 配置结构体：MERTConfig
// =========================
struct MERTConfig
{
    // int span = 16;           // 一次处理多少位，比如 16 bits
    int bucket_capacity = 16; // 每个桶最多存多少键值对
    int segment_size = 256;   // 每个段最多多少个桶
    int back_num = 8;         // 最多桶的数量为2^back_num=256
    int global_depth = 4;     // 最多有2^4=16个段
};

// =============================
// 2. MERTNode 声明
// =============================
class MERTNode
{
public:
    // -------------------------
    // 2.1 桶结构声明
    // -------------------------
    struct Bucket
    {
        // bucket里面可以存key-value或者指针
        using EntryType = std::variant<std::pair<std::string, std::string>, std::shared_ptr<MERTNode>>;
        std::vector<std::optional<EntryType>> entries;
    };

    // -------------------------
    // 2.2 段结构声明
    // -------------------------
    struct Segment
    {
        std::vector<Bucket> buckets;
        uint8_t local_depth = 0;
     //   mutable std::shared_mutex seg_lock;

        // 段构造函数
        Segment();
    };
    // 每一个前缀字节都有属于自己的目录，查询时用最长前缀匹配
    struct PrefixDirectory
    {
        char c{0};
       // mutable std::shared_mutex prefix_lock;
        std::vector<std::shared_ptr<Segment>> segments;
        int prefix_index; // 用于标记是第几个前缀,从0开始
    };

    // -------------------------
    // 2.3 节点头部信息
    // -------------------------
    struct Header
    {
        // std::atomic<int> depth{0}; // 节点层级深度(好像没啥用啊，debug时候用吧)
        // bool is_full{false};       // 这个是判断prefix是否已满，未满的话，符合前缀且比前缀长的话就会填入后续的prefix
        //  uint8_t prefix_length{0};  // 路径压缩用的前缀长度
        PrefixDirectory prefix[6];
    };

public:
    // -------------------------
    // 2.4 构造函数 & 接口声明
    // -------------------------
    MERTNode();
public:
    // -------------------------
    // 2.5 工具函数声明
    // -------------------------
    // 这里的key是完整的key，start为prefix后的第一个字节，提取该字节的后四位的前local_depth位
    uint8_t extract_subkey_segment(const std::string &key, int local_depth, int start);
    // 提取尾部的固定位，进入桶时需要
    uint8_t extract_subkey_bucket(const std::string &key, int num);
    // 段分裂，要指定是哪个前缀下的目录分裂，此时段分裂是还<=global_depth的情况
    void split_segment(size_t segment_index, PrefixDirectory &directory, const uint16_t &global_depth);
    // 二进制字符串转成十进制
    int binary_to_decimal(const std::string &binary_str);
    // 计算分裂后新的段索引，得到的是两个索引数组
    void generate_new_segment_index(int binaryNumber, int local_depth, std::vector<int> &resultZero, std::vector<int> &resultOne);
    // 添加子节点，进入下一层
    void add_child_node(MERTNode *new_node, Bucket &bucket, int start_pos);
    // 以下两个函数是查询字符串数组的从start_pos开始的两两之间最长的公共子串
    std::string longestCommonSubstringBetweenTwo(const std::string &s1, const std::string &s2, int start_pos);
    std::string longestCommonSubstringAmongTwo(const std::vector<std::string> &strs, int start_pos);
    // 生成新节点时将原来桶里的key-value插入到新的节点中，因为不知道和上面的insert是否有区别，所以先这么写
    void insert_to_new_node(MERTNode *new_node, const std::string &key, const std::string &value, int start_pos, bool &not_this_node);
    // 插入到段桶中
    void insert_to_segment_bucket(MERTNode *new_node, const std::string &key, const std::string &value, int start_pos, int directory_index);

private:
    // -------------------------
    // 2.6 成员变量
    // -------------------------
    Header header;
    // 注意这个是完全匹配，如果是前缀完全匹配的话，但是完整的键不是完全匹配的话就要进入桶
    std::vector<std::optional<std::string>> total_value; // 当键完全匹配时存储的键，下标即为匹配的键数量的数字
    // 节点锁（保护本节点 directory、header 以及子结构操作）
    //  mutable std::shared_mutex node_lock_;
};


/***
 * MERTNode的根节点，
 * 只有bucket
 *
 */
class MERTRootNode
{
public:
    struct RootBucket
    {
        // bucket里面可以存key-value或者指针
        using EntryType = std::shared_ptr<MERTNode>;
        std::optional<EntryType> node_entry; // root的bucket只存放一个entry
        // mutable std::shared_mutex bucket_lock;
    };

private:
    std::vector<RootBucket> root_bucket;

public:
   // uint8_t cal_SegmentIndex(const std::string &key);
    uint8_t cal_BucketIndex(const std::string &key);
    void insert(const std::string &key, const std::string &value);
    MERTRootNode();
};
// =============================
// 3. MERT 整体类声明
// =============================
class MERT
{
public:
    // 构造函数
    MERT();

    // 插入
    void insert(const std::string &key, const std::string &value);

    // 查找（返回是否找到，并输出到 value）
    std::string search(const std::string &key);

private:
    MERTRootNode root_;

    // 树锁
    //mutable std::shared_mutex tree_lock_;
};

#endif // MERT_H
