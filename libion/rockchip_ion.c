#include <cutils/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/ion.h>
#include <ion/ion.h>
#include <ion/rockchip_ion.h>

static int ion_ioctl(int fd, int req, void *arg)
{
    int ret = ioctl(fd, req, arg);
    if (ret < 0) {
        ALOGE("ioctl %x failed with code %d: %s\n", req,
              ret, strerror(errno));
        return -errno;
    }
    return ret;
}

int ion_get_phys(int fd, ion_user_handle_t handle, unsigned long *phys)
{
    struct ion_phys_data phys_data;
    struct ion_custom_data data;

    phys_data.handle = handle;
    phys_data.phys = 0;

    data.cmd = ION_IOC_GET_PHYS;
    data.arg = (unsigned long)&phys_data;

    int ret = ion_ioctl(fd, ION_IOC_CUSTOM, &data);
    if (ret<0)
        return ret;

    *phys = phys_data.phys;

    return 0;
}

int ion_secure_free(int fd, size_t len, unsigned long phys)
{
    struct ion_phys_data phys_data;
    struct ion_custom_data data;

    phys_data.handle = 0;
    phys_data.phys = phys;
    phys_data.size = len;

    data.cmd = ION_IOC_FREE_SECURE;
    data.arg = (unsigned long)&phys_data;

    return ion_ioctl(fd, ION_IOC_CUSTOM, &data);
}

int ion_secure_alloc(int fd, size_t len, unsigned long *phys)
{
    struct ion_phys_data phys_data;
    struct ion_custom_data data;

    phys_data.handle = 0;
    phys_data.size = len;

    data.cmd = ION_IOC_ALLOC_SECURE;
    data.arg = (unsigned long)&phys_data;

    int ret = ion_ioctl(fd, ION_IOC_CUSTOM, &data);
    if (ret<0)
        return ret;

    *phys = phys_data.phys;
    return ret;
}
