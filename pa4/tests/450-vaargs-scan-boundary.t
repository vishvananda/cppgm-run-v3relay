#define S(x) # x
#define EAT(x)
#define P(a,b) a ## b
S(__VA_ARGS__)
EAT(__VA_ARGS__)
P(x __VA_ARGS__, y)
