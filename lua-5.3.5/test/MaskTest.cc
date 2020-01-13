#include "UnitTest++/src/UnitTest++.h"

#include <iostream>
using namespace std;

#ifdef __cplusplus
extern "C" {
#endif

#include "lopcodes.h"

#ifdef __cplusplus
}
#endif

TEST(MaskTest) {
    // 4294967171
    // 1111 1111 1111 1111 1111 1111 1000 0011B
    cout << MASK0(5, 2) << endl;

    // 124
    // 0000 0000 0000 0000 0000 0000 0111 1100B
    cout << MASK1(5, 2) << endl;

    // 3229614079
    // 1100 0000 0111 1111 1111 1111 1111 1111B
    cout << MASK0(7, 23) << endl;

    // 1065353216
    // 0011 1111 1000 0000 0000 0000 0000 0000B
    cout << MASK1(7, 23) << endl;
    cout << "Hello world" << endl;
}