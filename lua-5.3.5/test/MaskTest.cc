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
    cout << MASK0(5, 2) << endl;
    cout << MASK1(5, 2) << endl;
    cout << "Hello world" << endl;
}