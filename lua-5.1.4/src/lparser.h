/*
** $Id: lparser.h,v 1.57.1.1 2007/12/27 13:02:25 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/

#ifndef lparser_h
#define lparser_h

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"


/*
** Expression descriptor
*/

typedef enum {
  VVOID,	/* no value */
  VNIL,
  VTRUE,
  VFALSE,
  VK,		/* info = index of constant in `k' */
  VKNUM,	/* nval = numerical value */
  VLOCAL,	/* info = local register */
  VUPVAL,       /* info = index of upvalue in `upvalues' */
  VGLOBAL,	/* info = index of table; aux = index of global name in `k' */
  VINDEXED,	/* info = table register; aux = index register (or `k') */
  VJMP,		/* info = instruction pc */
  VRELOCABLE,	/* info = instruction pc */
  VNONRELOC,	/* info = result register */
  VCALL,	/* info = instruction pc */
  VVARARG	/* info = instruction pc */
} expkind;

// 存放表达式的数据结构
typedef struct expdesc {
  // 表达式类型
  expkind k;
  // 保存表达式信息的联合体
  union {
    struct { // info 和aux里面存储的数据信息，根据不同的表达式类型有区分，见前面expkind的注释
    	int info,
    	aux; } s;
    lua_Number nval;  // 类型为数字的时候存储具体的数据
  } u;
  int t;  /* patch list of `exit when true' */
  int f;  /* patch list of `exit when false' */
} expdesc;


typedef struct upvaldesc {
  lu_byte k;
  lu_byte info;
} upvaldesc;


struct BlockCnt;  /* defined in lparser.c */


/* state needed to generate code for a given function */
/* 编译阶段用来辅助生成Proto的结构 */
typedef struct FuncState {
  Proto *f;  /* current function header */

  /* 
   * h代表的是常量数据
   * 因为FuncState不断向Proto装填其常量成员k
   * 利用FuncState自身的Table *h，可减少重复常量的内存申请（reuse）
   */
  Table *h;  /* table to find (and reuse) elements in `k' */

  /* 指向上一个FuncState，闭包函数的关系链，如
   *
   * function foo1()
   *	local a = 2
   *	function foo2()
   *		a = a + 1
   *	end
   * end
   *
   * FuncState(foo2).prev ==> FuncState(foo1)
   */
  struct FuncState *prev;  /* enclosing function */

  struct LexState *ls;  /* lexical state */
  struct lua_State *L;  /* copy of the Lua state */
  struct BlockCnt *bl;  /* chain of current blocks */
  int pc;  /* next position to code (equivalent to `ncode') */
  int lasttarget;   /* `pc' of last `jump target' */
  // 这里存放的是所有空悬,也就是没有确定好跳转位置的pc链表
  int jpc;  /* list of pending jumps to `pc' */
  int freereg;  /* first free register */
  int nk;  /* number of elements in `k' */
  int np;  /* number of elements in `p' */
  short nlocvars;  /* number of elements in `locvars' */
  lu_byte nactvar;  /* number of active local variables */
  upvaldesc upvalues[LUAI_MAXUPVALUES];  /* upvalues */
  unsigned short actvar[LUAI_MAXVARS];  /* declared-variable stack */
} FuncState;


LUAI_FUNC Proto *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                                            const char *name);


#endif
