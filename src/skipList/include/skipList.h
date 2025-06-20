#ifndef SKIPLIST_H
#define SKIPLIST_H // 防止头文件多次包含

#include <cmath>   // 用于随机数生成 rand()
#include <cstdlib> // 数学函数
#include <cstring> // 处理C风格的字符串操作（memset()）
#include <fstream> // 用于多线程同步
#include <iostream>
#include <mutex>

#define STORE_FILE "store/dumpFile" // 定义跳表序列化数据的存储文件路径
static std::string delimiter = ":"; // key 和 value 在序列化中使用的分隔符（例如 "100:abc"）


// 跳表节点 Node --------------------------------------------------------------------------------
template <typename K, typename V> // 模板类，K 和 V 是类型参数，在使用类时才具体指定
class Node
{
public:
    Node() {}
    Node(K k, V v, int); // 使用给定的键值和层级构造
    ~Node();

    K get_key() const;
    V get_value() const;
    void set_value(V);

    Node<K, V> **forward; // 指向不同层级下一个节点的指针数组，跳表核心
    int node_level;       // 节点所在的最大层级

private:
    K key;
    V value;
};

// Node 构造实现
template <typename K, typename V>
Node<K, V>::Node(const K k, const V v, int level) // 传入键、值和层级，创建一个节点
{
    this->key = k;
    this->value = v;
    this->node_level = level;

    // level + 1, because array index is from 0 - level
    this->forward = new Node<K, V> *[level + 1]; // 创建时指定 level 层级，则 forward 数组大小为 level + 1（含 0 层）

    // Fill forward array with 0(NULL)
    memset(this->forward, 0, sizeof(Node<K, V> *) * (level + 1)); // 初始化所有指针为 nullptr
};

// Node 析构
template <typename K, typename V>
Node<K, V>::~Node()
{
    delete[] forward; // 只释放 forward 指针数组
};

// Node::get_key()
template <typename K, typename V>
K Node<K, V>::get_key() const
{
    return key;
};

// Node::get_value()
template <typename K, typename V>
V Node<K, V>::get_value() const
{
    return value;
};

// Node::set_value
template <typename K, typename V>
void Node<K, V>::set_value(V value)
{
    this->value = value;
};




// 辅助类 临时保存跳表中的所有键值对数据，并用于序列化和反序列化 ---------------------------------------------
template <typename K, typename V>
class SkipListDump
{
public:
    friend class boost::serialization::access; // 友元声明 允许 Boost.Serialization 库访问私有成员，实现序列化功能

    // Boost 要求定义的序列化接口
    template <class Archive>
    void serialize(Archive &ar, const unsigned int version)
    {
        ar & keyDumpVt_;
        ar & valDumpVt_;
    }
    
    // 分别保存跳表节点的所有键和值
    std::vector<K> keyDumpVt_;
    std::vector<V> valDumpVt_;

public:
    // 把跳表中的某个节点插入这个容器里
    void insert(const Node<K, V> &node);
};


// insert() 把节点的键和值存入类成员的两个 std::vector 容器
template <typename K, typename V>
void SkipListDump<K, V>::insert(const Node<K, V> &node)
{
    keyDumpVt_.emplace_back(node.get_key());  // 使用 emplace_back 向 keyDumpVt_ 向量末尾直接构造并插入该键
    valDumpVt_.emplace_back(node.get_value());// 使用 emplace_back 向 keyDumpVt_ 向量末尾直接构造并插入该值
}




// 跳表类  ---------------------------------------------------------------------------------------
template <typename K, typename V>
class SkipList
{
public:
    SkipList(int);
    ~SkipList();

    int get_random_level();             // 随机生成层级

    Node<K, V> *create_node(K, V, int); // 创建新节点

    int insert_element(K, V);           // 插入新元素
    void insert_set_element(K &, V &);  // 插入或更新（如果元素存在则改变其值）
   
    bool search_element(K, V &value);   // 查找节点
    void delete_element(K);             // 删除节点
    void clear(Node<K, V> *);           // 从传入节点开始，递归释放整条链表
    
    void display_list();                // 打印当前跳表的所有内容

    std::string dump_file();                    // 跳表中所有数据转储为字符串
    void load_file(const std::string &dumpStr); // 从已序列化的字符串中反序列化出 key-value，再插入回跳表中

    int size(); // 当前跳表中元素数量

private:
    void get_key_value_from_string(const std::string &str, std::string *key, std::string *value); // 从序列化字符串中提取 key 和 value
    bool is_valid_string(const std::string &str); // 判断字符串是否有效，用于校验格式

private:
    int _max_level;       // 跳表允许的最大层级
    int _skip_list_level; // 当前实际层数

    Node<K, V> *_header;  // 头节点，每一层的起点

    // file operator 文件流，用于数据的持久化存储和加载
    std::ofstream _file_writer;
    std::ifstream _file_reader;
    
    int _element_count;   // 当前跳表中节点的数量

    std::mutex _mtx;  
};


// SkipList 构造
template <typename K, typename V>
SkipList<K, V>::SkipList(int max_level)
{
    this->_max_level = max_level;
    this->_skip_list_level = 0;
    this->_element_count = 0;

    // 创建头结点
    K k;
    V v;
    this->_header = new Node<K, V>(k, v, _max_level); // 每一层都存在，作为每一层的起始点
};


// ~SkipList() 析构
template <typename K, typename V>
SkipList<K, V>::~SkipList()
{
    if (_file_writer.is_open()) // 如果输出文件流 _file_writer 已经打开，就将其关闭
    {
        _file_writer.close();
    }
    if (_file_reader.is_open()) // 如果输入文件流 _file_reader 已经打开，就将其关闭
    {
        _file_reader.close();
    }

    // 删除跳表中所有节点
    if (_header->forward[0] != nullptr) // 判断头节点的第 0 层是否连接着任何节点
    {
        clear(_header->forward[0]); // clear 是一个私有递归函数，会一直清除链表上的所有节点
    }
    delete (_header); // 删除头节点本身
}


// get_random_level() 随机生成层级
template <typename K, typename V>
int SkipList<K, V>::get_random_level()
{
    int k = 1; // 表示最底层至少会出现一次

    // 表示以 50% 的概率继续加一层
    while (rand() % 2) // rand() % 2 会生成 0 或 1，其中 1 的概率是 50%
    {
        k++;
    }
    k = (k < _max_level) ? k : _max_level; // 避免生成的层数 k 超过跳表定义的最大允许层级 _max_level
    return k;
};


// create_node() 创建新节点
template <typename K, typename V>
Node<K, V>* SkipList<K, V>::create_node(const K k, const V v, int level)
{
    Node<K, V>* n = new Node<K, V>(k, v, level); // 使用 new 关键字动态分配一个跳表节点对象
    return n;
}


// 对 insert_element() 的说明
// Insert given key and value in skip list
// return 1 means element exists
// return 0 means insert successfully
/*
                           +------------+
                           |  insert 50 |   现在要插入 key 为 50 的节点
                           +------------+
level 4     +-->1+                                                      100     第4层
                 |
                 |                      insert +----+
level 3         1+-------->10+---------------> | 50 |          70       100     key=50 的节点，在第 3 层中将出现在 10 和 70 之间
                                               |    |
                                               |    |
level 2         1          10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 1         1    4     10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 0         1    4   9 10         30   40  | 50 |  60      70       100
                                               +----+

*/


// insert_element() 插入新节点  
// 返回1 - key已存在，插入失败   返回0 - 插入成功
template <typename K, typename V>
int SkipList<K, V>::insert_element(const K key, const V value)
{
    _mtx.lock(); // 加锁保护临界区

    Node<K, V> *current = this->_header; // current 指向当前正在扫描的节点，初始为头节点
    
    Node<K, V> *update[_max_level + 1]; // update 数组，每一层待插入位置的前驱节点
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    // 从当前最高层 _skip_list_level 开始往下扫描
    for (int i = _skip_list_level; i >= 0; i--)
    {
        // 如果当前节点的 forward[i] 不是 null 且 key 小于目标 key，则向前走
        while (current->forward[i] != NULL && current->forward[i]->get_key() < key)
        {
            current = current->forward[i]; 
        }
        update[i] = current; // 找到当前层该插入 key 应处于的位置（即 update[i]）
    }

    
    // 检查 key 是否已存在
    current = current->forward[0]; // 跳到 level 0 查看插入位置的右边节点 current（只有 level 0 包含完整所有数据节点）
    if (current != NULL && current->get_key() == key) // 如果 key 已存在，则打印信息并返回 1，不插入
    {
        std::cout << "key: " << key << ", exists" << std::endl;
        _mtx.unlock();
        return 1;
    }
    
    if (current == NULL || current->get_key() != key) // 确认是新 key，可以插入
    {
        // 随机生产新节点的层级
        int random_level = get_random_level();

        // 如果新节点的层数大于当前跳表层数，需要更新 update[] 的高层部分
        if (random_level > _skip_list_level)
        {
            for (int i = _skip_list_level + 1; i < random_level + 1; i++)
            {
                update[i] = _header;
            }
            _skip_list_level = random_level;
        }

        // 创建新节点
        Node<K, V> *inserted_node = create_node(key, value, random_level);

        // 对每一层进行插入
        for (int i = 0; i <= random_level; i++)
        {
            inserted_node->forward[i] = update[i]->forward[i]; // 新节点的 forward 指向 update[i] 原本的 forward
            update[i]->forward[i] = inserted_node; // update[i] 的 forward 改为指向新节点
        }
        std::cout << "Successfully inserted key:" << key << ", value:" << value << std::endl;

        // 更新节点数
        _element_count++;
    }
    _mtx.unlock(); // 解锁
    return 0;
}


/**
 * \brief 作用与insert_element相同类似，
 * insert_element是插入新元素，
 * insert_set_element是插入元素，如果元素存在则改变其值
 */

// insert_set_element() 覆盖性插入
template <typename K, typename V>
void SkipList<K, V>::insert_set_element(K &key, V &value)
{
    V oldValue;
    // 如果 key 存在就更新(删除再插入)，不存在就插入
    if (search_element(key, oldValue)) 
    {
        delete_element(key); // 如果当前跳表中已存在该 key，先删除它
    } 
    insert_element(key, value); // 插入新的 key-value
}


// Search for element in skip list
/*
                           +------------+
                           |  select 60 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |
level 3         1+-------->10+------------------>50+           70       100
                                                   |
                                                   |
level 2         1          10         30         50|           70       100
                                                   |
                                                   |
level 1         1    4     10         30         50|           70       100
                                                   |
                                                   |
level 0         1    4   9 10         30   40    50+-->60      70       100
*/

// search_element() 查找节点
template <typename K, typename V>
bool SkipList<K, V>::search_element(K key, V &value)
{
    std::cout << "search_element-----------------" << std::endl;
    
    Node<K, V> *current = _header; // 从跳表的头节点 _header 开始

    // // 从上向下，每层向右
    for (int i = _skip_list_level; i >= 0; i--) // 从最高层 level 开始向下遍历每一层
    {
        while (current->forward[i] && current->forward[i]->get_key() < key) // 当前层级中 向右移动
        {
            current = current->forward[i];
        }
        // 到达当前层末尾（forward[i] == nullptr），或forward[i]->key >= key，即不能再右移
    }

    // 说明查找路径已经收窄至最底层，接下来就要在该层检查 目标节点是否就是 key
    current = current->forward[0];

    // 如果当前位置 current 不为 null，并且 key 相等，就表示查找成功
    if (current and current->get_key() == key)
    {
        value = current->get_value(); // 提取其 value 赋值给引用参数
        std::cout << "Found key: " << key << ", value: " << current->get_value() << std::endl;
        return true;
    }


    // 查找失败，返回 false
    std::cout << "Not Found Key:" << key << std::endl;
    return false;
}




// delete_element() 删除节点
template <typename K, typename V>
void SkipList<K, V>::delete_element(K key)
{
    _mtx.lock(); // 加锁，确保操作线程安全

    Node<K, V> *current = this->_header;
    Node<K, V> *update[_max_level + 1]; // 记录各层中 “指向删除节点前驱节点” 的指针
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    // 从最高层往下查找删除节点
    for (int i = _skip_list_level; i >= 0; i--)
    {
        // 在每一层中，向右遍历直到找到第一个 forward[i] 大于等于 key 的节点
        while (current->forward[i] != NULL && current->forward[i]->get_key() < key)
        {
            current = current->forward[i];
        }
        update[i] = current; // 前驱节点放入update
    }

    // 到第0层找到目标节点，执行删除
    current = current->forward[0];
    if (current != NULL && current->get_key() == key) // 判断目标节点是否就是我们要找的节点
    {
        // 从最低层开始向上，删除每一层的目标节点（跳过当前节点）
        for (int i = 0; i <= _skip_list_level; i++)
        {
            // 如果该层对应前驱节点指向的不是当前节点，说明该层不存在这个节点，停止
            if (update[i]->forward[i] != current)
                break;

            update[i]->forward[i] = current->forward[i]; // 前驱节点跳过当前节点
        }

        // 移除空层（如果最高层已没有节点则降层）
        while (_skip_list_level > 0 && _header->forward[_skip_list_level] == 0)
        {
            _skip_list_level--;
        }

        std::cout << "Successfully deleted key " << key << std::endl;
        delete current; // 释放节点 （create_node内部调用了 new）
        _element_count--; // 节点数量减少
    }

    _mtx.unlock(); // 解锁
    return;
}



// clear() 从传入节点开始，递归释放其后整条链表
template <typename K, typename V>
void SkipList<K, V>::clear(Node<K, V> *cur)
{
    // 因为 forward[0] 是最底层链表，能保证所有节点都能通过这一链遍历出来，所以只用跟踪这一层
    if (cur->forward[0] != nullptr) 
    {
        clear(cur->forward[0]); // 递归意味着从当前节点一直传递给最后一个节点，然后从尾节点开始往前依次释放。
    }

    delete (cur);
}


// display_list() 打印跳表所有内容
template <typename K, typename V>
void SkipList<K, V>::display_list()
{
    std::cout << "\n*****Skip List*****"
              << "\n";
    for (int i = 0; i <= _skip_list_level; i++)
    {
        Node<K, V> *node = this->_header->forward[i];
        std::cout << "Level " << i << ": ";
        while (node != NULL) // 逐层显示每个节点的键值对
        {
            std::cout << node->get_key() << ":" << node->get_value() << ";"; 
            node = node->forward[i];
        }
        std::cout << std::endl;
    }
}


// dump_file() 将节点数据从内存序列化成一个字符串
template <typename K, typename V>
std::string SkipList<K, V>::dump_file()
{
    // std::cout << "dump_file-----------------" << std::endl;
    //
    //
    // _file_writer.open(STORE_FILE);
    // 这行代码被注释，表示原先设计中想要打开一个文件（文件名STORE_FILE），准备写入数据，
    // 但现在改成了用字符串流序列化，暂时不做文件操作

    Node<K, V> *node = this->_header->forward[0]; 
    SkipListDump<K, V> dumper; // 辅助类对象dumper，负责收集所有跳表节点的key和value
    while (node != nullptr)
    {
        dumper.insert(*node);
        // _file_writer << node->get_key() << ":" << node->get_value() << "\n";
        // std::cout << node->get_key() << ":" << node->get_value() << ";\n";
        node = node->forward[0];
    }

    // Boost序列化库负责将数据结构转换成可保存或传输的文本格式

    std::stringstream ss; // 创建一个字符串流对象ss (字符串流相当于一个内存中的文本缓冲区，用于存放序列化后的文本数据)
    boost::archive::text_oarchive oa(ss); // 使用Boost序列化库，创建一个文本输出序列化档案oa，并绑定到字符串流ss
    oa << dumper;    // 把辅助容器dumper中的数据序列化写入字符串流，这样跳表所有节点的key、value就变成一个序列化字符串
    return ss.str(); // 返回字符串流ss中的内容
    
    // _file_writer.flush();
    // _file_writer.close();
    // 这两行文件写操作也被注释，原先可能想用文件流写入文件后刷新和关闭，
    // 现在转为只返回字符串，不涉及直接文件操作。
}


// load_file() 反序列化  字符串 --> 跳表节点
template <typename K, typename V>
void SkipList<K, V>::load_file(const std::string &dumpStr)
{
    // 原本是从文件逐行读取序列化数据，逐行提取 key 和 value 再插入跳表
    // 目前被注释，表明改用基于 Boost 序列化的方式恢复数据
    // _file_reader.open(STORE_FILE);
    // std::cout << "load_file-----------------" << std::endl;
    // std::string line;
    // std::string* key = new std::string();
    // std::string* value = new std::string();
    // while (getline(_file_reader, line)) {
    //     get_key_value_from_string(line, key, value);
    //     if (key->empty() || value->empty()) {
    //         continue;
    //     }
    //     // Define key as int type
    //     insert_element(stoi(*key), *value);
    //     std::cout << "key:" << *key << "value:" << *value << std::endl;
    // }
    // delete key;
    // delete value;
    // _file_reader.close();


    // 如果传入的序列化字符串为空，直接返回，不做任何操作
    if (dumpStr.empty())
    {
        return;
    }

    SkipListDump<K, V> dumper;      // SkipListDump 类型对象 dumper，用于存放反序列化得到的键值对
    std::stringstream iss(dumpStr); // 将传入的字符串 dumpStr 包装成字符串流 iss
    boost::archive::text_iarchive ia(iss); // 创建一个 Boost 文本输入归档 ia，绑定到字符串流 iss
    ia >> dumper; // 从 ia 归档流中读取数据，反序列化到 dumper 对象
    
    // 遍历反序列化后的所有键，向跳表中插入反序列化得到的键值对
    for (int i = 0; i < dumper.keyDumpVt_.size(); ++i) 
    {
        insert_element(dumper.keyDumpVt_[i], dumper.valDumpVt_[i]);
    }
}


// size() 跳表节点个数
template <typename K, typename V>
int SkipList<K, V>::size()
{
    return _element_count;
}


// get_key_value_from_string()  从序列化字符串中提取 key 和 value
template <typename K, typename V>
void SkipList<K, V>::get_key_value_from_string(const std::string &str, std::string *key, std::string *value)
{
    if (!is_valid_string(str)) // 判断输入字符串是否符合预期格式
    {
        return;
    }

    // 从字符串 str 开始位置，截取直到分隔符（delimiter 即 : ）第一次出现的位置之间的子串，作为键赋值给 *key
    *key = str.substr(0, str.find(delimiter));

    // 从分隔符 ":" 后面的位置开始截取到字符串结尾的子串，作为值赋给 *value
    *value = str.substr(str.find(delimiter) + 1, str.length());
}



// is_valid_string() 判断字符串是否有效
template <typename K, typename V>
bool SkipList<K, V>::is_valid_string(const std::string &str)
{
    // 首先判断字符串是否为空
    if (str.empty())
    {
        return false;
    }

    // 判断字符串中是否包含指定的分隔符 delimiter " : "
    if (str.find(delimiter) == std::string::npos)
    {
        return false;
    }

    return true;
}



#endif // SKIPLIST_H