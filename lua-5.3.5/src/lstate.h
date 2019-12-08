/*
** $Id: lstate.h,v 2.133.1.1 2017/04/19 17:39:34 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include "lua.h"

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"


/*

** Some notes about garbage-collected objects: All objects in Lua must
** be kept somehow accessible until being freed, so all objects always
** belong to one (and only one) of these lists, using field 'next' of
** the 'CommonHeader' for the link:
**
** 'allgc': all objects not marked for finalization;
** 'finobj': all objects marked for finalization;
** 'tobefnz': all objects ready to be finalized;
** 'fixedgc': all objects that are not to be collected (currently
** only small strings, such as reserved words).
**
** Moreover, there is another set of lists that control gray objects.
** These lists are linked by fields 'gclist'. (All objects that
** can become gray have such a field. The field is not the same
** in all objects, but it always has this name.)  Any gray object
** must belong to one of these lists, and all objects in these lists
** must be gray:
**
** 'gray': regular gray objects, still waiting to be visited.
** 'grayagain': objects that must be revisited at the atomic phase.
**   That includes
**   - black objects got in a write barrier;
**   - all kinds of weak tables during propagation phase;
**   - all threads.
** 'weak': tables with weak values to be cleared;
** 'ephemeron': ephemeron tables with white->white entries;
** 'allweak': tables with weak keys and/or weak values to be cleared.
** The last three lists are used only during the atomic phase.

*/


struct lua_longjmp;  /* defined in ldo.c */


/*
** Atomic type (relative to signals) to better ensure that 'lua_sethook'
** is thread safe
*/
#if !defined(l_signalT)
#include <signal.h>
#define l_signalT	sig_atomic_t
#endif


/* extra stack space to handle TM calls and some other extras */
#define EXTRA_STACK   5


#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)    // 40


/* kinds of Garbage Collection */
#define KGC_NORMAL	0
#define KGC_EMERGENCY	1	/* gc was forced by an allocation failure */


typedef struct stringtable {
  TString **hash;
  int nuse;  /* number of elements */
  int size;  /* 容量，申请到的内存可存储的 TString* 数量 */
} stringtable;


/*
** Information about a call.
** When a thread yields, 'func' is adjusted to pretend that the
** top function has only the yielded values in its stack; in that
** case, the actual 'func' value is saved in field 'extra'.
** When a function calls another with a continuation, 'extra' keeps
** the function index so that, in case of errors, the continuation
** function can be called with the correct top.
*/
typedef struct CallInfo {
  StkId func;  /* function index in the stack */
  StkId	top;  /* top for this function */
  struct CallInfo *previous, *next;  /* dynamic call link */
  union {
    struct {  /* only for Lua functions */
      StkId base;  /* base for this function */
      const Instruction *savedpc;
    } l;
    struct {  /* only for C functions */
      lua_KFunction k;  /* continuation in case of yields */
      ptrdiff_t old_errfunc;
      lua_KContext ctx;  /* context info. in case of yields */
    } c;
  } u;
  ptrdiff_t extra;
  short nresults;  /* expected number of results from this function */
  unsigned short callstatus;
} CallInfo;


/*
** Bits in CallInfo status
*/
#define CIST_OAH	(1<<0)	/* original value of 'allowhook' */
#define CIST_LUA	(1<<1)	/* call is running a Lua function */
#define CIST_HOOKED	(1<<2)	/* call is running a debug hook */
#define CIST_FRESH	(1<<3)	/* call is running on a fresh invocation
                                   of luaV_execute */
#define CIST_YPCALL	(1<<4)	/* call is a yieldable protected call */
#define CIST_TAIL	(1<<5)	/* call was tail called */
#define CIST_HOOKYIELD	(1<<6)	/* last hook called yielded */
#define CIST_LEQ	(1<<7)  /* using __lt for __le */
#define CIST_FIN	(1<<8)  /* call is running a finalizer */

#define isLua(ci)	((ci)->callstatus & CIST_LUA)

/* assume that CIST_OAH has offset 0 and that 'v' is strictly 0/1 */
#define setoah(st,v)	((st) = ((st) & ~CIST_OAH) | (v))
#define getoah(st)	((st) & CIST_OAH)


/**
 * 'global state', 所有线程共享（见 lua_newthread ）
 * 
 * 全局共享数据
 * lua_Alloc frealloc;    // 内存分配的入口函数
 * void *ud;              // frealloc 的辅助数据，其实你可以把它当成虚拟机的标识数据，skynet的snlua就是这么干的
 * stringtable strt;      // 字符串 hash 表
 * TValue l_registry;     // 注册表。见 init_registry 一开始只有两个字段。代替了原来 "_G" 指向 l_gt 的功能
 * unsigned int seed;     // 哈希要用到的随机数种子
 * const lua_Number *version;     // 版本数字。通过它判断lua虚拟机是否完整初始化, lua_newstate -> f_luaopen
 * struct lua_State *mainthread;  // 主线程的lua虚拟机
 * lua_CFunction panic;   // 异常终止前执行的函数
 * struct lua_State *twups;       // lua_State 通过 twups 链接起来，这个是头结点
 * TString *memerrmsg;            // "not enough memory"
 * TString *tmname[TM_N];         // 元方法名称对应的 TString*
 * struct Table *mt[LUA_NUMTAGS]; // 各基本类型的元表
 * TString *strcache[STRCACHE_N][STRCACHE_M];   // API 用到的字符串缓存起来
 * 
 * 垃圾收集器 Garbage Collector
 * l_mem totalbytes;          // 有效内存大小。就是黑色、灰色节点，确定为活动节点的内存大小。
 * l_mem GCdebt;              // 当前待GC的数据大小。就是白色节点（待GC）占据的内存大小。
 * // totalbytes + GCdebt = 整个虚拟机占用内存大小
 * 
 * lu_mem GCmemtrav;          // 临时变量，记录每一步遍历的内存大小
 * lu_mem GCestimate;         // 一个估算值，根据这个计算 threshold (有效内存大小)
 * 
 * GC状态
 * lu_byte currentwhite;      // 当前白色，分成00白色和01白色。在两次完整GC间，交替变化。
 * lu_byte gcstate;           // GC状态
 * lu_byte gckind;            // GC类型：常规GC，通常为分步GC LUA_GCSTEP ；紧急GC，通常为完整GC LUA_GCCOLLECT
 * lu_byte gcrunning;         // GC进行中标记
 * 
 * GCObject *allgc;           // GCObject 单链表
 * GCObject **sweepgc;
 * GCObject *finobj;          // 拥有 __gc 的未被GC标记的对象记录到此
 * GCObject *gray;            // 灰色节点单链表
 * GCObject *grayagain;       // 两次GC间，黑变灰的节点。统一管理，放到最后 atomic 一次性操作。防止反复的黑变灰（同一个对象遍历多次）
 * 
 * // 白色：未遍历
 * // 容器类GCObject：定义了 gclist，可以加入到 gray 链表进行子节点遍历。
 * 
 * // weak table
 * // __mode='v'。如果没有数组，且存在v为白色的容器类GCObject，weak table链入
 * GCObject *weak;
 * 
 * // ephemeron table
 * // __mode='k'。如果存在k为白色的容器类GCObject，且v为白色（强）对象，week table链入：week table需要等一轮遍历之后，再遍历一遍。
 * GCObject *ephemeron;
 * 
 * // allweak table
 * // __mode='k'。如果存在k为白色的容器类GCObject，且v都不为白色，week table链入；或者 __mode='kv'。
 * GCObject *allweak;
 * 
 * GCObject *tobefnz;         // being-finalized表，记录的是前一轮GC的遗留对象
 * GCObject *fixedgc;
 * 
 * GC频率控制
 * unsigned int gcfinnum;     //
 * int gcpause;               // 可以配置的一个值，不是计算出来的，根据这个计算 threshold，以此来控制下一次GC触发的时间
 * int gcstepmul;
 * 
*/
typedef struct global_State {
  lua_Alloc frealloc;     /* function to reallocate memory */
  void *ud;               /* auxiliary data to 'frealloc' */

  l_mem totalbytes;       /* number of bytes currently allocated - GCdebt */
  l_mem GCdebt;           /* bytes allocated not yet compensated by the collector */
  lu_mem GCmemtrav;       /* memory traversed by the GC */
  lu_mem GCestimate;      /* an estimate of the non-garbage memory in use */

  stringtable strt;       /* hash table for strings */

  TValue l_registry;

  unsigned int seed;      /* randomized seed for hashes */
  
  lu_byte currentwhite;
  lu_byte gcstate;        /* state of garbage collector */
  lu_byte gckind;         /* kind of GC running */
  lu_byte gcrunning;      /* true if GC is running */

  GCObject *allgc;        /* list of all collectable objects */
  GCObject **sweepgc;     /* current position of sweep in list */
  GCObject *finobj;       /* list of collectable objects with finalizers */
  GCObject *gray;         /* list of gray objects */
  GCObject *grayagain;    /* list of objects to be traversed atomically */

  GCObject *weak;         /* list of tables with weak values */
  GCObject *ephemeron;    /* list of ephemeron tables (weak keys) */
  GCObject *allweak;      /* list of all-weak tables */
  GCObject *tobefnz;      /* list of userdata to be GC */
  GCObject *fixedgc;      /* list of objects not to be collected */

  struct lua_State *twups;      /* list of threads with open upvalues */

  unsigned int gcfinnum;        /* number of finalizers to call in each GC step */
  int gcpause;                  /* size of pause between successive GCs */
  int gcstepmul;                /* GC 'granularity' */

  lua_CFunction panic;          /* to be called in unprotected errors */
  struct lua_State *mainthread;
  
  const lua_Number *version;                  /* pointer to version number */

  TString *memerrmsg;                         /* memory-error message */

  TString *tmname[TM_N];                      /* array with tag-method names */
  struct Table *mt[LUA_NUMTAGS];              /* metatables for basic types */

  TString *strcache[STRCACHE_N][STRCACHE_M];  /* cache for strings in API */

} global_State;


/* 
 * lua_State
 *
 * 函数调用栈：操作局部变量的空间（寄存器）
 * StkId top;			    // 栈顶
 * StdId stack_last;	// 最末位的单元
 * stdId stack;			  // 栈底
 * int stacksize;		  // 栈的大小
 *
 * 函数信息栈
 * CallInfo *ci;		    // 当前调用的函数信息（可理解为栈顶）
 * CallInfo *base_ci;   // 栈底
 * unsigned short nci;  // 当前函数信息数量
 * int size_ci;			    // 栈的大小
 * 
 * 异常处理
 * lu_byte status;                  // 函数调用状态
 * struct lua_longjmp *errorJmp;    // 异常恢复点
 * ptrdiff_t errfunc;               // 异常处理函数
 * 
 * 状态信息
 * global_State                     // 全局信息，所有线程（协程）共享
 * 
 * upvalues
 * UpVal *openupval;
 * struct lua_State *twups; 
 * 
 * Hook
 * volatile lua_Hook hook;
 * int basehookcount;
 * int hookcount;
 * l_signalT hookmask;
 * lu_byte allowhook;
 * 
 */
struct lua_State {
  CommonHeader;
  
  unsigned short nci;               /* number of items in 'ci' list */  
  lu_byte status;
  StkId top;                        /* first free slot in the stack */

  global_State *l_G;

  CallInfo *ci;                     /* call info for current function */
  
  const Instruction *oldpc;         /* last pc traced */

  StkId stack_last;                 /* last free slot in the stack */
  StkId stack;                      /* stack base */

  UpVal *openupval;                 /* list of open upvalues in this stack */

  GCObject *gclist;

  struct lua_State *twups;          /* list of threads with open upvalues */
  struct lua_longjmp *errorJmp;     /* current error recover point */

  CallInfo base_ci;                 /* CallInfo for first level (C calling Lua) */
  volatile lua_Hook hook;

  ptrdiff_t errfunc;                /* current error handling function (stack index) */
  
  int stacksize;
  int basehookcount;
  int hookcount;
  unsigned short nny;               /* number of non-yieldable calls in stack */
  unsigned short nCcalls;           /* number of nested C calls */

  l_signalT hookmask;
  lu_byte allowhook;
};


#define G(L)	(L->l_G)


/**
 * 没有UpVal了
** Union of all collectable objects (only for conversions)
*/
union GCUnion {
  GCObject gc;  /* common header */
  struct TString ts;
  struct Udata u;
  union Closure cl;
  struct Table h;
  struct Proto p;
  struct lua_State th;  /* thread */
};


#define cast_u(o)	cast(union GCUnion *, (o))

/* macros to convert a GCObject into a specific value */
#define gco2ts(o)  \
	check_exp(novariant((o)->tt) == LUA_TSTRING, &((cast_u(o))->ts))
#define gco2u(o)  check_exp((o)->tt == LUA_TUSERDATA, &((cast_u(o))->u))
#define gco2lcl(o)  check_exp((o)->tt == LUA_TLCL, &((cast_u(o))->cl.l))
#define gco2ccl(o)  check_exp((o)->tt == LUA_TCCL, &((cast_u(o))->cl.c))
#define gco2cl(o)  \
	check_exp(novariant((o)->tt) == LUA_TFUNCTION, &((cast_u(o))->cl))
#define gco2t(o)  check_exp((o)->tt == LUA_TTABLE, &((cast_u(o))->h))
#define gco2p(o)  check_exp((o)->tt == LUA_TPROTO, &((cast_u(o))->p))
#define gco2th(o)  check_exp((o)->tt == LUA_TTHREAD, &((cast_u(o))->th))


/* macro to convert a Lua object into a GCObject */
#define obj2gco(v) \
	check_exp(novariant((v)->tt) < LUA_TDEADKEY, (&(cast_u(v)->gc)))


/* actual number of total bytes allocated */
#define gettotalbytes(g)	cast(lu_mem, (g)->totalbytes + (g)->GCdebt)

LUAI_FUNC void luaE_setdebt (global_State *g, l_mem debt);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);
LUAI_FUNC CallInfo *luaE_extendCI (lua_State *L);
LUAI_FUNC void luaE_freeCI (lua_State *L);
LUAI_FUNC void luaE_shrinkCI (lua_State *L);


#endif

