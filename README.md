# Code for Eric Niebler's CppCon 2021 talk

```sh
mkdir build
cd build
cmake -G Ninja -DCMAKE_CXX_STANDARD:STRING=23 -DCMAKE_CXX_FLAGS:STRING="/Zc:externConstexpr /EHsc /fsanitize=address" ..
```
