auto f() -> int { return 0; }
auto g(int p) -> decltype(p);
using YF = auto (int) -> int;
auto h() -> int (*)(int);
