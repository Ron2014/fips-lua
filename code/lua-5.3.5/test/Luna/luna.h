extern "C" {
    #include "lua.h"
    #include "luaconf.h"
    #include "lualib.h"
}

template <typename T> class Luna {
  typedef struct { T *pT; } userdataType;   // pT 是lua对象 对应的真正的 C++对象
public:
  typedef int (T::*mfp)(lua_State *L);
  typedef struct { const char *name; mfp mfunc; } RegType;

  static void Register(lua_State *L) {
    /** 3 个 table
     * 1. methods 类
     *      实例化函数 methods.new
     *      成员函数 通过 RegType 映射数组生成
     * 2. metatable 对象的元表:
     *      创建对象时 setmetatbale(obj, metatable)
     *      所以这个 metatable.__index = methods
     *      这个表直接放到 registry 里
     * 3. mt 类的元表:
     *      用它实现 methods()
     */
    lua_newtable(L);
    int methods = lua_gettop(L);            // methods = {}
                                            // metatable = {__name = name}
    luaL_newmetatable(L, T::className);     // registry.name = metatable
    int metatable = lua_gettop(L);

    // store method table in globals so that
    // scripts can add functions written in Lua.
    lua_pushvalue(L, methods);
    lua_setglobal(L, T::className);            // _G[name] = methods

    lua_pushliteral(L, "__metatable");
    lua_pushvalue(L, methods);
    lua_settable(L, metatable);  // hide metatable from Lua getmetatable()
    // metatable.__metatable = methods

    lua_pushliteral(L, "__index");
    lua_pushvalue(L, methods);
    lua_settable(L, metatable);
    // metatable.__index = methods

    lua_pushliteral(L, "__tostring");
    lua_pushcfunction(L, tostring_T);
    lua_settable(L, metatable);
    // metatable.__tostring = tostring_T

    lua_pushliteral(L, "__gc");
    lua_pushcfunction(L, gc_T);
    lua_settable(L, metatable);
    // metatable.__gc = gc_T

    // 从类实例化一个对象 __call -> new_T
    lua_newtable(L);                // mt for method table
    int mt = lua_gettop(L);
    lua_pushliteral(L, "__call");
    lua_pushcfunction(L, new_T);

    lua_pushliteral(L, "new");
    lua_pushvalue(L, -2);           // dup new_T function
    lua_settable(L, methods);       // add new_T to method table
                                    // methods.new = new_T

    lua_settable(L, mt);            // mt.__call = new_T
    lua_setmetatable(L, methods);   // methods ----> mt

    // 类的成员函数
    // fill method table with methods from class T
    for (RegType *l = T::methods; l->name; l++) {
    /* edited by Snaily: shouldn't it be const RegType *l ... ? */
      lua_pushstring(L, l->name);

      lua_pushlightuserdata(L, (void*)l);
      lua_pushcclosure(L, thunk, 1);        // 生成 cclosure, 这个 cclosure 有1个upvalue, 就是这个 [函数名->函数指针] 的映射entry

      lua_settable(L, methods);
      // methods[函数名] = cclosure
    }

    lua_pop(L, 2);  // drop metatable and method table
  }

  // get userdata from Lua stack and return pointer to T object
  // 这个 check 其实是个 get 函数: 拿到C++对象
  static T *check(lua_State *L, int narg) {
    userdataType *ud =
      static_cast<userdataType*>(luaL_checkudata(L, narg, T::className));       // 强制转换, 如果不成功, 得到的是NULL
    if(!ud) luaL_typerror(L, narg, T::className);
    return ud->pT;  // pointer to T object
  }

private:
  Luna();  // hide default constructor

  // obj:foo(...)
  // member function dispatcher
  static int thunk(lua_State *L) {
    // stack has userdata, followed by method args
    T *obj = check(L, 1);  // get 'self', or if you prefer, 'this'
    
    lua_remove(L, 1);  // remove self so member function args start at index 1
    // remove 的原因是为了后面 (obj->*(l->mfunc))(L) 
    // 注意这里有内存拷贝(性能点)

    // get member function from upvalue
    // lua_upvalueindex(1) == LUA_REGISTRYINDEX-1
    // 这里用的是个整型的 trick, index2addr 根据 idx 不同的区间判断索引指向的位置(共4种)
    // 如果是 upvalue, 会从调用栈 callInfo 上找到这个函数, 最终找到 upvalue
    // 这个 upvalue 就是构造 cclosure 时的 lightuserdata
    RegType *l = static_cast<RegType*>(lua_touserdata(L, lua_upvalueindex(1)));
    return (obj->*(l->mfunc))(L);  // call member function
  }

  // create a new T object and
  // push onto the Lua stack a userdata containing a pointer to T object
  static int new_T(lua_State *L) {
    lua_remove(L, 1);   // use classname:new(), instead of classname.new()

    T *obj = new T(L);  // call constructor for T objects
    userdataType *ud =
      static_cast<userdataType*>(lua_newuserdata(L, sizeof(userdataType)));
    ud->pT = obj;  // store pointer to object in userdata
    luaL_getmetatable(L, T::className);  // lookup metatable in Lua registry
    lua_setmetatable(L, -2);
    return 1;  // userdata containing pointer to T object
  }

  // garbage collection metamethod
  static int gc_T(lua_State *L) {
    userdataType *ud = static_cast<userdataType*>(lua_touserdata(L, 1));
    T *obj = ud->pT;
    delete obj;  // call destructor for T objects
    return 0;
  }

  static int tostring_T (lua_State *L) {
    char buff[32];
    userdataType *ud = static_cast<userdataType*>(lua_touserdata(L, 1));
    T *obj = ud->pT;
    sprintf(buff, "%p", obj);
    lua_pushfstring(L, "%s (%s)", T::className, buff);
    return 1;
  }
};