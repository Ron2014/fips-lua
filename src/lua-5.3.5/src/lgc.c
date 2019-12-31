/*
** $Id: lgc.c,v 2.215.1.2 2017/08/31 16:15:27 roberto Exp $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#define lgc_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


/*
** internal state for collector while inside the atomic phase. The
** collector should never be in this state while running regular code.
*/
#define GCSinsideatomic		(GCSpause + 1)

/*
** cost of sweeping one element (the size of a small object divided
** by some adjust for the sweep speed)
*/
#define GCSWEEPCOST	((sizeof(TString) + 4) / 4)

/* maximum number of elements to sweep in each single step */
#define GCSWEEPMAX	(cast_int((GCSTEPSIZE / GCSWEEPCOST) / 4))

/* cost of calling one finalizer */
#define GCFINALIZECOST	GCSWEEPCOST


/*
** macro to adjust 'stepmul': 'stepmul' is actually used like
** 'stepmul / STEPMULADJ' (value chosen by tests)
*/
#define STEPMULADJ		200


/*
** macro to adjust 'pause': 'pause' is actually used like
** 'pause / PAUSEADJ' (value chosen by tests)
*/
#define PAUSEADJ		100


/*
** 'makewhite' erases all color bits then sets only the current white
** bit
*/
#define maskcolors	(~(bitmask(BLACKBIT) | WHITEBITS))  // 000B
#define makewhite(g,x)	\
 (x->marked = cast_byte((x->marked & maskcolors) | luaC_white(g)))

#define white2gray(x)	resetbits(x->marked, WHITEBITS) // 01B -> 00B; 10B -> 00B
#define black2gray(x)	resetbit(x->marked, BLACKBIT)


#define valiswhite(x)   (iscollectable(x) && iswhite(gcvalue(x)))

#define checkdeadkey(n)	lua_assert(!ttisdeadkey(gkey(n)) || ttisnil(gval(n)))


#define checkconsistency(obj)  \
  lua_longassert(!iscollectable(obj) || righttt(obj))


// value 有可能不是 GCObject ，这几加了个判断
#define markvalue(g,o) { checkconsistency(o); \
  if (valiswhite(o)) reallymarkobject(g,gcvalue(o)); }

#define markobject(g,t)	{ if (iswhite(t)) reallymarkobject(g, obj2gco(t)); }

/*
** mark an object that can be NULL (either because it is really optional,
** or it was stripped as debug info, or inside an uncompleted structure)
*/
#define markobjectN(g,t)	{ if (t) markobject(g,t); }

static void reallymarkobject (global_State *g, GCObject *o);


/*
** {======================================================
** Generic functions
** =======================================================
*/


/*
** one after last element in a hash array
*/
#define gnodelast(h)	gnode(h, cast(size_t, sizenode(h)))


/*
** link collectable object 'o' into list pointed by 'p'
*/
#define linkgclist(o,p)	((o)->gclist = (p), (p) = obj2gco(o))


/*
** If key is not marked, mark its entry as dead. This allows key to be
** collected, but keeps its entry in the table.  A dead node is needed
** when Lua looks up for a key (it may be part of a chain) and when
** traversing a weak table (key might be removed from the table during
** traversal). Other places never manipulate dead keys, because its
** associated nil value is enough to signal that the entry is logically
** empty.
*/
static void removeentry (Node *n) {
  lua_assert(ttisnil(gval(n)));
  if (valiswhite(gkey(n)))
    setdeadvalue(wgkey(n));  /* unused and unmarked key; remove it */
}


/*
** tells whether a key or value can be cleared from a weak
** table. Non-collectable objects are never removed from weak
** tables. Strings behave as 'values', so are never removed too. for
** other objects: if really collected, cannot keep them; for objects
** being finalized, keep them in keys, but not in values
*/
static int iscleared (global_State *g, const TValue *o) {
  if (!iscollectable(o)) return 0;
  else if (ttisstring(o)) {
    markobject(g, tsvalue(o));  /* strings are 'values', so are never weak */
    return 0;
  }
  // 好像也就是五种有 gclist 的 GCObject 对象
  else return iswhite(gcvalue(o));
}


/*
** barrier that moves collector forward, that is, mark the white object
** being pointed by a black object. (If in sweep phase, clear the black
** object to white [sweep it] to avoid other barrier calls for this
** same object.)
*/
void luaC_barrier_ (lua_State *L, GCObject *o, GCObject *v) {
  global_State *g = G(L);
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  if (keepinvariant(g))  /* must keep invariant? */
    reallymarkobject(g, v);  /* restore invariant */
  else {  /* sweep phase */
    lua_assert(issweepphase(g));
    makewhite(g, o);  /* mark main obj. as white to avoid other barriers */
  }
}


/*
** barrier that moves collector backward, that is, mark the black object
** pointing to a white object as gray again.
*/
void luaC_barrierback_ (lua_State *L, Table *t) {
  global_State *g = G(L);
  lua_assert(isblack(t) && !isdead(g, t));
  black2gray(t);  /* make table gray (again) */
  linkgclist(t, g->grayagain);
}


/*
** barrier for assignments to closed upvalues. Because upvalues are
** shared among closures, it is impossible to know the color of all
** closures pointing to it. So, we assume that the object being assigned
** must be marked.
*/
void luaC_upvalbarrier_ (lua_State *L, UpVal *uv) {
  global_State *g = G(L);
  GCObject *o = gcvalue(uv->v);
  lua_assert(!upisopen(uv));  /* ensured by macro luaC_upvalbarrier */
  if (keepinvariant(g))
    markobject(g, o);
}


void luaC_fix (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  lua_assert(g->allgc == o);  /* object must be 1st in 'allgc' list! */
  white2gray(o);  /* they will be gray forever */
  g->allgc = o->next;  /* remove object from 'allgc' list */
  o->next = g->fixedgc;  /* link it to 'fixedgc' list */
  g->fixedgc = o;
}


/*
** create a new collectable object (with given type and size) and link
** it to 'allgc' list.
*/
GCObject *luaC_newobj (lua_State *L, int tt, size_t sz) {
  global_State *g = G(L);
  // novariant(tt) 暂时没用，大概是想在 l_alloc 那边统计不同类型的 newobj 频率吧
  GCObject *o = cast(GCObject *, luaM_newobject(L, novariant(tt), sz));
  o->marked = luaC_white(g);
  o->tt = tt;
  o->next = g->allgc;
  g->allgc = o;
  return o;
}

/* }====================================================== */



/*
** {======================================================
** Mark functions
** =======================================================
*/


/*
** mark an object. Userdata, strings, and closed upvalues are visited
** and turned black here. Other objects are marked gray and added
** to appropriate list to be visited (and turned black) later. (Open
** upvalues are already linked in 'headuv' list.)
*/

/**
 * white -> gray
 * 对象o已访问，至少要标记为灰色。
 * 如果是字符串，标记为黑色返回了。
 * 如果是UserData，深度遍历其元表，再标记黑色返回。
 * 如果是其他类型，考虑到水很深，就记到 gray 链表返回：
 *  Lua Closure   闭包的内容很多：参数，局部变量，upvalue
 *  C Closure     
 *  Table         表的内容很复杂
 *  Thread        又是一个虚拟机
 *  Proto         局部函数
 * 
 * gray 链表是通过 gclist 指针把对象串起来的。
 * 
 * global_State.allgc -> o   .next-> o   .next-> o   .next-> o
 * global_State.gray  ->   .gclist->   .gclist->   .gclist->
 * 
 * 这个 gclist 设计好搓，难道不应该定义在 CommonHeader 里么？:)
 * 好像又只在上述的【其他类型】中才有，这或许就是所谓的【容器对象】吧。
 * Proto里还有Proto，Table里面也可以有Table，要考虑【循环引用】的问题。
 * 
*/
static void reallymarkobject (global_State *g, GCObject *o) {
 reentry:
  white2gray(o);
  switch (o->tt) {
    case LUA_TSHRSTR: {
      gray2black(o);
      g->GCmemtrav += sizelstring(gco2ts(o)->shrlen);
      break;
    }
    case LUA_TLNGSTR: {
      gray2black(o);
      g->GCmemtrav += sizelstring(gco2ts(o)->u.lnglen);
      break;
    }
    case LUA_TUSERDATA: {
      TValue uvalue;
      markobjectN(g, gco2u(o)->metatable);  /* mark its metatable */
      gray2black(o);

      g->GCmemtrav += sizeudata(gco2u(o));
      getuservalue(g->mainthread, gco2u(o), &uvalue);
      if (valiswhite(&uvalue)) {  /* markvalue(g, &uvalue); */
        o = gcvalue(&uvalue);
        goto reentry;
      }
      break;
    }
    case LUA_TLCL: {
      linkgclist(gco2lcl(o), g->gray);
      break;
    }
    case LUA_TCCL: {
      linkgclist(gco2ccl(o), g->gray);
      break;
    }
    case LUA_TTABLE: {
      linkgclist(gco2t(o), g->gray);
      break;
    }
    case LUA_TTHREAD: {
      linkgclist(gco2th(o), g->gray);
      break;
    }
    case LUA_TPROTO: {
      linkgclist(gco2p(o), g->gray);
      break;
    }
    default: lua_assert(0); break;
  }
}


/*
** mark metamethods for basic types
*/
static void markmt (global_State *g) {
  int i;
  for (i=0; i < LUA_NUMTAGS; i++)
    markobjectN(g, g->mt[i]);
}


/*
** mark all objects in list of being-finalized
*/
static void markbeingfnz (global_State *g) {
  GCObject *o;
  for (o = g->tobefnz; o != NULL; o = o->next)
    markobject(g, o);
}


/*
** Mark all values stored in marked open upvalues from non-marked threads.
** (Values from marked threads were already marked when traversing the
** thread.) Remove from the list threads that no longer have upvalues and
** not-marked threads.
*/
static void remarkupvals (global_State *g) {
  lua_State *thread;
  lua_State **p = &g->twups;

  while ((thread = *p) != NULL) {
    lua_assert(!isblack(thread));  /* threads are never black */

    if (isgray(thread) && thread->openupval != NULL)
      // 没有破坏原有 twups 链
      p = &thread->twups;  /* keep marked thread with upvalues in the list */

    else {  /* thread is not marked or without upvalues */
      UpVal *uv;
      // twups 链中删掉一个元素
      *p = thread->twups;  /* remove thread from the list */
      // 让它引用自己， luaF_findupval 就不会把它链回去
      thread->twups = thread;  /* mark that it is out of list */

      for (uv = thread->openupval; uv != NULL; uv = uv->u.open.next) {
        if (uv->u.open.touched) {
          markvalue(g, uv->v);  /* remark upvalue's value */
          uv->u.open.touched = 0;
        }
      }
    }
  }
}


/*
** mark root set and reset all gray lists, to start a new collection
*/
/**
 * 初始化新一轮GC
 * 4个root对象，显而易见：
 *  主线程对象
 *  注册表
 *  基本类型的元表
 *  being-finalized 对象（上一轮GC遗留的对象）
*/
static void restartcollection (global_State *g) {
  g->gray = g->grayagain = NULL;
  g->weak = g->allweak = g->ephemeron = NULL;
  
  markobject(g, g->mainthread);
  markvalue(g, &g->l_registry);
  markmt(g);

  markbeingfnz(g);              /* mark any finalizing object left from previous cycle */
}

/* }====================================================== */


/*
** {======================================================
** Traverse functions
** =======================================================
*/

/*
** Traverse a table with weak values and link it to proper list. During
** propagate phase, keep it in 'grayagain' list, to be revisited in the
** atomic phase. In the atomic phase, if table has any white value,
** put it in 'weak' list, to be cleared.
*/
static void traverseweakvalue (global_State *g, Table *h) {
  Node *n, *limit = gnodelast(h);
  /* if there is array part, assume it may have white values (it is not
     worth traversing it now just to check) */
  int hasclears = (h->sizearray > 0);
  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    checkdeadkey(n);
    if (ttisnil(gval(n)))  /* entry is empty? */
      removeentry(n);  /* remove it */
    else {
      lua_assert(!ttisnil(gkey(n)));
      markvalue(g, gkey(n));  /* mark key */
      if (!hasclears && iscleared(g, gval(n)))  /* is there a white value? */
        hasclears = 1;  /* table will have to be cleared */
    }
  }
  if (g->gcstate == GCSpropagate)
    linkgclist(h, g->grayagain);  /* must retraverse it in atomic phase */
  else if (hasclears)
    linkgclist(h, g->weak);  /* has to be cleared later */
}


/*
** Traverse an ephemeron table and link it to proper list. Returns true
** iff any object was marked during this traversal (which implies that
** convergence has to continue). During propagation phase, keep table
** in 'grayagain' list, to be visited again in the atomic phase. In
** the atomic phase, if table has any white->white entry, it has to
** be revisited during ephemeron convergence (as that key may turn
** black). Otherwise, if it has any white key, table has to be cleared
** (in the atomic phase).
*/
static int traverseephemeron (global_State *g, Table *h) {
  int marked = 0;  /* true if an object is marked in this traversal */
  int hasclears = 0;  /* true if table has white keys */
  int hasww = 0;  /* true if table has entry "white-key -> white-value" */
  Node *n, *limit = gnodelast(h);
  unsigned int i;
  /* traverse array part */
  for (i = 0; i < h->sizearray; i++) {
    if (valiswhite(&h->array[i])) {
      marked = 1;
      reallymarkobject(g, gcvalue(&h->array[i]));
    }
  }
  /* traverse hash part */
  for (n = gnode(h, 0); n < limit; n++) {
    checkdeadkey(n);
    if (ttisnil(gval(n)))  /* entry is empty? */
      removeentry(n);  /* remove it */
    else if (iscleared(g, gkey(n))) {  /* key is not marked (yet)? */
      hasclears = 1;  /* table must be cleared */
      if (valiswhite(gval(n)))  /* value not marked yet? */
        hasww = 1;  /* white-white entry */
    }
    else if (valiswhite(gval(n))) {  /* value not marked yet? */
      marked = 1;
      reallymarkobject(g, gcvalue(gval(n)));  /* mark it now */
    }
  }
  /* link table into proper list */
  if (g->gcstate == GCSpropagate)
    linkgclist(h, g->grayagain);  /* must retraverse it in atomic phase */
  else if (hasww)  /* table has white->white entries? */
    linkgclist(h, g->ephemeron);  /* have to propagate again */
  else if (hasclears)  /* table has white keys? */
    linkgclist(h, g->allweak);  /* may have to clean white keys */
  return marked;
}


static void traversestrongtable (global_State *g, Table *h) {
  Node *n, *limit = gnodelast(h);
  unsigned int i;
  for (i = 0; i < h->sizearray; i++)  /* traverse array part */
    markvalue(g, &h->array[i]);

  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    checkdeadkey(n);
    if (ttisnil(gval(n)))  /* entry is empty? */
      removeentry(n);  /* remove it 有点草率把？？？如果 key 是个被复用的字符串呢？？？ */
    else {
      lua_assert(!ttisnil(gkey(n)));
      markvalue(g, gkey(n));  /* mark key */
      markvalue(g, gval(n));  /* mark value */
    }
  }
}


static lu_mem traversetable (global_State *g, Table *h) {
  const char *weakkey, *weakvalue;
  const TValue *mode = gfasttm(g, h->metatable, TM_MODE);
  markobjectN(g, h->metatable);

  // 虚表判断 __mode = 'k'/'v'/'kv'
  if (mode && ttisstring(mode) &&  /* is there a weak mode? */
      ((weakkey = strchr(svalue(mode), 'k')),
       (weakvalue = strchr(svalue(mode), 'v')),
       (weakkey || weakvalue))) {  /* is really weak? */
    
    black2gray(h);  /* keep table gray */

    if (!weakkey)  /* strong keys? */
      traverseweakvalue(g, h);
    else if (!weakvalue)  /* strong values? */
      traverseephemeron(g, h);
    else  /* all weak */
      linkgclist(h, g->allweak);  /* nothing to traverse now */
  }
  else  /* not weak */
    traversestrongtable(g, h);
  return sizeof(Table) + sizeof(TValue) * h->sizearray +
                         sizeof(Node) * cast(size_t, allocsizenode(h));
}


/*
** Traverse a prototype. (While a prototype is being build, its
** arrays can be larger than needed; the extra slots are filled with
** NULL, so the use of 'markobjectN')
*/
static int traverseproto (global_State *g, Proto *f) {
  int i;
  if (f->cache && iswhite(f->cache))
    f->cache = NULL;  /* allow cache to be collected */
  markobjectN(g, f->source);
  for (i = 0; i < f->sizek; i++)  /* mark literals */
    markvalue(g, &f->k[i]);
  for (i = 0; i < f->sizeupvalues; i++)  /* mark upvalue names */
    markobjectN(g, f->upvalues[i].name);
  for (i = 0; i < f->sizep; i++)  /* mark nested protos */
    markobjectN(g, f->p[i]);
  for (i = 0; i < f->sizelocvars; i++)  /* mark local-variable names */
    markobjectN(g, f->locvars[i].varname);
  return sizeof(Proto) + sizeof(Instruction) * f->sizecode +
                         sizeof(Proto *) * f->sizep +
                         sizeof(TValue) * f->sizek +
                         sizeof(int) * f->sizelineinfo +
                         sizeof(LocVar) * f->sizelocvars +
                         sizeof(Upvaldesc) * f->sizeupvalues;
}


static lu_mem traverseCclosure (global_State *g, CClosure *cl) {
  int i;
  for (i = 0; i < cl->nupvalues; i++)  /* mark its upvalues */
    markvalue(g, &cl->upvalue[i]);
  return sizeCclosure(cl->nupvalues);
}

/*
** open upvalues point to values in a thread, so those values should
** be marked when the thread is traversed except in the atomic phase
** (because then the value cannot be changed by the thread and the
** thread may not be traversed again)
*/
static lu_mem traverseLclosure (global_State *g, LClosure *cl) {
  int i;
  markobjectN(g, cl->p);  /* mark its prototype */
  for (i = 0; i < cl->nupvalues; i++) {  /* mark its upvalues */
    UpVal *uv = cl->upvals[i];
    if (uv != NULL) {
      if (upisopen(uv) && g->gcstate != GCSinsideatomic)
        uv->u.open.touched = 1;  /* can be marked in 'remarkupvals' */
      else
        markvalue(g, uv->v);
    }
  }
  return sizeLclosure(cl->nupvalues);
}


static lu_mem traversethread (global_State *g, lua_State *th) {
  StkId o = th->stack;
  if (o == NULL)
    return 1;  /* stack not completely built yet */
  lua_assert(g->gcstate == GCSinsideatomic ||
             th->openupval == NULL || isintwups(th));

  for (; o < th->top; o++)  /* mark live elements in the stack */
    markvalue(g, o);

  if (g->gcstate == GCSinsideatomic) {  /* final traversal? */
    StkId lim = th->stack + th->stacksize;  /* real end of stack */
    for (; o < lim; o++)  /* clear not-marked stack slice */
      setnilvalue(o);

    /* 'remarkupvals' may have removed thread from 'twups' list */
    if (!isintwups(th) && th->openupval != NULL) {
      // thread 链到 glbal_State 上就表示 intwups(th) 了
      th->twups = g->twups;  /* link it back to the list */
      g->twups = th;
    }
  }
  else if (g->gckind != KGC_EMERGENCY)
    luaD_shrinkstack(th); /* do not change stack in emergency cycle */
  return (sizeof(lua_State) + sizeof(TValue) * th->stacksize +
          sizeof(CallInfo) * th->nci);
}


/**
 * gray -> black
 * 和 reallymarkobject 不一样的是，初始颜色是灰色。
 * 
** traverse one gray object, turning it to black (except for threads,
** which are always gray).
*/
static void propagatemark (global_State *g) {
  lu_mem size;
  GCObject *o = g->gray;
  lua_assert(isgray(o));
  gray2black(o);
  switch (o->tt) {
    case LUA_TTABLE: {
      Table *h = gco2t(o);
      g->gray = h->gclist;  /* remove from 'gray' list */
      size = traversetable(g, h);
      break;
    }
    case LUA_TLCL: {
      LClosure *cl = gco2lcl(o);
      g->gray = cl->gclist;  /* remove from 'gray' list */
      size = traverseLclosure(g, cl);
      break;
    }
    case LUA_TCCL: {
      CClosure *cl = gco2ccl(o);
      g->gray = cl->gclist;  /* remove from 'gray' list */
      size = traverseCclosure(g, cl);
      break;
    }
    case LUA_TTHREAD: {
      lua_State *th = gco2th(o);
      g->gray = th->gclist;  /* remove from 'gray' list */
      linkgclist(th, g->grayagain);  /* insert into 'grayagain' list */
      black2gray(o);
      size = traversethread(g, th);
      break;
    }
    case LUA_TPROTO: {
      Proto *p = gco2p(o);
      g->gray = p->gclist;  /* remove from 'gray' list */
      size = traverseproto(g, p);
      break;
    }
    default: lua_assert(0); return;
  }
  g->GCmemtrav += size;
}


static void propagateall (global_State *g) {
  while (g->gray) propagatemark(g);
}


static void convergeephemerons (global_State *g) {
  int changed;
  do {
    GCObject *w;
    GCObject *next = g->ephemeron;  /* get ephemeron list */
    g->ephemeron = NULL;  /* tables may return to this list when traversed */
    changed = 0;
    while ((w = next) != NULL) {
      next = gco2t(w)->gclist;
      if (traverseephemeron(g, gco2t(w))) {  /* traverse marked some value? */
        propagateall(g);  /* propagate changes */
        changed = 1;  /* will have to revisit all ephemeron tables */
      }
    }
  } while (changed);
}

/* }====================================================== */


/*
** {======================================================
** Sweep Functions
** =======================================================
*/


/*
** clear entries with unmarked keys from all weaktables in list 'l' up
** to element 'f'
*/
static void clearkeys (global_State *g, GCObject *l, GCObject *f) {
  for (; l != f; l = gco2t(l)->gclist) {
    Table *h = gco2t(l);
    Node *n, *limit = gnodelast(h);
    for (n = gnode(h, 0); n < limit; n++) {
      if (!ttisnil(gval(n)) && (iscleared(g, gkey(n)))) {
        setnilvalue(gval(n));  /* remove value ... */
      }
      if (ttisnil(gval(n)))  /* is entry empty? */
        removeentry(n);  /* remove entry from table */
    }
  }
}


/*
** clear entries with unmarked values from all weaktables in list 'l' up
** to element 'f'
*/
static void clearvalues (global_State *g, GCObject *l, GCObject *f) {
  for (; l != f; l = gco2t(l)->gclist) {
    Table *h = gco2t(l);
    Node *n, *limit = gnodelast(h);
    unsigned int i;
    for (i = 0; i < h->sizearray; i++) {
      TValue *o = &h->array[i];
      if (iscleared(g, o))  /* value was collected? */
        setnilvalue(o);  /* remove value */
    }
    for (n = gnode(h, 0); n < limit; n++) {
      if (!ttisnil(gval(n)) && iscleared(g, gval(n))) {
        setnilvalue(gval(n));  /* remove value ... */
        removeentry(n);  /* and remove entry from table */
      }
    }
  }
}


void luaC_upvdeccount (lua_State *L, UpVal *uv) {
  lua_assert(uv->refcount > 0);
  uv->refcount--;
  if (uv->refcount == 0 && !upisopen(uv))
    luaM_free(L, uv);
}


static void freeLclosure (lua_State *L, LClosure *cl) {
  int i;
  for (i = 0; i < cl->nupvalues; i++) {
    UpVal *uv = cl->upvals[i];
    if (uv)
      luaC_upvdeccount(L, uv);
  }
  luaM_freemem(L, cl, sizeLclosure(cl->nupvalues));
}

/**
 * 释放对象，最终调用到 l_alloc -> free
 * 是真的释放了，没有用 freelist
*/
static void freeobj (lua_State *L, GCObject *o) {
  switch (o->tt) {
    case LUA_TPROTO: luaF_freeproto(L, gco2p(o)); break;
    case LUA_TLCL: {
      freeLclosure(L, gco2lcl(o));
      break;
    }
    case LUA_TCCL: {
      luaM_freemem(L, o, sizeCclosure(gco2ccl(o)->nupvalues));
      break;
    }
    case LUA_TTABLE: luaH_free(L, gco2t(o)); break;
    case LUA_TTHREAD: luaE_freethread(L, gco2th(o)); break;
    case LUA_TUSERDATA: luaM_freemem(L, o, sizeudata(gco2u(o))); break;
    case LUA_TSHRSTR:
      luaS_remove(L, gco2ts(o));  /* remove it from hash table */
      luaM_freemem(L, o, sizelstring(gco2ts(o)->shrlen));
      break;
    case LUA_TLNGSTR: {
      luaM_freemem(L, o, sizelstring(gco2ts(o)->u.lnglen));
      break;
    }
    default: lua_assert(0);
  }
}


#define sweepwholelist(L,p)	sweeplist(L,p,MAX_LUMEM)
static GCObject **sweeplist (lua_State *L, GCObject **p, lu_mem count);


/*
** sweep at most 'count' elements from a list of GCObjects erasing dead
** objects, where a dead object is one marked with the old (non current)
** white; change all non-dead objects back to white, preparing for next
** collection cycle. Return where to continue the traversal or NULL if
** list is finished.
*/
static GCObject **sweeplist (lua_State *L, GCObject **p, lu_mem count) {
  global_State *g = G(L);
  int ow = otherwhite(g);     // old white，之 atomic 切换过白色了，所以这个 ow 就是垃圾白色
  int white = luaC_white(g);  /* current white */

  while (*p != NULL && count-- > 0) {
    GCObject *curr = *p;
    int marked = curr->marked;
    if (isdeadm(ow, marked)) {  /* is 'curr' dead? */
      *p = curr->next;  /* remove 'curr' from list */
      freeobj(L, curr);  /* erase 'curr' */
    }
    else {  /* change mark to 'white' */
      curr->marked = cast_byte((marked & maskcolors) | white);    // 清除黑色标记，并标记为当前白色
      p = &curr->next;  /* go to next element */
    }
  }
  return (*p == NULL) ? NULL : p;
}


/*
** sweep a list until a live object (or end of list)
*/
static GCObject **sweeptolive (lua_State *L, GCObject **p) {
  GCObject **old = p;
  do {
    p = sweeplist(L, p, 1);
  } while (p == old);
  return p;
}

/* }====================================================== */


/*
** {======================================================
** Finalization
** =======================================================
*/

/*
** If possible, shrink string table
*/
static void checkSizes (lua_State *L, global_State *g) {
  if (g->gckind != KGC_EMERGENCY) {
    l_mem olddebt = g->GCdebt;
    if (g->strt.nuse < g->strt.size / 4)  /* string table too big? */
      luaS_resize(L, g->strt.size / 2);  /* shrink it a little */
    g->GCestimate += g->GCdebt - olddebt;  /* update estimate */
  }
}


static GCObject *udata2finalize (global_State *g) {
  GCObject *o = g->tobefnz;  /* get first element */
  lua_assert(tofinalize(o));
  // tobefnz 会清空
  g->tobefnz = o->next;  /* remove it from 'tobefnz' list */
  o->next = g->allgc;  /* return it to 'allgc' list */
  g->allgc = o;
  // 清除标记，这样就是可回收对象了
  resetbit(o->marked, FINALIZEDBIT);  /* object is "normal" again */
  if (issweepphase(g))
    makewhite(g, o);  /* "sweep" object */
  return o;
}


static void dothecall (lua_State *L, void *ud) {
  UNUSED(ud);
  luaD_callnoyield(L, L->top - 2, 0);
}


static void GCTM (lua_State *L, int propagateerrors) {
  global_State *g = G(L);
  const TValue *tm;
  TValue v;
  setgcovalue(L, &v, udata2finalize(g));
  tm = luaT_gettmbyobj(L, &v, TM_GC);     // __gc

  if (tm != NULL && ttisfunction(tm)) {  /* is there a finalizer? */
    int status;
    lu_byte oldah = L->allowhook;
    int running  = g->gcrunning;
    L->allowhook = 0;  /* stop debug hooks during GC metamethod */
    g->gcrunning = 0;  /* avoid GC steps */
    
    setobj2s(L, L->top, tm);  /* push finalizer... */
    setobj2s(L, L->top + 1, &v);  /* ... and its argument */
    L->top += 2;  /* and (next line) call the finalizer */
    L->ci->callstatus |= CIST_FIN;  /* will run a finalizer */
    status = luaD_pcall(L, dothecall, NULL, savestack(L, L->top - 2), 0);
    L->ci->callstatus &= ~CIST_FIN;  /* not running a finalizer anymore */
    L->allowhook = oldah;  /* restore hooks */

    g->gcrunning = running;  /* restore state */
    if (status != LUA_OK && propagateerrors) {  /* error while running __gc? */
      if (status == LUA_ERRRUN) {  /* is there an error object? */
        const char *msg = (ttisstring(L->top - 1))
                            ? svalue(L->top - 1)
                            : "no message";
        luaO_pushfstring(L, "error in __gc metamethod (%s)", msg);
        status = LUA_ERRGCMM;  /* error in __gc metamethod */
      }
      luaD_throw(L, status);  /* re-throw error */
    }
  }
}


/*
** call a few (up to 'g->gcfinnum') finalizers
*/
static int runafewfinalizers (lua_State *L) {
  global_State *g = G(L);
  unsigned int i;
  lua_assert(!g->tobefnz || g->gcfinnum > 0);
  for (i = 0; g->tobefnz && i < g->gcfinnum; i++)
    GCTM(L, 1);  /* call one finalizer */
  g->gcfinnum = (!g->tobefnz) ? 0  /* nothing more to finalize? */
                    : g->gcfinnum * 2;  /* else call a few more next time */
  return i;
}


/*
** call all pending finalizers
*/
static void callallpendingfinalizers (lua_State *L) {
  global_State *g = G(L);
  while (g->tobefnz)
    GCTM(L, 0);
}


/*
** find last 'next' field in list 'p' list (to add elements in its end)
*/
static GCObject **findlast (GCObject **p) {
  while (*p != NULL)
    p = &(*p)->next;
  return p;
}


/**
 * 遍历 finobj 链表，将白色对象移动到 tobefnz 链表的末端（保证顺序）
** move all unreachable objects (or 'all' objects) that need
** finalization from list 'finobj' to list 'tobefnz' (to be finalized)
*/
static void separatetobefnz (global_State *g, int all) {
  GCObject *curr;
  GCObject **p = &g->finobj;
  GCObject **lastnext = findlast(&g->tobefnz);
  while ((curr = *p) != NULL) {  /* traverse all finalizable objects */
    lua_assert(tofinalize(curr));
    if (!(iswhite(curr) || all))  /* not being collected? */
      p = &curr->next;  /* don't bother with it */
    else {
      *p = curr->next;  /* remove 'curr' from 'finobj' list */
      curr->next = *lastnext;  /* link at the end of 'tobefnz' list */
      *lastnext = curr;
      lastnext = &curr->next;
    }
  }
}


/*
** if object 'o' has a finalizer, remove it from 'allgc' list (must
** search the list to find it) and link it in 'finobj' list.
*/
void luaC_checkfinalizer (lua_State *L, GCObject *o, Table *mt) {
  global_State *g = G(L);
  if (tofinalize(o) ||                 /* obj. is already marked... */
      gfasttm(g, mt, TM_GC) == NULL)   /* or has no finalizer? */
    return;  /* nothing to be done */
  else {  /* move 'o' to 'finobj' list */
    GCObject **p;
    if (issweepphase(g)) {
      makewhite(g, o);  /* "sweep" object 'o' */
      if (g->sweepgc == &o->next)  /* should not remove 'sweepgc' object */
        g->sweepgc = sweeptolive(L, g->sweepgc);  /* change 'sweepgc' */
    }

    // 拥有 __gc 的对象会从 allgc 链表移除，放到 finobj 链表中，并标记特殊类型
    /* search for pointer pointing to 'o' */
    for (p = &g->allgc; *p != o; p = &(*p)->next) { /* empty */ }
    *p = o->next;  /* remove 'o' from 'allgc' list */
    o->next = g->finobj;  /* link it in 'finobj' list */
    g->finobj = o;
    l_setbit(o->marked, FINALIZEDBIT);  /* mark it as such */
  }
}

/* }====================================================== */



/*
** {======================================================
** GC control
** =======================================================
*/


/*
** Set a reasonable "time" to wait before starting a new GC cycle; cycle
** will start when memory use hits threshold. (Division by 'estimate'
** should be OK: it cannot be zero (because Lua cannot even start with
** less than PAUSEADJ bytes).
*/
static void setpause (global_State *g) {
  l_mem threshold, debt;
  l_mem estimate = g->GCestimate / PAUSEADJ;  /* adjust 'estimate' */
  lua_assert(estimate > 0);
  threshold = (g->gcpause < MAX_LMEM / estimate)  /* overflow? */
            ? estimate * g->gcpause  /* no overflow */
            : MAX_LMEM;  /* overflow; truncate to maximum */
  debt = gettotalbytes(g) - threshold;
  luaE_setdebt(g, debt);
}


/*
** Enter first sweep phase.
** The call to 'sweeplist' tries to make pointer point to an object
** inside the list (instead of to the header), so that the real sweep do
** not need to skip objects created between "now" and the start of the
** real sweep.
*/
static void entersweep (lua_State *L) {
  global_State *g = G(L);
  g->gcstate = GCSswpallgc;
  lua_assert(g->sweepgc == NULL);
  // 从 allgc 开始进行回收了，allgc 并没有因此清零，反而让 gc->sweepgc 也指向它
  g->sweepgc = sweeplist(L, &g->allgc, 1);
}


void luaC_freeallobjects (lua_State *L) {
  global_State *g = G(L);
  separatetobefnz(g, 1);  /* separate all objects with finalizers */
  lua_assert(g->finobj == NULL);
  callallpendingfinalizers(L);
  lua_assert(g->tobefnz == NULL);
  g->currentwhite = WHITEBITS; /* 11B this "white" makes all objects look dead */
  g->gckind = KGC_NORMAL;
  sweepwholelist(L, &g->finobj);
  sweepwholelist(L, &g->allgc);
  sweepwholelist(L, &g->fixedgc);  /* collect fixed objects */
  lua_assert(g->strt.nuse == 0);
}

static l_mem atomic (lua_State *L) {
  global_State *g = G(L);
  l_mem work;
  GCObject *origweak, *origall;
  GCObject *grayagain = g->grayagain;  /* save original list */
  lua_assert(g->ephemeron == NULL && g->weak == NULL);
  lua_assert(!iswhite(g->mainthread));
  
  g->gcstate = GCSinsideatomic;
  g->GCmemtrav = 0;  /* start counting work */

  // 1. 重新标记 root 对象
  markobject(g, L);  /* mark running thread */
  /* registry and global metatables may be changed by API */
  markvalue(g, &g->l_registry);
  markmt(g);  /* mark global metatables */

  // 2. 标记所有 thread 的 openupval 中的元素
  /* remark occasional upvalues of (maybe) dead threads */
  remarkupvals(g);
  
  // 3. 标记完 gray 链
  propagateall(g);  /* propagate changes */
  work = g->GCmemtrav;  /* stop counting (do not recount 'grayagain') */

  // 4. 标记完 grayagain 链
  g->gray = grayagain;  // grayagain 都是遍历过的，所以不用统计到 work
  propagateall(g);  /* traverse 'grayagain' list grayagain 链遍历完 */

  g->GCmemtrav = 0;  /* restart counting */
  // 5. 标记 ephemerons 链，直到 ephemerons/gray 都为NULL，但是会更新 grayagain 和 allweak
  convergeephemerons(g);

  // 6. 清理 weak 表 和 allweak 表中的数组和散列表，将白色容器类元素设成 nil
  /* at this point, all strongly accessible objects are marked. */
  /* Clear values from weak tables, before checking finalizers */
  clearvalues(g, g->weak, NULL);
  clearvalues(g, g->allweak, NULL);
  origweak = g->weak; origall = g->allweak;
  work += g->GCmemtrav;  /* stop counting (objects being finalized) */

  // 7. 遍历 finobj 链表，将白色对象移动到 tobefnz 链表的末端（保证顺序）
  separatetobefnz(g, 0);  /* separate objects to be finalized */
  g->gcfinnum = 1;  /* there may be objects to be finalized */

  // 8. being-finalized 链表的对象都算是可达的（毕竟虚拟机要执行 __gc）
  markbeingfnz(g);  /* mark objects that will be finalized */
  propagateall(g);  /* remark, to propagate 'resurrection' */
  g->GCmemtrav = 0;  /* restart counting */
  
  // 9. 标记 ephemerons 链，直到 ephemerons/gray 都为NULL，但是会更新 grayagain 和 allweak
  convergeephemerons(g);

  // 10. 清理4个 weak 表，将其中的白色容器类元素都设成 nil
  /* at this point, all resurrected objects are marked. */
  /* remove dead objects from weak tables */
  clearkeys(g, g->ephemeron, NULL);  /* clear keys from all ephemeron tables */
  clearkeys(g, g->allweak, NULL);  /* clear keys from all 'allweak' tables */
  /* clear values from resurrected weak tables */
  clearvalues(g, g->weak, origweak);
  clearvalues(g, g->allweak, origall);
  
  // 11. 清掉 API 的字符串缓存
  luaS_clearcache(g);

  // 12. 切换白色
  g->currentwhite = cast_byte(otherwhite(g));  /* flip current white */
  work += g->GCmemtrav;  /* complete counting */
  return work;  /* estimate of memory marked by 'atomic' */
}


static lu_mem sweepstep (lua_State *L, global_State *g,
                         int nextstate, GCObject **nextlist) {
  if (g->sweepgc) {
    l_mem olddebt = g->GCdebt;
    // 将 g->sweepgc 再 sweep 回收一次，这次指定了最大回收数量
    g->sweepgc = sweeplist(L, g->sweepgc, GCSWEEPMAX);
    g->GCestimate += g->GCdebt - olddebt;  /* update estimate */
    if (g->sweepgc)  /* is there still something to sweep? */
      return (GCSWEEPMAX * GCSWEEPCOST);
  }
  /* else enter next state */
  g->gcstate = nextstate;
  g->sweepgc = nextlist;
  return 0;
}

/**
 * 垃圾收集器执行单独的一步
 * switch + g->gcstate 改变，我们可以整理出各GC步骤之间的关系
 * 
 * GCSpause -> GCSpropagate     ： 新一轮GC的初始化。
 * GCSpropagate -> GCSatomic    ： 完成了标记阶段，针对 gray 链表。【多段操作】
 * GCSatomic -> GCSswpallgc     ： 完成了原子操作，之后 sweep 了一次 allgc。 entersweep 封装是为了 fullgc 调用。回收完成后，剩下的对象 链入sweepgc
 * GCSswpallgc -> GCSswpfinobj  ： 将 sweepgc 再回收一次【多段操作】，如果空了，就指向 finobj
 * GCSswpfinobj -> GCSswptobefnz： 将 sweepgc 再回收一次【多段操作】，如果空了，就指向 tobefnz
 * GCSswptobefnz -> GCSswpend   ： 将 sweepgc 再回收一次【多段操作】
 * GCSswpend -> GCScallfin      ： 将 mainthread 标记为当前白色，对全局字符串缓存表进行 resize
 * GCScallfin -> GCSpause       ： 对 tobefnz 链表中的对象执行 __gc
*/
static lu_mem singlestep (lua_State *L) {
  global_State *g = G(L);
  switch (g->gcstate) {
    case GCSpause: {
      // 全局的字符串缓存表的内存，其实不需要遍历。它只是 *TString[] 指针数组，并不是 GCObject。
      g->GCmemtrav = g->strt.size * sizeof(GCObject*);
      restartcollection(g);
      g->gcstate = GCSpropagate;
      return g->GCmemtrav;
    }
    case GCSpropagate: {
      g->GCmemtrav = 0;
      lua_assert(g->gray);
      propagatemark(g);
       if (g->gray == NULL)  /* no more gray objects? */
        g->gcstate = GCSatomic;  /* finish propagate phase */
      return g->GCmemtrav;  /* memory traversed in this step */
    }
    case GCSatomic: {
      lu_mem work;
      propagateall(g);  /* make sure gray list is empty */
      work = atomic(L);  /* work is what was traversed by 'atomic' */
      entersweep(L);
      g->GCestimate = gettotalbytes(g);  /* first estimate */;
      return work;
    }
    case GCSswpallgc: {  /* sweep "regular" objects */
      return sweepstep(L, g, GCSswpfinobj, &g->finobj);
    }
    case GCSswpfinobj: {  /* sweep objects with finalizers */
      return sweepstep(L, g, GCSswptobefnz, &g->tobefnz);
    }
    case GCSswptobefnz: {  /* sweep objects to be finalized */
      return sweepstep(L, g, GCSswpend, NULL);
    }
    case GCSswpend: {  /* finish sweeps */
      makewhite(g, g->mainthread);  /* sweep main thread */
      checkSizes(L, g);
      g->gcstate = GCScallfin;
      return 0;
    }
    case GCScallfin: {  /* call remaining finalizers */
      if (g->tobefnz && g->gckind != KGC_EMERGENCY) {
        int n = runafewfinalizers(L);
        return (n * GCFINALIZECOST);
      }
      else {  /* emergency mode or no more finalizers */
        g->gcstate = GCSpause;  /* finish collection */
        return 0;
      }
    }
    default: lua_assert(0); return 0;
  }
}


/*
** advances the garbage collector until it reaches a state allowed
** by 'statemask'
*/
void luaC_runtilstate (lua_State *L, int statesmask) {
  global_State *g = G(L);
  while (!testbit(statesmask, g->gcstate))
    singlestep(L);
}


/*
** get GC debt and convert it from Kb to 'work units' (avoid zero debt
** and overflows)
*/
static l_mem getdebt (global_State *g) {
  l_mem debt = g->GCdebt;
  int stepmul = g->gcstepmul;
  if (debt <= 0) return 0;  /* minimal debt */
  else {
    debt = (debt / STEPMULADJ) + 1;
    debt = (debt < MAX_LMEM / stepmul) ? debt * stepmul : MAX_LMEM;
    return debt;
  }
}

/**
 * 常规 GC 的执行
 * luaV_execute -> checkGC -> luaC_condGC -> luaC_step -> singlestep
 * OP_NEWTABLE/OP_CONCAT/OP_CLOSURE
 * 
** performs a basic GC step when collector is running
*/
void luaC_step (lua_State *L) {
  global_State *g = G(L);
  l_mem debt = getdebt(g);  /* GC deficit (be paid now) */
  if (!g->gcrunning) {  /* not running? */
    luaE_setdebt(g, -GCSTEPSIZE * 10);  /* avoid being called too often */
    return;
  }
  do {  /* repeat until pause or enough "credit" (negative debt) */
    lu_mem work = singlestep(L);  /* perform one single step */
    debt -= work;
  } while (debt > -GCSTEPSIZE && g->gcstate != GCSpause);
  if (g->gcstate == GCSpause)
    setpause(g);  /* pause until next cycle */
  else {
    debt = (debt / g->gcstepmul) * STEPMULADJ;  /* convert 'work units' to Kb */
    luaE_setdebt(g, debt);
    runafewfinalizers(L);
  }
}


/*
** Performs a full GC cycle; if 'isemergency', set a flag to avoid
** some operations which could change the interpreter state in some
** unexpected ways (running finalizers and shrinking some structures).
** Before running the collection, check 'keepinvariant'; if it is true,
** there may be some objects marked as black, so the collector has
** to sweep all objects to turn them back to white (as white has not
** changed, nothing will be collected).
*/
void luaC_fullgc (lua_State *L, int isemergency) {
  global_State *g = G(L);
  lua_assert(g->gckind == KGC_NORMAL);
  if (isemergency) g->gckind = KGC_EMERGENCY;  /* set flag */
  if (keepinvariant(g)) {  /* black objects? */
    entersweep(L); /* sweep everything to turn them back to white */
  }
  /* finish any pending sweep phase to start a new cycle */
  // 完成当前这轮GC
  luaC_runtilstate(L, bitmask(GCSpause));

  luaC_runtilstate(L, ~bitmask(GCSpause));  /* start new collection */
  luaC_runtilstate(L, bitmask(GCScallfin));  /* run up to finalizers */
  /* estimate must be correct after a full GC cycle */
  lua_assert(g->GCestimate == gettotalbytes(g));
  luaC_runtilstate(L, bitmask(GCSpause));  /* finish collection */

  g->gckind = KGC_NORMAL;
  setpause(g);
}

/* }====================================================== */


