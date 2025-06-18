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
// 模板类，K 和 V 是类型参数，在使用类时才具体指定
template <typename K, typename V>
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




// 用于跳表中节点的数据序列化和存储
template <typename K, typename V>
class SkipListDump
{
public:
    friend class boost::serialization::access; // 友元声明 允许 Boost.Serialization 库访问私有成员，实现序列化功能

    template <class Archive>
    void serialize(Archive &ar, const unsigned int version)
    {
        ar & keyDumpVt_;
        ar & valDumpVt_;
    }
    std::vector<K> keyDumpVt_;
    std::vector<V> valDumpVt_;

public:
    void insert(const Node<K, V> &node);
};



// 跳表类  ---------------------------------------------------------------------------------------
// Class template for Skip list
template <typename K, typename V>
class SkipList
{
public:
    SkipList(int);
    ~SkipList();

    int get_random_level();             // 随机生成层级
    Node<K, V> *create_node(K, V, int); // 创建节点
    int insert_element(K, V);           // 插入新元素

    void display_list();                // 打印list    
    bool search_element(K, V &value);   // 查找元素
    void delete_element(K);             // 删除元素
    void insert_set_element(K &, V &);  // 插入元素，如果元素存在则改变其值

    std::string dump_file();            // 内存数据转到文件
    void load_file(const std::string &dumpStr); // 从磁盘加载数据

    void clear(Node<K, V> *); // 递归删除节点
    int size(); // 当前跳表大小

private:
    void get_key_value_from_string(const std::string &str, std::string *key, std::string *value);
    bool is_valid_string(const std::string &str);

private:
    int _max_level;       // aximum level of the skip list 跳表允许的最大层级
    int _skip_list_level; // current level of skip list 当前实际层数

    Node<K, V> *_header;  // pointer to header node 跳表的头节点，起始点

    // file operator 文件流，用于数据的持久化存储和加载
    std::ofstream _file_writer;
    std::ifstream _file_reader;

    
    int _element_count;   // skiplist current element count 跳表中元素的数量

    std::mutex _mtx;      // mutex for critical section
};


// create new node
template <typename K, typename V>
Node<K, V> *SkipList<K, V>::create_node(const K k, const V v, int level)
{
    Node<K, V> *n = new Node<K, V>(k, v, level);
    return n;
}


// Insert given key and value in skip list
// return 1 means element exists
// return 0 means insert successfully
/*
                           +------------+
                           |  insert 50 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |                      insert +----+
level 3         1+-------->10+---------------> | 50 |          70       100
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


// insert_element
template <typename K, typename V>
int SkipList<K, V>::insert_element(const K key, const V value)
{
    _mtx.lock();
    Node<K, V> *current = this->_header;

    // create update array and initialize it
    // update is array which put node that the node->forward[i] should be operated later
    Node<K, V> *update[_max_level + 1];
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    // start form highest level of skip list
    for (int i = _skip_list_level; i >= 0; i--)
    {
        while (current->forward[i] != NULL && current->forward[i]->get_key() < key)
        {
            current = current->forward[i];
        }
        update[i] = current;
    }

    // reached level 0 and forward pointer to right node, which is desired to insert key.
    current = current->forward[0];

    // if current node have key equal to searched key, we get it
    if (current != NULL && current->get_key() == key)
    {
        std::cout << "key: " << key << ", exists" << std::endl;
        _mtx.unlock();
        return 1;
    }

    // if current is NULL that means we have reached to end of the level
    // if current's key is not equal to key that means we have to insert node between update[0] and current node
    if (current == NULL || current->get_key() != key)
    {
        // Generate a random level for node
        int random_level = get_random_level();

        // If random level is greater thar skip list's current level, initialize update value with pointer to header
        if (random_level > _skip_list_level)
        {
            for (int i = _skip_list_level + 1; i < random_level + 1; i++)
            {
                update[i] = _header;
            }
            _skip_list_level = random_level;
        }

        // create new node with random level generated
        Node<K, V> *inserted_node = create_node(key, value, random_level);

        // insert node
        for (int i = 0; i <= random_level; i++)
        {
            inserted_node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = inserted_node;
        }
        std::cout << "Successfully inserted key:" << key << ", value:" << value << std::endl;
        _element_count++;
    }
    _mtx.unlock();
    return 0;
}

// Display skip list
template <typename K, typename V>
void SkipList<K, V>::display_list()
{
    std::cout << "\n*****Skip List*****"
              << "\n";
    for (int i = 0; i <= _skip_list_level; i++)
    {
        Node<K, V> *node = this->_header->forward[i];
        std::cout << "Level " << i << ": ";
        while (node != NULL)
        {
            std::cout << node->get_key() << ":" << node->get_value() << ";";
            node = node->forward[i];
        }
        std::cout << std::endl;
    }
}

// todo 对dump 和 load 后面可能要考虑加锁的问题
// Dump data in memory to file
template <typename K, typename V>
std::string SkipList<K, V>::dump_file()
{
    // std::cout << "dump_file-----------------" << std::endl;
    //
    //
    // _file_writer.open(STORE_FILE);
    Node<K, V> *node = this->_header->forward[0];
    SkipListDump<K, V> dumper;
    while (node != nullptr)
    {
        dumper.insert(*node);
        // _file_writer << node->get_key() << ":" << node->get_value() << "\n";
        // std::cout << node->get_key() << ":" << node->get_value() << ";\n";
        node = node->forward[0];
    }
    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);
    oa << dumper;
    return ss.str();
    // _file_writer.flush();
    // _file_writer.close();
}


// Load data from disk
template <typename K, typename V>
void SkipList<K, V>::load_file(const std::string &dumpStr)
{
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

    if (dumpStr.empty())
    {
        return;
    }
    SkipListDump<K, V> dumper;
    std::stringstream iss(dumpStr);
    boost::archive::text_iarchive ia(iss);
    ia >> dumper;
    for (int i = 0; i < dumper.keyDumpVt_.size(); ++i)
    {
        insert_element(dumper.keyDumpVt_[i], dumper.keyDumpVt_[i]);
    }
}


// Get current SkipList size
template <typename K, typename V>
int SkipList<K, V>::size()
{
    return _element_count;
}


// get_key_value_from_string()
template <typename K, typename V>
void SkipList<K, V>::get_key_value_from_string(const std::string &str, std::string *key, std::string *value)
{
    if (!is_valid_string(str))
    {
        return;
    }
    *key = str.substr(0, str.find(delimiter));
    *value = str.substr(str.find(delimiter) + 1, str.length());
}

template <typename K, typename V>
bool SkipList<K, V>::is_valid_string(const std::string &str)
{
    if (str.empty())
    {
        return false;
    }
    if (str.find(delimiter) == std::string::npos)
    {
        return false;
    }
    return true;
}



// delete_element()   Delete element from skip list
template <typename K, typename V>
void SkipList<K, V>::delete_element(K key)
{
    _mtx.lock();
    Node<K, V> *current = this->_header;
    Node<K, V> *update[_max_level + 1];
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    // start from highest level of skip list
    for (int i = _skip_list_level; i >= 0; i--)
    {
        while (current->forward[i] != NULL && current->forward[i]->get_key() < key)
        {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0];
    if (current != NULL && current->get_key() == key)
    {
        // start for lowest level and delete the current node of each level
        for (int i = 0; i <= _skip_list_level; i++)
        {
            // if at level i, next node is not target node, break the loop.
            if (update[i]->forward[i] != current)
                break;

            update[i]->forward[i] = current->forward[i];
        }

        // Remove levels which have no elements
        while (_skip_list_level > 0 && _header->forward[_skip_list_level] == 0)
        {
            _skip_list_level--;
        }

        std::cout << "Successfully deleted key " << key << std::endl;
        delete current;
        _element_count--;
    }
    _mtx.unlock();
    return;
}



/**
 * \brief 作用与insert_element相同类似，
 * insert_element是插入新元素，
 * insert_set_element是插入元素，如果元素存在则改变其值
 */

// insert_set_element
template <typename K, typename V>
void SkipList<K, V>::insert_set_element(K &key, V &value)
{
    V oldValue;
    if (search_element(key, oldValue))
    {
        delete_element(key);
    }
    insert_element(key, value);
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

// search_element()
template <typename K, typename V>
bool SkipList<K, V>::search_element(K key, V &value)
{
    std::cout << "search_element-----------------" << std::endl;
    Node<K, V> *current = _header;

    // start from highest level of skip list
    for (int i = _skip_list_level; i >= 0; i--)
    {
        while (current->forward[i] && current->forward[i]->get_key() < key)
        {
            current = current->forward[i];
        }
    }

    // reached level 0 and advance pointer to right node, which we search
    current = current->forward[0];

    // if current node have key equal to searched key, we get it
    if (current and current->get_key() == key)
    {
        value = current->get_value();
        std::cout << "Found key: " << key << ", value: " << current->get_value() << std::endl;
        return true;
    }

    std::cout << "Not Found Key:" << key << std::endl;
    return false;
}


// insert()
template <typename K, typename V>
void SkipListDump<K, V>::insert(const Node<K, V> &node)
{
    keyDumpVt_.emplace_back(node.get_key());
    valDumpVt_.emplace_back(node.get_value());
}

// SkipList构造 construct skip list
template <typename K, typename V>
SkipList<K, V>::SkipList(int max_level)
{
    this->_max_level = max_level;
    this->_skip_list_level = 0;
    this->_element_count = 0;

    // create header node and initialize key and value to null
    K k;
    V v;
    this->_header = new Node<K, V>(k, v, _max_level);
};


// ~SkipList() 析构
template <typename K, typename V>
SkipList<K, V>::~SkipList()
{
    if (_file_writer.is_open())
    {
        _file_writer.close();
    }
    if (_file_reader.is_open())
    {
        _file_reader.close();
    }

    // 递归删除跳表链条
    if (_header->forward[0] != nullptr)
    {
        clear(_header->forward[0]);
    }
    delete (_header);
}
template <typename K, typename V>
void SkipList<K, V>::clear(Node<K, V> *cur)
{
    if (cur->forward[0] != nullptr)
    {
        clear(cur->forward[0]);
    }
    delete (cur);
}


// get_random_level()
template <typename K, typename V>
int SkipList<K, V>::get_random_level()
{
    int k = 1;
    while (rand() % 2)
    {
        k++;
    }
    k = (k < _max_level) ? k : _max_level;
    return k;
};

// vim: et tw=100 ts=4 sw=4 cc=120

#endif // SKIPLIST_H