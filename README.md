# 简介

Fips 是用python编写的命令行工具集，基于对cmake的封装（cmake wrapper），为C/C++工程提供了“集成构建环境”（Integrated Build Environment，IBE）。

## 我知道IDE，但啥是IBE

市面上已经有很多构建系统诸如 scratch，Fips在这里不敢班门弄斧（重复造轮子），而是把这些构建工具组合在fips命令行工具集中：

1. cmake 用来描述工程结构、生成工程文件。
2. make, ninja, xcodebuild 的命令行构建工具。
3. IDE 支持的cmake，诸如Visual Studio，Xcode，VSCode
4. git 用来获取外部工程
5. python 扩展fips命令，实现代码自动生成

## 总结

fips是用来组织代码的工具。它能够实现，工程的源码的跨平台编译环境：一处编写，通过fips 工具，就能很方便地在OSX上生成对应的OSX工程，在Windows上生成VS工程，在Linux上生成对应的工程。

当然你会说cmake等工具也能实现，但是fips在这里用胶水语言python赋予了cmake更多扩展。甚至提供了fips_xxx的cmake macro，让CMakeLists.txt的编写更加简洁、清晰。


## lua的编译平台

lua.org提供[下载](https://www.lua.org/download.html)的lua-5.3.5.tar.gz目录结构如下

> 1. doc/      文档（离线页面）
> 2. src/      源码
> 3. Makefile  make配置
> 4. README    说明

对于编译，官网仅提供了linux的方法

```
curl -R -O http://www.lua.org/ftp/lua-5.3.5.tar.gz
tar zxf lua-5.3.5.tar.gz
cd lua-5.3.5
make linux test
```

可见官方对跨平台编译的诚意并不够，fips的出现刚好弥补了这一点。当然已有的lua windows platform项目包括lua-for-windows等开源项目，不过绑定了SciTE等这些wtf的编辑器就显得很恶心。我们何不以自由软件的名义，找些简单的工具去构建自己想要的，足够清！爽！的lua项目呢？

## lua的版本

lua版本也是个问题。

比如看的一本关于lua的技术书/博客是一个版本，自己工作中用的lua是一个版本，自己最熟的又是最早的某个版本，而且不排除工作中会涉及到不同版本间的比较。就目前来说，暂时用了两个版本：

1. lua-5.1.4

这是《lua设计与实现》中使用的版本，[作者codedump](https://github.com/lichuang)。因为他给源码加上了有趣而又宝贵的注释，我就从他的git主页上clone过来了。

2. lua-5.3.5

这是我的工作中用到的版本，也是lua最新的稳定版本。很多其他的项目也会尝试去支持这个版本，包括TypeScriptToLua，skynet等。我这里效仿codedump，clone下来一边阅读一边注释，感觉蛮爽。

## lua的编译

每个lua版本都会提供src目录，其中都是.h和.c文件。这些文件大致分为3部分：

1. lua.c。解释器入口文件。
2. luac.c。编译器入口文件。
3. 其他.h和.c。lua虚拟机的实现。

所谓入口文件，就是提供了main函数，所以lua.c和luac.c这两个文件不能包含到一个VS工程（注意是project，而非solution）里。在层级结构里，【其他.h和.c】出于底层，lua.c和luac.c出于上层。

简单理解，那些【其他.h和.c】可以编译成 *.lib（可重定位文件），就叫他lua.lib吧。lua.lib + lua.c 一起（静态）编译、链接得到的lua.exe就是解释器；lua.lib + luac.c 一起（静态）编译、链接得到的luac.exe就是编译器。

### 解释器和编译器的区别

1. 解释器lua.exe：直接执行lua脚本/字节码文件的逻辑，如果没有指定lua脚本/字节码文件，则进入交互模式。
2. 编译器luac.exe：对于lua脚本，仅编译到opcode的程度（没有执行），并将这些opcode信息dump出来生成文件，这个文件就叫字节码文件，该文件默认名luac.out。

这样一番解释你该明白了，任何一个版本的lua源码（src目录），实际上至少包含三个（VS）工程：

1. lua虚拟机的lib工程。就是那些【其他的.h和.c】。
2. lua解释器的exe工程。仅包含lua.c文件，但是编译时依赖虚拟机lib工程。
3. lua编译器的exe工程。仅包含luac.c文件，但是编译时依赖虚拟机lib工程。

当然上面的工程都是静态编译的，如果你了解静态编译和动态编译的区别，还可以指定三个动态编译的（VS）工程：

4. lua虚拟机的dll工程。就是那些【其他的.h和.c】。
5. lua解释器的exe工程。仅包含lua.c文件，但是编译时依赖虚拟机dll工程。
6. lua编译器的exe工程。仅包含luac.c文件，但是编译时依赖虚拟机dll工程。

fips也提供 fips_begin_sharelib 这样的创建dll的cmake宏。只是其官网推荐静态编译，所以dll的工程构建就作为保留项目交给网友自己推敲啦:)。

要注意的是，这里说来说去都是讨论工程文件如何编写，并没有涉及到任何文件的拷贝，也就是说dll工程中的文件和lib工程中的文件是同一个，要改一起改。

作为跨平台，exe/lib/dll 在linux上也有对等的概念 */a/so，在fips提供的cmake宏中就高度统一了起来： fips_begin_app/fips_begin_lib/fips_begin_sharelib，所以在fips支持的CMakeLists.txt中没有过多的platform判断和文件后缀名的麻烦。

## 使用方式

1. 安装python和cmake
2. 将 [Code Janitor大佬的fips工程](https://github.com/floooh/fips) clone下来。
3. 将 [我这个工程](https://github.com/Ron2014/fips-lua) clone下来。
4. 保证 fips 和 fips-lua 这两个目录是同一级的。
5. 执行以下命令进行工程构建

```
cd fips-lua
python fips gen
```

你会看到vs工程生成在fips-build目录下。当然你也可以执行命令build，build=构建+编译。

```
cd fips-lua
python fips build
```

编译后的结果都放到fips-deploy目录下。

windows编译可能会有个 [小插曲](https://github.com/Ron2014/fips-lua/wiki/fips:-warning-C4819-x-error-C2220-%E7%9A%84%E8%A7%A3%E5%86%B3) 。

## 实现与参考文件

### 工程文件

https://github.com/Ron2014/fips-lua/blob/master/CMakeLists.txt

https://github.com/Ron2014/fips-lua/blob/master/lua-5.1.4/CMakeLists.txt

https://github.com/Ron2014/fips-lua/blob/master/lua-5.3.5/CMakeLists.txt

### 参考文档

http://floooh.github.io/fips/

官网写的还是很通俗易懂的，学习曲线主要是通读这10篇文章，在这个过程中把 [fips-hello-world](https://github.com/floooh/fips-hello-world) 这个demo看一遍。
