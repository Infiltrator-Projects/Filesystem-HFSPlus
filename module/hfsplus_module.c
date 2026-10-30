/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_hfsplus_module_v1.h"
int ifs_hfsplus_worker_main(int argc, char **argv);
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
static int worker(int argc, char **argv, const IfsDefragServicesV1 *services) {
 if (ifs_bind_services(services) != 0) return 1;
 const int result=ifs_hfsplus_worker_main(argc,argv);
 ifs_unbind_services();
 return result;
}
static const IfsHfsplusModuleV1 api={
 IFS_HFSPLUS_MODULE_V1,sizeof(IfsHfsplusModuleV1),"hfsplus",
 IFS_HFSPLUS_CAP_RUN,worker
};
const IfsHfsplusModuleV1 *infiltrator_filesystem_hfsplus_v1(void){return &api;}
