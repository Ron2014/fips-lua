#include "UnitTest++/src/UnitTest++.h"
#include <iostream>
using namespace std;

extern "C" {
    #include "lua.h"
    #include "luaconf.h"
}

TEST(SizeTest) {
    printf("size: pointer %zd\n", sizeof(void*));
    printf("size: integer %zd\n", sizeof(LUA_INTEGER));
    printf("size: long %zd\n", sizeof(long));
    printf("size: int %zd\n", sizeof(int));
    printf("size: size_t %zd\n", sizeof(size_t));
}