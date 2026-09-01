void f()
{
    int* p = new int[5];
    int* q = new (buf) TC1<int>*[n][3];
    delete [] p;
}
