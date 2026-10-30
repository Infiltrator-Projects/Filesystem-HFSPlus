/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_hfsplus_module_v1.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"fail: %s\n",#x); return 1; } } while (0)
int main(void) {
 const IfsHfsplusModuleV1 *api=infiltrator_filesystem_hfsplus_v1();
 CHECK(api!=NULL);
 CHECK(api->abi_version==1u);
 CHECK(api->structure_bytes>=sizeof(*api));
 CHECK(api->format_id && strcmp(api->format_id,"hfsplus")==0);
 CHECK(api->capabilities==(IFS_HFSPLUS_CAP_RUN | IFS_HFSPLUS_CAP_FIXTURE));
 CHECK(api->fixture!=NULL);
 CHECK(api->run!=NULL);
 char *args[]={"defragmenter-hfsplus-worker","--version",NULL};
 /* No host safety functions: even --version must fail without a trusted host. */
 CHECK(api->run(2,args,NULL)!=0);
 return 0;
}
