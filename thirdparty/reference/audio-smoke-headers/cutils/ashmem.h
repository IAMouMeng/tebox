#pragma once

#ifdef __cplusplus
extern "C" {
#endif
int ashmem_create_region(const char* name, size_t size);
int ashmem_set_prot_region(int fd, int prot);
#ifdef __cplusplus
}
#endif
