void f()
{
    [[foo]] y = 1;
    [[foo]] if (y) ;
    [[foo]] for (;;) break;
    [[foo]] return;
}
