/*
** $Id: lmem.h,v 1.43.1.1 2017/04/19 17:20:42 roberto Exp $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/

#ifndef lmem_h
#define lmem_h


#include <stddef.h>

#include "llimits.h"
#include "lua.h"

/**
 * lua 内存分配调用流程
 * 各种内存管理宏定义 -> luaM_realloc_ -> l_alloc -> realloc
 * 
 * 1. 各种内存管理宏定义。分4类接口 new/realloc/free
 *    luaM_new：UpVal CallInfo
 *    luaM_newvector：对象中的成员是某些结构体的序列，通过此序列化。如
 *       lua_State 中的 *stack 寄存器栈(TValue)
 *       Table 中的 *node 哈希表(Node)
 *       Proto 中的 *code 指令集(Instruction)
 *       Proto 中的 *k 常量数组(TValue)
 *       Proto 中的 **p 函数中的函数(*Proto)
 *       Proto 中的 *upvalues upvalue信息(Upvaldesc)
 *       Proto 中的 *lineinfo 行号信息(int)
 *       Proto 中的 *locvars 局部变量(LocVar)
 *       因为newvector需要事先知道序列中元素的数量，在编译期的递归下降法中不太能做到。
 *       所以这个接口主要是undump字节码时调用。
 *    luaM_newobject：这个object代表的就是GCObject，有两处调用
 *       luaC_newobj 创建一个GCObject
 *       lua_newthread 创建可回收的lua_State，可想而知这是个协程
 * 
 *    luaM_reallocv：修改序列指向内存的大小。搞清楚b，on，n，e就好了
 *       b：base pointer。指向这篇序列的原始指针。
 *       on：old number。原来的元素个数。
 *       n：number。修改后的元素个数。
 *       e：element size。每个元素的大小。所以执行成功后，内存大小为 n*e
 *       保险起见，判断了 n*e 是否会超过一个size_t可以表示的最大的数（溢出）
 *       MAX_SIZET/(e) 没有判断除零，本意是如果异常了就尽快报出好了。
 *    luaM_reallocvector：对 luaM_reallocv 进行了一层包装，用于指定element的类型，进行类型转换后返回
 *    luaM_reallocvchar：序列元素为1个char的情况，此时不用指定e。
 *    luaM_growvector：在原来的序列上增加1个单位。grow有点特殊，在realloc之前，有一番处理
 *       luaM_growvector -> luaM_growaux_ -> luaM_reallocv -> luaM_realloc_ -> l_alloc -> realloc
 *       先看参数——
 *       v：vector。相当于上面的base pointer。
 *       nelems：当前有效元素的个数。
 *       size：分配的内存可以容纳的元素个数（容量capacity）。
 *       t：元素类型。
 *       limit：如果要申请新的内存，申请后vector内存大小的上限。
 *       e：error message。字符串，用于异常处理
 *       所以宏定义中，会判断 nelems+1会不会超过内存大小。如果不超过，是不需要修改这个指针v的
 *       luaM_growaux_ 的工作就是限额报警，并根据limit计算newsize，正常情况都是double计算。
 *   
 *    luaM_freemem：释放b指向的内存，大小为s。
 *    luaM_free：释放b指向的内存，大小通过sizeof去取。这是对luaM_freemem的简化
 *    luaM_freearray：释放连续的内存空间。n为元素数量，元素大小还是sizeof去取。
 * 
 * 2. luaM_realloc_ 的工作很简单：
 *    它拿到一个虚拟机L，一个指针block，原始内存大小osize，要把它改成新大小nsize，就拿了L->l_G.frealloc来调用。
 *    只是它多了一些和GC的交互，分为两个情况：
 *    1. frealloc 调用失败：会做一遍完整GC（前提是lua虚拟机已经完整初始化），然后再调用
 *    2. frealloc 调用成功：会更新L->l_G.GCdebt
 * 
 * 3. l_alloc：这个l_alloc函数就是L->l_G.frealloc，它是通过lua_newtable指定，并记录到global_State上的。
 *    这个函数功能非常简单，就是只会判断函数，从free/realloc两个接口中选择一个调用。
 *    free/malloc这类函数由C运行时库提供。
 * 
 * 内存管理的思路：
 * 内存申请避免不了调用free/malloc这些接口（当然你也可以尝试mmap）
 * 但是调用前给内存大小做些修正，在头部或者尾部打打上标记，并组织在自定义的全局/静态结构中是自由的。
 * 这里有3个做法可以尝试：
 * 1. 接入第三方内存分配器（Dynamic Memory Allocation）。这些内存分配器以动态库的方式提供，常见的有TCMalloc，jemalloc。
 *    其原始是，在不介入这些库的情况下，free/malloc使用的是C运行时库glibc/MSVC提供的原生接口（弱符号）。
 *    而这些第三方库中也会定义同名的接口（#define je_malloc malloc），这些符号会替换掉C运行时库的符号。
 * 2. lua_newstate使用自定义函数。
 *    在自定义函数中可以针对整个虚拟机做增量统计，第一个参数void *ud记录在L->l_G.ud，也是lua_newstate指定的。
 *    在lua原生代码中这个值为NULL，属于用户自定义项。你可以创建自定义结构实例，用它来标识lua虚拟机，记录统计信息。
 * 3. 对于整个虚拟机的内存统计，你可能会觉得粒度太大了。如果想知道内存中有多少table，使用效率如何，还得从别的地方下手。
 *    此时，你可能会想从宏定义中做手脚，让其支持不同类型的TValue内存统计。只是这个方法暂时还行不通，因为free类的宏，并没有传入类型参数。
 *    对于这类需求，需要了解GC的运作原理。很有可能GC就做了这样的工作，千万不要重复造轮子。
*/

/*
** This macro reallocs a vector 'b' from 'on' to 'n' elements, where
** each element has size 'e'. In case of arithmetic overflow of the
** product 'n'*'e', it raises an error (calling 'luaM_toobig'). Because
** 'e' is always constant, it avoids the runtime division MAX_SIZET/(e).
**
** (The macro is somewhat complex to avoid warnings:  The 'sizeof'
** comparison avoids a runtime comparison when overflow cannot occur.
** The compiler should be able to optimize the real test by itself, but
** when it does it, it may give a warning about "comparison is always
** false due to limited range of data type"; the +1 tricks the compiler,
** avoiding this warning but also this optimization.)
*/
#define luaM_reallocv(L,b,on,n,e) \
  (((sizeof(n) >= sizeof(size_t) && cast(size_t, (n)) + 1 > MAX_SIZET/(e)) \
      ? luaM_toobig(L) : cast_void(0)) , \
   luaM_realloc_(L, (b), (on)*(e), (n)*(e)))

/*
** Arrays of chars do not need any test
*/
#define luaM_reallocvchar(L,b,on,n)  \
    cast(char *, luaM_realloc_(L, (b), (on)*sizeof(char), (n)*sizeof(char)))

#define luaM_freemem(L, b, s)	luaM_realloc_(L, (b), (s), 0)
#define luaM_free(L, b)		luaM_realloc_(L, (b), sizeof(*(b)), 0)
#define luaM_freearray(L, b, n)   luaM_realloc_(L, (b), (n)*sizeof(*(b)), 0)

#define luaM_malloc(L,s)	luaM_realloc_(L, NULL, 0, (s))
#define luaM_new(L,t)		cast(t *, luaM_malloc(L, sizeof(t)))
#define luaM_newvector(L,n,t) \
		cast(t *, luaM_reallocv(L, NULL, 0, n, sizeof(t)))

#define luaM_newobject(L,tag,s)	luaM_realloc_(L, NULL, tag, (s))

#define luaM_growvector(L,v,nelems,size,t,limit,e) \
          if ((nelems)+1 > (size)) \
            ((v)=cast(t *, luaM_growaux_(L,v,&(size),sizeof(t),limit,e)))

#define luaM_reallocvector(L, v,oldn,n,t) \
   ((v)=cast(t *, luaM_reallocv(L, v, oldn, n, sizeof(t))))

LUAI_FUNC l_noret luaM_toobig (lua_State *L);

/* not to be called directly */
LUAI_FUNC void *luaM_realloc_ (lua_State *L, void *block, size_t oldsize,
                                                          size_t size);
LUAI_FUNC void *luaM_growaux_ (lua_State *L, void *block, int *size,
                               size_t size_elem, int limit,
                               const char *what);

#endif

