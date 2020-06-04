/*
** $Id: lstring.c,v 2.56.1.1 2017/04/19 17:20:42 roberto Exp $
** String table (keeps all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#define lstring_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"


#define MEMERRMSG       "not enough memory"


/**
 * 2^LUAI_HASHLIMIT 次计算得到 hash 值
 * 
** Lua will use at most ~(2^LUAI_HASHLIMIT) bytes from a string to
** compute its hash
*/
#if !defined(LUAI_HASHLIMIT)
#define LUAI_HASHLIMIT		5
#endif


/*
** equality for long strings
*/
int luaS_eqlngstr (TString *a, TString *b) {
  size_t len = a->u.lnglen;
  lua_assert(a->tt == LUA_TLNGSTR && b->tt == LUA_TLNGSTR);
  return (a == b) ||  /* same instance or... */
    ((len == b->u.lnglen) &&  /* equal length and ... */
     (memcmp(getstr(a), getstr(b), len) == 0));  /* equal contents */
}


unsigned int luaS_hash (const char *str, size_t l, unsigned int seed) {
  unsigned int h = seed ^ cast(unsigned int, l);    // seed 与 (截断32位)长度 异或
  size_t step = (l >> LUAI_HASHLIMIT) + 1;          // 步长 = 长度/32 + 1
  for (; l >= step; l -= step)
    h ^= ((h<<5) + (h>>2) + cast_byte(str[l - 1]));
  return h;
}


/**
 * 通过 extra 判断长字符串没有hash
*/
unsigned int luaS_hashlongstr (TString *ts) {
  lua_assert(ts->tt == LUA_TLNGSTR);
  if (ts->extra == 0) {  /* no hash? */
    ts->hash = luaS_hash(getstr(ts), ts->u.lnglen, ts->hash);
    ts->extra = 1;  /* now it has its hash */
  }
  return ts->hash;
}


/*
** resizes the string table
*/
void luaS_resize (lua_State *L, int newsize) {
  int i;
  stringtable *tb = &G(L)->strt;

  if (newsize > tb->size) {  /* grow table if needed */
    luaM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
    // 初始化新申请的内存空间为NULL 
    // memset(tb->hash+tb->size, NULL, sizeof(TString*)*(newsize-tb->size))
    for (i = tb->size; i < newsize; i++)
      tb->hash[i] = NULL;
  }

  for (i = 0; i < tb->size; i++) {  /* rehash */
    TString *p = tb->hash[i];
    tb->hash[i] = NULL;

    // 重新分配新的 key 指向 TString
    // 整个过程是重排 k-v
    // 没有调用哈希函数 luaS_hash 去处理字符串:
    // TString 的 hash, 一次计算, 终身有效

    while (p) {  /* for each node in the list */
      TString *hnext = p->u.hnext;  /* save next */
      // 如果 newsize 是2的幂
      // hash%newsize 可转换成位运算 hash&(newsize-1)
      unsigned int h = lmod(p->hash, newsize);  /* new position */
      p->u.hnext = tb->hash[h];  /* chain it 哈希冲突的节点, 通过头插入形成单链表 head := tb->hash[h] */
      tb->hash[h] = p;

      p = hnext;
    }
  }

  if (newsize < tb->size) {  /* shrink table if needed */
    /* vanishing slice should be empty */
    // 做了简单检查: 要裁剪掉的内存区域的第一位和最后一位都为NULL(而不是遍历)
    lua_assert(tb->hash[newsize] == NULL && tb->hash[tb->size - 1] == NULL);
    luaM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
  }
  tb->size = newsize;

  /**
   * 所以你会发现 如果 newsize==size 只会触发 rehash
   * 但是 TString 的 hash 又不会变
   * rehash 只会让单链表 reverse, 除此之外没有任何变化(无用功)
   * 感觉需要规避 newsize==size 的情况
   */
  
}


/*
** Clear API string cache. (Entries cannot be empty, so fill them with
** a non-collectable string.)
*/
void luaS_clearcache (global_State *g) {
  int i, j;
  for (i = 0; i < STRCACHE_N; i++)
    for (j = 0; j < STRCACHE_M; j++) {
    if (iswhite(g->strcache[i][j]))  /* will entry be collected? */
      g->strcache[i][j] = g->memerrmsg;  /* replace it with something fixed */
    }
}


/*
** Initialize the string table and the string cache
*/
void luaS_init (lua_State *L) {
  global_State *g = G(L);
  int i, j;
  luaS_resize(L, MINSTRTABSIZE);  /* initial size of string table */
  
  /* pre-create memory-error message */
  // 这是个短字符串, 会存到哈希表中, 但是理论上永远不会被GC的
  g->memerrmsg = luaS_newliteral(L, MEMERRMSG); // not enough memory 
  luaC_fix(L, obj2gco(g->memerrmsg));  /* it should never be collected */
  
  for (i = 0; i < STRCACHE_N; i++)  /* fill cache with valid strings */
    for (j = 0; j < STRCACHE_M; j++)
      g->strcache[i][j] = g->memerrmsg;
}



/*
** creates a new string object
*/
static TString *createstrobj (lua_State *L, size_t l, int tag, unsigned int h) {
  TString *ts;
  GCObject *o;
  size_t totalsize;  /* total size of TString object */
  totalsize = sizelstring(l);
  o = luaC_newobj(L, tag, totalsize);
  ts = gco2ts(o);
  ts->hash = h;
  ts->extra = 0;
  getstr(ts)[l] = '\0';  /* ending 0 */
  return ts;
}


/**
 * 创建长字符串
 * 你会发现它的hash值是seed, 即没有hash
*/
TString *luaS_createlngstrobj (lua_State *L, size_t l) {
  TString *ts = createstrobj(L, l, LUA_TLNGSTR, G(L)->seed);
  ts->u.lnglen = l;
  return ts;
}


void luaS_remove (lua_State *L, TString *ts) {
  stringtable *tb = &G(L)->strt;
  TString **p = &tb->hash[lmod(ts->hash, tb->size)];
  while (*p != ts)  /* find previous element */
    p = &(*p)->u.hnext;
  *p = (*p)->u.hnext;  /* remove element from its list */
  tb->nuse--;
}


/*
** checks whether short string exists and reuses it or creates a new one
*/
static TString *internshrstr (lua_State *L, const char *str, size_t l) {
  TString *ts;
  global_State *g = G(L);

  // 通过 hash 值拿到冲突链
  unsigned int h = luaS_hash(str, l, g->seed);
  TString **list = &g->strt.hash[lmod(h, g->strt.size)];
  lua_assert(str != NULL);  /* otherwise 'memcmp'/'memcpy' are undefined */

  for (ts = *list; ts != NULL; ts = ts->u.hnext) {
    if (l == ts->shrlen &&
        (memcmp(str, getstr(ts), l * sizeof(char)) == 0)) {
      // 这个过程有内存比较(遍历), 还是很耗时的
      /* found! */
      if (isdead(g, ts))  /* dead (but not collected yet)? */
        changewhite(ts);  /* resurrect it */
      return ts;
    }
  }

  if (g->strt.nuse >= g->strt.size && g->strt.size <= MAX_INT/2) {
    luaS_resize(L, g->strt.size * 2);
    list = &g->strt.hash[lmod(h, g->strt.size)];  /* recompute with new size */
  }

  ts = createstrobj(L, l, LUA_TSHRSTR, h);
  memcpy(getstr(ts), str, l * sizeof(char));
  ts->shrlen = cast_byte(l);
  ts->u.hnext = *list;
  *list = ts;
  g->strt.nuse++;
  return ts;
}


/*
** new string (with explicit length)
*/
TString *luaS_newlstr (lua_State *L, const char *str, size_t l) {
  if (l <= LUAI_MAXSHORTLEN)  /* short string? */
    // 仅对长度40以内的短字符串做 hash 缓存
    return internshrstr(L, str, l);
  else {
    /**
     * 长字符串没有缓存，直接申请内存。
     * 长字符串最长支持到 MAX_SIZE, 这个值是 min(size_t上限, lua_Integer上限)
     * 32位: 4G
     * 64位: 4G*4G (算是无限长了)
     * 问题是, 我们有需要去创建几个G大小的 TString 么?
     * 
     * 好处：
     * 1. 短字符串 hash 计算较快，提高查找效率。
     * 2. 字符串是 GCObject ，这表示它会记录在 global_State.allgc 链表上。
     *  同时，全局字符串缓存表 global_State.strt[hash] 也会记录其地址。
     *  这表示字符串的 GC 回收要修改两处地方。长字符串则很容易成为垃圾对象，内存使用率不高。
     * 坏处：
     * 1. 间接要求了配置表中，不要有大片的重复的文本配置。
     */
    TString *ts;
    if (l >= (MAX_SIZE - sizeof(TString))/sizeof(char))
      luaM_toobig(L);
    ts = luaS_createlngstrobj(L, l);
    memcpy(getstr(ts), str, l * sizeof(char));
    return ts;
  }
}


/**
 * global_State 的 strcache 是真的缓存
 * strt 只能说是个全局字符串哈希表
 * 
 * 或者也可以理解成:
 * strcache 是1级缓存
 * strt 是2级缓存/内存
 * 
 * 使用过(活跃)的 TString 优先排列在最前(p[0])
 * 提高下次 luaS_new 同一个字符串的命中率
 * 
 * 也许, 缓存(cache)-命中率(hit) 这两个概念应该绑定在一起
 * 
** Create or reuse a zero-terminated string, first checking in the
** cache (using the string address as a key). The cache can contain
** only zero-terminated strings, so it is safe to use 'strcmp' to
** check hits.
*/
TString *luaS_new (lua_State *L, const char *str) {
  unsigned int i = point2uint(str) % STRCACHE_N;  /* hash 直接拿内存地址号取模 */
  int j;
  TString **p = G(L)->strcache[i];
  for (j = 0; j < STRCACHE_M; j++) {
    if (strcmp(str, getstr(p[j])) == 0)  /* hit? */
      return p[j];  /* that is it */
  }
  /* normal route 内存右移, 腾出p[0] */
  for (j = STRCACHE_M - 1; j > 0; j--)
    p[j] = p[j - 1];  /* move out last element */
  /* new element is first in the list */
  p[0] = luaS_newlstr(L, str, strlen(str));
  return p[0];
}


Udata *luaS_newudata (lua_State *L, size_t s) {
  Udata *u;
  GCObject *o;
  if (s > MAX_SIZE - sizeof(Udata))
    luaM_toobig(L);
  o = luaC_newobj(L, LUA_TUSERDATA, sizeludata(s));
  u = gco2u(o);
  u->len = s;
  u->metatable = NULL;
  setuservalue(L, u, luaO_nilobject);
  return u;
}

