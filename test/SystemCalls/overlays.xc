// RUN: xcc -foverlay=syscall -target=XK-1A %s -o %t1.xe
// RUN: %sim %t1.xe > %t2.txt
// RUN: cmp %t2.txt %s.expect
// RUN: xcc -foverlay=syscall -target=XCORE-200-EXPLORER %s -o %t1.xe
// RUN: %sim %t1.xe > %t2.txt
// RUN: cmp %t2.txt %s.expect

#include <print.h>

[[overlay]]
void f() {
  printstr("f\n");
}

[[overlay]]
void g() {
  printstr("g\n");
}

int main()
{
  f();
  g();
  return 0;
}
