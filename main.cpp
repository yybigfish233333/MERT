#include <iostream>
#include <string>
#include <chrono>
#include <random>
#include "extendible_radix_tree/MERT.hh"

// 生成随机字符串
std::string generateRandomString(size_t length)
{
    static const std::string charset = "0123456789";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, charset.size() - 1);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i)
    {
        result += charset[dis(gen)];
    }
    return result;
}

int main()
{
    //std::cout << "this is my first try" << std::endl;
    MERT mert;
    const int numInsertions = 400000; // 插入操作的次数
    const size_t keyLength = 4;      // 键的长度
    const size_t valueLength = 10;    // 值的长度

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numInsertions; ++i)
    {
        std::string key = generateRandomString(keyLength);
        std::string value = generateRandomString(valueLength);
        mert.insert(key, value);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "插入 " << numInsertions << " 个键值对花费了 " << duration << " 毫秒。" << std::endl;

    return 0;
}
