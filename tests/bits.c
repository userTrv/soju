/* Разрядность процесса и wow64: собирается под i686 и x86_64, печатает ptr=4|8 wow64=0|1. */
#include <stdio.h>
#include <windows.h>

int main(void) {
  BOOL wow64 = FALSE;
  IsWow64Process(GetCurrentProcess(), &wow64);
  printf("ptr=%d wow64=%d\n", (int)sizeof(void *), (int)wow64);
  return 0;
}
