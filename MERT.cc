#include "MERT.hh"
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <functional>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <variant>

uint8_t MERTNode::extract_subkey_segment(const std::string &key, int local_depth, int start)
{
    using ReturnType = uint8_t;
    // key只能是string类型
    // 若字符串为空或 local_depth=0，直接返回 0
    if (key.empty() || local_depth == 0 || start >= key.size())
    {
        return static_cast<ReturnType>(0);
    }
    // 取第start字符的最高位的 local_depth bits
    uint8_t byte = static_cast<uint8_t>(key[start]); // 先取第start个字符
    uint8_t lower_nibble = byte & 0x0F;              // 取后四位
    uint8_t topBits = (lower_nibble >> (4 - local_depth)) & ((1 << local_depth) - 1);
    return static_cast<ReturnType>(topBits);
}

uint8_t MERTNode::extract_subkey_bucket(const std::string &key, int num)
{
    using ReturnType = uint8_t;
    if (key.empty() || num == 0)
    {
        return static_cast<ReturnType>(0);
    }
    uint8_t c = static_cast<uint8_t>(key[0]); // 先取第一个字符
    uint8_t mask = (1 << num) - 1;
    uint8_t result = c & mask;
    return static_cast<ReturnType>(result);
}

/***
 * 接下来要考虑一些变换节点的情况了
 * 首先键进入节点，查看该节点是否为空节点，
 * 如果它不为空节点(有一个bool值) 但是prefix_length为0的，
 * 说明该节点存的都是节点指针
 * 注意：如果一个如果bucket里存的是指针的话，
 * 下一个节点的前缀是从上一个的前缀后的字节算起
 * 而不是从上一个前缀+local_depth的位算起
 * 如果bucket存的是key-value那问题不大
 * 因为输入的key为uint64_t时是以十进制输入的，所以都会存入到索引为0000的段里
 * 造成段倾斜，把uint64_t转成string，再处理，但是因为转成string，它的ascii基本也是连续的，
 * 所以段的话就按照prefix后的第一个字节的后四位进行段的索引，然后prefix后的第一个字节的前四位作为进入桶后的排序索引
 *
 * 对于根节点，当在根节点中时，如果遇到了因为前缀完全不匹配而要添加新的节点的情况，这种情况只在根节点会出现
 * 因为当普通节点递增(指深度加深)
 *
 *
 ***/

// 段分裂是一个段下的某个桶的键值对数量到达阈值且local_depth<global_Depth
// 然后要把桶里的数据给分散开来
// 在一个段下的桶们，prefix后的第一个字节的后四位的前local_depth位相等
// 而在一个段下的一个桶里的数据，除了上面的相等，它们的后8位也是相等的，桶索引和后八位有关
// 数据在桶里索引和prefix后的第一个字节的前四位有关
// 进行段分裂，首先获取对应的prefix下的锁，segment_index为要分裂的段的索引
void MERTNode::split_segment(size_t segment_index, PrefixDirectory &directory, const uint16_t &global_depth)
{
    // 因为段分裂是在insert情境下才会发生，所以这里只需要获得新段的锁即可
    //  首先进行目录上锁
    // std::unique_lock<std::shared_mutex> dir_lock(directory.prefix_lock);

    // 获取要分裂的段
    std::shared_ptr<Segment> old_segment = directory.segments[segment_index];

    if (!old_segment)
    {
        return;
    }
    // 对原段上写锁，上锁的顺序是从上至下的：节点->目录->段->桶，这里对该前缀下的目录上锁即可，不影响其他目录
    // std::unique_lock<std::shared_mutex> seg_lock(old_segment->seg_lock);

    if (old_segment->local_depth >= global_depth)
    {
        return;
        // 当local_depth大于等于global_depth时(按理应该是只会等于)就会进行深度++，bucket放节点指针
    }
    const uint8_t old_local_depth = old_segment->local_depth;

    // 创建两个新的段，local_depth+1
    std::shared_ptr<MERTNode::Segment> new_segment0 = std::make_shared<MERTNode::Segment>();
    std::shared_ptr<MERTNode::Segment> new_segment1 = std::make_shared<MERTNode::Segment>();

    // 获取这两个段的锁
    // std::unique_lock<std::shared_mutex> seg_lock0(new_segment0->seg_lock);
    // std::unique_lock<std::shared_mutex> seg_lock1(new_segment1->seg_lock);

    new_segment0->local_depth = old_local_depth + 1;
    new_segment1->local_depth = old_local_depth + 1;

    int prefix_index_ = directory.prefix_index; // 从这个prefix后的第一个字节开始

    // 此为该段的“实际下标”
    uint8_t old_segment_index;

    for (auto &old_bucket : old_segment->buckets)
    {
        // 这里要获取每个桶的锁
        // std::unique_lock<std::shared_mutex> bucket_lock_(old_bucket.bucket_lock);
        for (auto it = old_bucket.entries.begin(); it != old_bucket.entries.end(); it++)
        {
            // 因为桶有两种数据类型，所以先判断一下是键值对还是指针
            // 如果桶里存放的是指针的话，先去查看该指针的第0个前缀字节，再根据该字节，再去重新分配到别的段里
            if (*it)
            {
                auto &entry = it->value();
                if (std::holds_alternative<std::pair<std::string, std::string>>(entry))
                {
                    // 找到当前prefix的字节，段是从prefix后的第一个字节开始
                    // 段索引是该字节的后四位
                    // 这里的bucket是直接取下标的，而不是重新计算
                    // 如果是键值对的话，获取键，该键是完整的键值对
                    std::string key = std::get<std::pair<std::string, std::string>>(entry).first;
                    // 这个是获取新的segment的index
                    uint8_t new_segment_index = extract_subkey_segment(key, old_local_depth + 1, prefix_index_ + 1);
                    old_segment_index = extract_subkey_segment(key, old_local_depth, prefix_index_ + 1); // 这里的old_segment_index按理来说是和下面的是一样的
                    std::size_t new_bucket_index = std::distance(old_bucket.entries.begin(), it);        // bucket的索引是不会变的
                    if (new_segment_index == old_segment_index * 2)
                    {
                        new_segment0->buckets[new_bucket_index].entries.push_back(entry);
                    }
                    else if (new_segment_index == old_segment_index * 2 + 1)
                    {
                        new_segment1->buckets[new_bucket_index].entries.push_back(entry);
                    }
                }
                else if (std::holds_alternative<std::shared_ptr<MERTNode>>(entry))
                {
                    // 如果是指针的话，获取指针
                    // 要获取一下独占锁，因为要更改node所在的位置
                    std::shared_ptr<MERTNode> node = std::get<std::shared_ptr<MERTNode>>(entry);
                    // std::unique_lock<std::shared_mutex> node_lock(node->node_lock_); // 给node上个写锁
                    // 获取指针的第一个前缀
                    MERTNode::PrefixDirectory &prefixDir = node->header.prefix[0];
                    char firstPrefixByte = prefixDir.c;
                    // 获取字节后查看新的段索引
                    std::string temp_str(1, firstPrefixByte);
                    uint8_t new_segment_index = extract_subkey_segment(temp_str, old_local_depth + 1, 0);
                    old_segment_index = extract_subkey_segment(temp_str, old_local_depth, 0);
                    std::size_t new_bucket_index = std::distance(old_bucket.entries.begin(), it); // bucket的索引是不会变的
                    if (new_segment_index == old_segment_index * 2)
                    {
                        new_segment0->buckets[new_bucket_index].entries.push_back(entry);
                    }
                    else if (new_segment_index == old_segment_index * 2 + 1)
                    {
                        new_segment1->buckets[new_bucket_index].entries.push_back(entry);
                    }
                }
                // 将key-value放入新的段中
            }
        }
        // 处理完old_segment的所有桶后，进行指针的更新
        // 更新的逻辑是，这里要先缩小再放大
        // 因为这里的逻辑是这十六个指针都是创建好的，之后的段分裂是更新段指针
        // 这里是要从原来的段取出old_local_depth算得它的初始值，它分裂后按理是该初始值的两倍和两倍+1，但是要扩大到16的目录里，要算出它在16的目录里的最终值再放入
    }

    std::vector<int> resultZero;
    std::vector<int> resultOne;
    generate_new_segment_index(old_segment_index, old_local_depth, resultZero, resultOne);
    for (int i = 0; i < resultZero.size(); i++)
    {
        directory.segments[resultZero[i]] = new_segment0;
    }
    for (int i = 0; i < resultOne.size(); i++)
    {
        directory.segments[resultOne[i]] = new_segment1;
    } // 替换段指针即可
}

void MERTNode::generate_new_segment_index(int binaryNumber, int local_depth, std::vector<int> &resultZero, std::vector<int> &resultOne)
{
    int diff = 3 - local_depth;

    // 初始化temp_zero和temp_one
    int temp_zero = binaryNumber << 1;
    int temp_one = (binaryNumber << 1) + 1;

    std::vector<int> currentZero{temp_zero};
    std::vector<int> currentOne{temp_one};
    // 进行3-local_depth循环
    for (int i = 0; i < diff; i++)
    {
        std::vector<int> nextZero;
        std::vector<int> nextOne;
        // 对currentZero中的每个数添加0和1
        for (int num : currentZero)
        {
            nextZero.push_back(num << 1);
            nextZero.push_back((num << 1) + 1);
        }
        // 同理，对currentOne中的每个数添加0和1
        for (int num : currentOne)
        {
            nextOne.push_back(num << 1);
            nextOne.push_back((num << 1) + 1);
        }
        currentZero = nextZero;
        currentOne = nextOne;
    }
    resultZero = currentZero;
    resultOne = currentOne;
}

// 二进制转十进制，这里的string是二进制的，而不是直接的string
int MERTNode::binary_to_decimal(const std::string &binary_str)
{
    int decimal_value = 0;
    int base = 1; // 2^0
    for (int i = binary_str.length() - 1; i >= 0; i--)
    {
        if (binary_str[i] == '1')
        {
            decimal_value += base;
        }
        base *= 2;
    }
    return decimal_value;
}

/****
 * 这里的想法是先获取该bucket下的所有key-value，因为这些key-value至少有两个会有一个字节的前缀是相同的
 * 然后根据这个key-value数组，获取它们的最长前缀匹配，注意node的prefix不一定要填满
 * 非add_child_node的情况下，因为不知道要填入的数据关系，所以开局填入是该key直接填入到PrefixDirectory，如果key不够6个字节的话就后面遇到满足前缀的再后续填上
 * 这些key-value都存在最长公共前缀下的目录下
 *
 */
void MERTNode::add_child_node(MERTNode *new_node, Bucket &bucket, int start_pos)
{
    // 这个new_node是新创建的节点
    // std::unique_lock<std::shared_mutex> bucket_lock(bucket.bucket_lock);
    std::vector<std::string> temp_key;
    std::unordered_map<std::string, int> key2index;         // key和在原来的bucket的index的对应关系
    std::unordered_map<std::string, std::string> key2value; // key和value的对应关系
    for (auto it = bucket.entries.begin(); it != bucket.entries.end(); it++)
    {
        if (it->has_value())
        {
            auto &entry = it->value();
            if (std::holds_alternative<std::pair<std::string, std::string>>(entry))
            {
                // 如果是键值对的话，把key放入到数组中
                temp_key.push_back(std::get<std::pair<std::string, std::string>>(entry).first);
                key2index[std::get<std::pair<std::string, std::string>>(entry).first] = std::distance(bucket.entries.begin(), it);
                key2value[std::get<std::pair<std::string, std::string>>(entry).first] = std::get<std::pair<std::string, std::string>>(entry).second;
            }
        }
    }
    // 获取得到的键数组的只要存在的最长前缀，从start_pos开始，因为前面的都是相同的
    std::string common_prefix = longestCommonSubstringAmongTwo(temp_key, start_pos);
    // 获取最长前缀的子串(上一个prefix之后的)，放入到longestCommonSubstringAmongTwo新节点的prefix中
    // 然后再遍历字符串数组，如果完全匹配前缀的话就放入total_value,不是完全匹配的话就放入桶里
    for (int i = 0; i < std::min(common_prefix.length(), static_cast<size_t>(6)); i++)
    {
        new_node->header.prefix[i].c = common_prefix[i];
    }
    for (int i = 0; i < temp_key.size(); i++)
    {
        bool not_this_node = false;
        insert_to_new_node(new_node, temp_key[i], key2value[temp_key[i]], start_pos, not_this_node);
        if (not_this_node)
        {
            // 说明是完全插入不进去下一层节点中，继续留在该bucket中
            continue;
        }
        else
        {
            // 说明可以插入到下一层节点中，那先把这个bucket中该entry去除
            bucket.entries[key2index[temp_key[i]]] = std::nullopt;
        }
    }
}

void MERTNode::insert_to_new_node(MERTNode *new_node, const std::string &key, const std::string &value, int start_pos, bool &not_this_node)
{
    // start_pos是下标
    // std::unique_lock<std::shared_mutex> node_lock(new_node->node_lock_); // 先上锁吧
    int key_index = start_pos;
    // prefix和key匹配的长度
    int prefix_index_ = 0;
    // 求得前缀的有效长度
    int prefix_index_len = 0;
    // 首先查看一下这个node的prefix是多长
    while (prefix_index_len < 6 && new_node->header.prefix[prefix_index_len].c != 0)
    {
        prefix_index_len++;
    }
    // 然后查看key和prefix的最长匹配
    while (key_index < key.length() && prefix_index_ < prefix_index_len && key[key_index] == new_node->header.prefix[prefix_index_].c)
    {
        key_index++;
        prefix_index_++;
    }
    if (prefix_index_len == 0 && prefix_index_ == 0)
    {
        // 空节点，把key从start_pos开始赋值给prefix
        int will_len = std::min(key.length() - start_pos, static_cast<size_t>(6));
        if (will_len == key.length() - start_pos)
        {
            // 能放得下prefix中，所以value直接放入total_value中
            for (int i = start_pos; i < will_len + start_pos; i++)
            {
                new_node->header.prefix[i - start_pos].c = key[i];
            }
            new_node->total_value[will_len - 1] = value;
            return; // 已经存好了，所以直接return
        }
        else if (will_len < key.length() - start_pos)
        {
            // 先把能放的放进去，即6个字节放进prefix中
            for (int i = start_pos; i < start_pos + 6; i++)
            {
                new_node->header.prefix[i - start_pos].c = key[i];
            }
            // 这种prefix存不下的，所以要存放在最后一个prefix下的段桶里
            // 这里start_pos+6是指prefix后的第一个字节，6是指它放在prefix[5]的目录中
            insert_to_segment_bucket(new_node, key, value, start_pos + 6, 5);
            return; // 插入结束
        }
    }
    else if (prefix_index_len != 0 && prefix_index_ == 0)
    {
        // 这种是完全不匹配，需要新创建节点
        not_this_node = true;
        return;
    }
    else if (prefix_index_ != 0 && prefix_index_ < prefix_index_len && key_index <= key.length() - 1)
    {
        if (key_index == key.length() - 1)
        {
            // 完全匹配，直接赋值
            new_node->total_value[prefix_index_ - 1] = value;
            return;
        }
        else if (key_index < key.length() - 1)
        {
            // 不完全匹配，放入桶里
            insert_to_segment_bucket(new_node, key, value, key_index , prefix_index_);
            return;
        }
    }
    else if (prefix_index_ != 0 && prefix_index_ == prefix_index_len && key_index <= key.length() - 1)
    {

        if (key_index == key.length() - 1)
        {
            new_node->total_value[prefix_index_ - 1] = value;
            return; // 直接放入total_value
        }
        else if (key_index < key.length() - 1)
        {
            // 再看prefix能不能继续放，能的话就放，不能的话就进入桶
            if (prefix_index_ == 6)
            {
                // 说明不能放了。需要放入桶中
                // 这个也是放入prefix[5]的桶里
                insert_to_segment_bucket(new_node, key, value, key_index , 5);
                return;
            }
            else if (prefix_index_ < 6)
            {
                // 能放的话就继续放
                int maybe_len = std::min(key.length() - key_index, static_cast<size_t>(6 - prefix_index_));
                for (int i = prefix_index_; i < maybe_len; i++)
                {
                    new_node->header.prefix[i].c = key[key_index];
                    key_index++;
                }
                // 然后继续判断
                if (key_index == key.length() - 1)
                {
                    new_node->total_value[prefix_index_ - 1] = value;
                    return; // 直接放入total_value
                }
                else if (key_index < key.length() - 1)
                {
                    // 进入桶
                    // 说明key很长，继续放入prefix[5]的桶里
                    insert_to_segment_bucket(new_node, key, value, key_index, 5);
                }
            }
        }
    }
}

void MERTNode::insert_to_segment_bucket(MERTNode *this_node, const std::string &key, const std::string &value, int start_pos, int directory_index)
{
    /***
     * 进入段桶的逻辑是，根据，prefix后的第一个字节的前local_depth位,
     * 先查看local_depth是否为0，如果是0的话就分裂为2，如果不是的话就从1开始
     * 找段索引的逻辑是，直接获取4位的local_depth的值,然后获取segment的指针
     */
    uint8_t segment_index = extract_subkey_segment(key, 4, start_pos);
    uint8_t segment_local_depth = this_node->header.prefix[directory_index].segments[segment_index]->local_depth;
    uint8_t bucket_index = extract_subkey_bucket(key, 8);

    if (segment_local_depth == 0)
    {
        std::shared_ptr<MERTNode::Segment> new_segment = std::make_shared<MERTNode::Segment>();
        // 说明是第一个插入该node的(0~7目录或8~15目录)key-value，查看后四位local_depth的第一位是0还是1
        // 0的话0-7设为该segment指针，1的话8-15设为该segment指针
        uint8_t first_num = extract_subkey_segment(key, 1, start_pos);
        // 后8位为桶索引
        new_segment->buckets[bucket_index].entries.push_back(std::make_pair(key, value));
        if (first_num == 0)
        {
            // 因为这里是第一个，所以直接push_back即可
            // 0~7都需要插入该segment
            for (int i = 0; i < 8; i++)
            {
                this_node->header.prefix[directory_index].segments[i] = new_segment;
                this_node->header.prefix[directory_index].segments[i]->local_depth += 1; // 记得这里要+1啊
            }
        }
        else if (first_num == 1)
        {
            // 8~15都需要插入该segment
            for (int i = 8; i < 16; i++)
            {
                this_node->header.prefix[directory_index].segments[i] = new_segment;
                this_node->header.prefix[directory_index].segments[i]->local_depth += 1;
            }
        }
    }
    else
    {
        // 如果local_depth不为0的话，就要查看该segment下的桶是否已满
        // 逻辑是查看该segment下的桶是否已满，如果已满的话就要段分裂，如果段分裂都还是满的话需要继续add_new_node
        // 插入的逻辑是查看是否有bucket存放的是节点，如果是节点查看会不会更加匹配，如果会的话就继续存入这个节点里
        // 如果不会的话就存放在空的entry中
        int first_empty_index = -1;
        auto &entries = this_node->header.prefix[directory_index].segments[segment_index]->buckets[bucket_index].entries;
        for (auto it = entries.begin(); it != entries.end(); ++it)
        {
            if (it->has_value())
            {
                auto &entry = it->value();
                if (std::holds_alternative<std::shared_ptr<MERTNode>>(entry))
                {
                    // 如果是节点的话先看看这个节点能不能插入
                    bool not_this_node = false;
                    auto nodePtr = std::get<std::shared_ptr<MERTNode>>(entry).get();
                    insert_to_new_node(nodePtr, key, value, start_pos, not_this_node);
                    if (not_this_node)
                    {
                        continue;
                    }
                    else
                    {
                        return; // 说明插入到下一层节点了
                    }
                }
                else if (std::holds_alternative<std::pair<std::string, std::string>>(entry))
                {
                    // 说明这里已经有键值对了，查看这个这个key-value是否key相同，如果相同的话直接替换value即可
                    if (std::get<std::pair<std::string, std::string>>(entry).first == key)
                    {
                        std::get<std::pair<std::string, std::string>>(entry).second = value;
                        return;
                    }
                    else
                    {
                        continue;
                    }
                }
            }
            else
            {
                // 如果既不会进入下一层节点，也不会替换value，那就记录第一个空的entry位置
                first_empty_index = std::distance(entries.begin(), it);
            }
        }
        if (first_empty_index != -1)
        {
            this_node->header.prefix[directory_index].segments[segment_index]->buckets[bucket_index].entries[first_empty_index] = std::make_pair(key, value);
            return; // 插入完毕，返回
        }
        else if (first_empty_index == -1 && segment_local_depth < 4)
        {
            split_segment(segment_index, this_node->header.prefix[directory_index], 4);
            // 段分裂后也重新插入insert_to_segment_bucket(this_node, key, value, start_pos, directory_index);
        }
        else if (first_empty_index == -1 && segment_local_depth >= 4)
        {
            // 这里要继续生成下一层节点
            // 这里首先要创造一个新的节点，然后再把该key-value插入
            std::shared_ptr<MERTNode> new_node = std::make_shared<MERTNode>();
            auto new_node_ptr = new_node.get();
            add_child_node(new_node_ptr, this_node->header.prefix[directory_index].segments[segment_index]->buckets[bucket_index], start_pos);
            // 重新插入
            insert_to_segment_bucket(this_node, key, value, start_pos, directory_index);
        }
    }
    // 然后查看该segment下面的
}

void MERTRootNode::insert(const std::string &key, const std::string &value)
{
    // 这里是创造新的根节点，因为根节点会出现前缀完全不匹配的情况，所以这里要创建新的节点
    /*****
     * 1.创建新的节点，
     * 2.把已存在的节点放入到根节点的segment_bucket
     * 3.根节点的话prefix是为空的，而且bucket索引是节点的前八位了，而不是后八位
     * 4.因为节点的话不好获取它们的后八位，而且也不统一
     * 5.根节点的桶是存放mertnode的指针的，不存放key-value
     * 6.根节点的桶是存放指针的，
     */
    // uint8_t root_segment_index = cal_SegmentIndex(key);
    uint8_t root_bucket_index = cal_BucketIndex(key);
    bool not_this_node = false;

    if (root_bucket[root_bucket_index].node_entry.has_value())
    {
        // 获取在这里的MERTNode节点，并插入键值对
        std::shared_ptr<MERTNode> nodePtr = root_bucket[root_bucket_index].node_entry.value();
        nodePtr->insert_to_new_node(nodePtr.get(), key, value, 0, not_this_node);
    }
    else
    {
        // 如果没有的话就创建一个新的节点
        std::shared_ptr<MERTNode> new_node = std::make_shared<MERTNode>();
        auto new_node_ptr = new_node.get();
        new_node->insert_to_new_node(new_node_ptr, key, value, 0, not_this_node);
        root_bucket[root_bucket_index].node_entry = new_node;
    }
    // std::shared_ptr<MERTNode> new_root = std::make_shared<MERTNode>(0,config_);
}

uint8_t MERTRootNode::cal_BucketIndex(const std::string &key)
{
    if (key.empty())
    {
        return 0; // 如果 key 为空字符串，返回 0
    }
    // 直接将第 0 个字符转换为 uint8_t 类型，其值就是对应的十进制数
    return static_cast<uint8_t>(key[0]);
}

std::string MERTNode::longestCommonSubstringBetweenTwo(const std::string &s1, const std::string &s2, int start_pos)
{
    int len1 = s1.length();
    int len2 = s2.length();
    int max_len = 0;                                                   // 记录最大公共子串的长度
    int end_pos = 0;                                                   // 记录最大公共子串的结束位置
    std::vector<std::vector<int>> dp(len1, std::vector<int>(len2, 0)); // DP 表

    // 从 start_pos 开始进行动态规划计算
    for (int i = start_pos; i < len1; ++i)
    {
        for (int j = start_pos; j < len2; ++j)
        {
            if (s1[i] == s2[j])
            {
                dp[i][j] = (i > 0 && j > 0) ? dp[i - 1][j - 1] + 1 : 1; // 继续扩展公共子串
                if (dp[i][j] > max_len)
                {
                    max_len = dp[i][j]; // 更新最大公共子串长度
                    end_pos = i;        // 更新最大公共子串的结束位置
                }
            }
        }
    }

    // 返回最长公共子串
    return s1.substr(end_pos - max_len + 1, max_len);
}

// 查找字符串数组从 start_pos 开始，任意两个字符串间的最长公共子串
std::string MERTNode::longestCommonSubstringAmongTwo(const std::vector<std::string> &strs, int start_pos)
{
    std::string longest;
    int n = strs.size();
    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            std::string current = longestCommonSubstringBetweenTwo(strs[i], strs[j], start_pos);
            if (current.length() > longest.length())
            {
                longest = current; // 更新最长公共子串
            }
        }
    }
    return longest;
}

void MERT::insert(const std::string &key, const std::string &value)
{
    // 首先创造根节点
    root_.insert(key, value);
    return;
}

MERTNode::MERTNode()
{
    total_value.reserve(6);
    // 初始化一下prefix
    for (int i = 0; i < 6; i++)
    {
        header.prefix[i].prefix_index = i;
        header.prefix[i].segments.resize(16);
        for (int j = 0; j < 16; j++)
        {
            /* 创建segment对象，否则会段错误 */
            header.prefix[i].segments[j] = std::make_shared<MERTNode::Segment>();
        }
    }
}

MERTNode::Segment::Segment()
{
    local_depth = 0;
    buckets.reserve(256);
}

MERTRootNode::MERTRootNode()
{
    root_bucket.reserve(256);
} // 初始化根节点的桶

MERT::MERT()
{
    root_ = MERTRootNode();
}
