#include <stdlib.h>

extern "C" void __cxa_pure_virtual(void)
{
	while (true);
}

void *__dso_handle = 0;

void* operator new(unsigned int n) throw()
{
	return malloc(n);
}

void* operator new[](unsigned int n) throw()
{
	return malloc(n);
}

void operator delete(void* p) throw()
{
	free(p);
}

void operator delete[](void* p) throw()
{
	free(p);
}

void operator delete(void* p, unsigned int n) throw()
{
	free(p);
}

void operator delete[](void* p, unsigned int n) throw()
{
	free(p);
}
