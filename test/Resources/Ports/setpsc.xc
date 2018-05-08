// RUN: xcc -O2 -target=XK-1A %s -o %t1.xe
// RUN: axe %t1.xe --loopback 0x80000 0x80100
// RUN: xcc -O2 -target=XCORE-200-EXPLORER %s -o %t1.xe
// RUN: axe %t1.xe --loopback 0x80000 0x80100

#include <xs1.h>

buffered out port:32 p = XS1_PORT_8A;
buffered in port:32 q = XS1_PORT_8B;
clock c = XS1_CLKBLK_1;

// Defeat compiler optimizations.
int identity(int x) {
  asm("mov %0, %1" : "=r"(x) : "r"(x));
  return x;
}

int main() {
  int val;
  int tmp;
  configure_in_port(q, c);
  configure_out_port(p, c, 0);
  configure_clock_ref(c, 10);
  start_clock(c);
  p @ 10 <: 0x99542c28;
  q @ 10 :> val;
  val >>= 24;
  set_port_shift_count(q, identity(8));
  q :> tmp;
  val |= (tmp >> 24) << 8;
  set_port_shift_count(q, identity(8));
  q :> tmp;
  val |= (tmp >> 24) << 16;
  set_port_shift_count(q, identity(8));
  q :> tmp;
  val |= (tmp >> 24) << 24;
  if (val != 0x99542c28)
    return 1;
  return 0;
}
