# MERT

### 1.前缀匹配思想和可拓展哈希表的结合

![MERT 结构](images/MERTStructure.png)

MERTNode的结构大致如上，prefixdirectory存放匹配的前缀，当key-value插入时，采用最大匹配原则进入对应的prefixdirectory，再根据该prefix后的第一个字节的后四位进入对应的segment，进入segment后通过key的最后一个字节的八位进入对应的bucket，进入bucket后存放(或进入下一层更匹配的节点中)

当bucket满时，会进行段分裂，重新分配段的桶里的数据，每个segment都有local_depth，global_depth设置为16，当段无法再分裂时，就会创造下一层节点。

### 2.已完成内容

完成了基本框架的搭建(单线程)

可以进行不同键长度的插入操作

完成了insert的操作

### 3.todolist

后面逐步实现多线程的，暂时想的是多层级的互斥锁

性能不大好，测了一下大概是6000~8000RPS。后续查看是哪儿的瓶颈
