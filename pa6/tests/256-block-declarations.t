void f()
{
    using YB = int;
    using N1::x;
    using namespace N1;
    namespace QN2 = N1;
    asm("nop");
    static_assert(1, "ok");
}
