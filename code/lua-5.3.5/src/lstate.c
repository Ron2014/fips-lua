/*
** $Id: lstate.c,v 2.133.1.1 2017/04/19 17:39:34 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/

#define lstate_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


#if !defined(LUAI_GCPAUSE)
#define LUAI_GCPAUSE	200  /* 200% */
#endif

#if !defined(LUAI_GCMUL)
#define LUAI_GCMUL	200 /* GC runs 'twice the speed' of memory allocation */
#endif


/*
** a macro to help the creation of a unique random seed when a state is
** created; the seed is used to randomize hashes.
*/
#if !defined(luai_makeseed)
#include <time.h>
#define luai_makeseed()		cast(unsigned int, time(NULL))
#endif



/*
** thread state + extra space
*/
typedef struct LX {
  lu_byte extra_[LUA_EXTRASPACE];     // 一个指针的内存大小，又分割成字节数组
  lua_State l;
} LX;


/*
** Main thread combines a thread state and the global state
*/
typedef struct LG {
  LX l;
  global_State g;
} LG;



#define fromstate(L)	(cast(LX *, cast(lu_byte *, (L)) - offsetof(LX, l)))


/*
** Compute an initial seed as random as possible. Rely on Address Space
** Layout Randomization (if present) to increase randomness..
*/
#define addbuff(b,p,e) \
  { size_t t = cast(size_t, e); \
    memcpy(b + p, &t, sizeof(t)); p += sizeof(t); }

static unsigned int makeseed (lua_State *L) {
  char buff[4 * sizeof(size_t)];
  unsigned int h = luai_makeseed();
  
  // 将4种类型的指针地址号拷贝到 buff 里
  // p 最终等于 sizeof(void*) * 4
  int p = 0;
  addbuff(buff, p, L);  /* heap variable */
  addbuff(buff, p, &h);  /* local variable */
  addbuff(buff, p, luaO_nilobject);  /* global variable */
  addbuff(buff, p, &lua_newstate);  /* public function */
  
  lua_assert(p == sizeof(buff));
  return luaS_hash(buff, p, h);
}


/*
** set GCdebt to a new value keeping the value (totalbytes + GCdebt)
** invariant (and avoiding underflows in 'totalbytes')
*/
void luaE_setdebt (global_State *g, l_mem debt) {
  l_mem tb = gettotalbytes(g);
  lua_assert(tb > 0);
  if (debt < tb - MAX_LMEM)
    debt = tb - MAX_LMEM;  /* will make 'totalbytes == MAX_LMEM' */
  g->totalbytes = tb - debt;
  g->GCdebt = debt;
}


CallInfo *luaE_extendCI (lua_State *L) {
  CallInfo *ci = luaM_new(L, CallInfo);
  lua_assert(L->ci->next == NULL);
  L->ci->next = ci;
  ci->previous = L->ci;
  ci->next = NULL;
  L->nci++;
  return ci;
}


/*
** free all CallInfo structures not in use by a thread
*/
void luaE_freeCI (lua_State *L) {
  CallInfo *ci = L->ci;
  CallInfo *next = ci->next;
  ci->next = NULL;
  while ((ci = next) != NULL) {
    next = ci->next;
    luaM_free(L, ci);
    L->nci--;
  }
}


/*
** free half of the CallInfo structures not in use by a thread
*/
void luaE_shrinkCI (lua_State *L) {
  CallInfo *ci = L->ci;
  CallInfo *next2;  /* next's next */
  /* while there are two nexts */
  while (ci->next != NULL && (next2 = ci->next->next) != NULL) {
    luaM_free(L, ci->next);  /* free next */
    L->nci--;
    ci->next = next2;  /* remove 'next' from the list */
    next2->previous = ci;
    ci = next2;  /* keep next's next */
  }
}


static void stack_init (lua_State *L1, lua_State *L) {
  int i; CallInfo *ci;
  /* initialize stack array */
  L1->stack = luaM_newvector(L, BASIC_STACK_SIZE, TValue);
  L1->stacksize = BASIC_STACK_SIZE;
  for (i = 0; i < BASIC_STACK_SIZE; i++)
    setnilvalue(L1->stack + i);  /* erase new stack */
  L1->top = L1->stack;
  L1->stack_last = L1->stack + L1->stacksize - EXTRA_STACK;
  /* initialize first ci */
  ci = &L1->base_ci;
  ci->next = ci->previous = NULL;
  ci->callstatus = 0;
  ci->func = L1->top;
  setnilvalue(L1->top++);  /* 'function' entry for this 'ci' */
  ci->top = L1->top + LUA_MINSTACK;
  L1->ci = ci;
}


static void freestack (lua_State *L) {
  if (L->stack == NULL)
    return;  /* stack not completely built yet */
  L->ci = &L->base_ci;  /* free the entire 'ci' list */
  luaE_freeCI(L);
  lua_assert(L->nci == 0);
  luaM_freearray(L, L->stack, L->stacksize);  /* free stack array */
}


/*
 * 注册表 registry 是 global_State 记录的一张表：g->l_registry，全局唯一
 * 对于 L 中的数据访问，指定索引为 LUA_REGISTRYINDEX，会在 index2addr 中定位于此
 * 
 * 其中包含两个k-v映射关系(k都是int)
 * LUA_RIDX_MAINTHREAD    主线程(lua_State)
 * LUA_RIDX_GLOBALS       _G表(table)
 * 
 * 
** Create registry table and its predefined values
*/
static void init_registry (lua_State *L, global_State *g) {
  TValue temp;
  /* create registry */
  Table *registry = luaH_new(L);
  sethvalue(L, &g->l_registry, registry);
  luaH_resize(L, registry, LUA_RIDX_LAST, 0);   // LUA_RIDX_LAST 表示的是个长度值

  /* registry[LUA_RIDX_MAINTHREAD] = L */
  setthvalue(L, &temp, L);  /* temp = L */
  luaH_setint(L, registry, LUA_RIDX_MAINTHREAD, &temp);
  /**
   * LUA_RIDX_MAINTHREAD 没有多大作用
   * 获取主线程的方式可以先拿到 global_State, 然后获取它的 mainthread 字段
   * 参考: lua_newstate
  */
  
  /* registry[LUA_RIDX_GLOBALS] = table of globals */
  sethvalue(L, &temp, luaH_new(L));  /* temp = new table (global table) */
  luaH_setint(L, registry, LUA_RIDX_GLOBALS, &temp);
}


/*
** open parts of the state that may cause memory-allocation errors.
** ('g->version' != NULL flags that the state was completely build)
*/
static void f_luaopen (lua_State *L, void *ud) {
  global_State *g = G(L);
  UNUSED(ud);
  stack_init(L, L);       /* init stack */
  init_registry(L, g);    // 初始化注册表(table)
  
  luaS_init(L);           // 初始化(短)字符串哈希(缓存)表
  luaT_init(L);           // 初始化元方法的名称列表
  luaX_init(L);           // 初始化关键字

  g->gcrunning = 1;  /* allow gc */
  g->version = lua_version(NULL);
  luai_userstateopen(L);
}


/**
 * 初始化主线程 lua_State
 * 
** preinitialize a thread with consistent values without allocating
** any memory (to avoid errors)
*/
static void preinit_thread (lua_State *L, global_State *g) {
  G(L) = g;       // 最主要的操作：global_State 赋给这个线程（协程），G(L)->mainthread 就能找到主线程

  L->stack = NULL;
  L->ci = NULL;
  L->nci = 0;
  L->stacksize = 0;
  // 一开始初始化是 isintwups(L) 的
  L->twups = L;         /* thread has no upvalues */
  L->errorJmp = NULL;
  L->nCcalls = 0;
  L->hook = NULL;
  L->hookmask = 0;
  L->basehookcount = 0;
  L->allowhook = 1;
  resethookcount(L);
  L->openupval = NULL;
  L->nny = 1;
  L->status = LUA_OK;
  L->errfunc = 0;
}


static void close_state (lua_State *L) {
  global_State *g = G(L);
  luaF_close(L, L->stack);  /* close all upvalues for this thread */
  luaC_freeallobjects(L);  /* collect all objects */
  if (g->version)  /* closing a fully built state? */
    luai_userstateclose(L);
  luaM_freearray(L, G(L)->strt.hash, G(L)->strt.size);
  freestack(L);
  lua_assert(gettotalbytes(g) == sizeof(LG));
  (*g->frealloc)(g->ud, fromstate(L), sizeof(LG), 0);  /* free main block */
}


/**
 * 创建协程
 * newthread 只会创建一个新的 lua_State (非主线程)
 * global_State 是真的全局唯一 (所有 lua_State 共享)
*/
LUA_API lua_State *lua_newthread (lua_State *L) {
  global_State *g = G(L);   // 主线程拿到的 global_State
  lua_State *L1;
  lua_lock(L);
  luaC_checkGC(L);
  
  /**
   * 与 lua_newstate 不同
   * 这里申请的是 LX 空间 而非 LG
   * 
   * create new thread */
  L1 = &cast(LX *, luaM_newobject(L, LUA_TTHREAD, sizeof(LX)))->l;

  // 类似 luaC_newobj 的操作
  L1->marked = luaC_white(g);
  L1->tt = LUA_TTHREAD;
  /* link it on list 'allgc' */
  L1->next = g->allgc;
  g->allgc = obj2gco(L1);
  
  /* anchor it on L stack */
  setthvalue(L, L->top, L1);
  api_incr_top(L);

  preinit_thread(L1, g);    // 初始化子线程
  L1->hookmask = L->hookmask;
  L1->basehookcount = L->basehookcount;
  L1->hook = L->hook;
  resethookcount(L1);

  /* initialize L1 extra space */
  memcpy(lua_getextraspace(L1), lua_getextraspace(g->mainthread),
         LUA_EXTRASPACE);

  luai_userstatethread(L, L1);
  stack_init(L1, L);  /* init stack */
  lua_unlock(L);
  return L1;
}


void luaE_freethread (lua_State *L, lua_State *L1) {
  LX *l = fromstate(L1);
  luaF_close(L1, L1->stack);  /* close all upvalues for this thread */
  lua_assert(L1->openupval == NULL);
  luai_userstatefree(L, L1);
  freestack(L1);
  luaM_free(L, l);
}

/**
 * 创建主线程
*/
LUA_API lua_State *lua_newstate (lua_Alloc f, void *ud) {
  int i;
  lua_State *L;
  global_State *g;

  // lua_State 和 global_State 的内存空间是一起申请的
  // 也就是说, 它们的内存地址实际上是连续的
  LG *l = cast(LG *, (*f)(ud, NULL, LUA_TTHREAD, sizeof(LG)));

  if (l == NULL) return NULL;
  L = &l->l.l;
  g = &l->g;
  L->next = NULL;
  L->tt = LUA_TTHREAD;
  g->currentwhite = bitmask(WHITE0BIT);   // 01B
  L->marked = luaC_white(g);
  
  preinit_thread(L, g);   // L->l_G = g;--------------|
  g->frealloc = f;        // 内存申请函数              |-----两者互相建立关系
  g->ud = ud;             // 内存申请函数附加参数       |
  g->mainthread = L;      // g->mainthread = L;-------|

  g->seed = makeseed(L);  // 用于 luaS_hash 的种子

  g->gcrunning = 0;  /* no GC while building state */
  g->GCestimate = 0;
  
  g->strt.size = g->strt.nuse = 0;
  g->strt.hash = NULL;

  setnilvalue(&g->l_registry);
  g->panic = NULL;
  g->version = NULL;
  g->gcstate = GCSpause;
  g->gckind = KGC_NORMAL;
  g->allgc = g->finobj = g->tobefnz = g->fixedgc = NULL;
  g->sweepgc = NULL;
  g->gray = g->grayagain = NULL;
  g->weak = g->ephemeron = g->allweak = NULL;
  g->twups = NULL;
  g->totalbytes = sizeof(LG);
  g->GCdebt = 0;
  g->gcfinnum = 0;
  g->gcpause = LUAI_GCPAUSE;
  g->gcstepmul = LUAI_GCMUL;

  for (i=0; i < LUA_NUMTAGS; i++) g->mt[i] = NULL;
  if (luaD_rawrunprotected(L, f_luaopen, NULL) != LUA_OK) {
    /* memory allocation error: free partial state */
    close_state(L);
    L = NULL;
  }
  return L;
}


LUA_API void lua_close (lua_State *L) {
  L = G(L)->mainthread;  /* only the main thread can be closed */
  lua_lock(L);
  close_state(L);
}


