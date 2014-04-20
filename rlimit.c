#include <sys/time.h>
#include <sys/resource.h>

int main()
{
  struct rlimit r;
  getrlimit(RLIMIT_NOFILE, &r);
  printf("soft: %d\n", (int) r.rlim_cur);
  printf("hard: %d\n", (int) r.rlim_max);
  r.rlim_cur = 65536;
  r.rlim_max = 65536;
  printf("returned: %d\n", setrlimit(RLIMIT_NOFILE, &r));
  printf("soft: %d\n", (int) r.rlim_cur);
  printf("hard: %d\n", (int) r.rlim_max);
}
