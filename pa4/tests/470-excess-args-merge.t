#define ONE(a) [a]
#define TWO(a,b) [a][b]
#define STR(a) # a
#define CAT(a,b) a ## b
ONE(1, 2)
ONE(1, 2, 3)
TWO(1, 2, 3)
ONE(1,)
STR(1, 2)
CAT(u8, "x", _s)
