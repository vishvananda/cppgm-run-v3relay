#define M 5
#define G1() # M q
#define G2() # # x
#define G3() # ## y
#define G4() # "s\n" w
G1()
G2()
G3()
G4()
