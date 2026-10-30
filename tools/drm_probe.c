#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int main(int argc, char **argv)
{
    int fd = open(argc > 1 ? argv[1] : "/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    drmModeRes *r = drmModeGetResources(fd);
    for (int i = 0; i < r->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, r->connectors[i]);
        printf("connector %u type %u connected=%d modes=%d", c->connector_id, c->connector_type, c->connection == DRM_MODE_CONNECTED, c->count_modes);
        if (c->count_modes) printf(" first=%s@%u", c->modes[0].name, c->modes[0].vrefresh);
        printf(" enc=%u\n", c->encoder_id);
    }
    for (int i = 0; i < r->count_crtcs; i++) printf("crtc %u\n", r->crtcs[i]);
    drmModePlaneRes *p = drmModeGetPlaneResources(fd);
    for (uint32_t i = 0; i < p->count_planes; i++) {
        drmModePlane *pl = drmModeGetPlane(fd, p->planes[i]);
        drmModeObjectProperties *pr = drmModeObjectGetProperties(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
        uint64_t type = 9;
        for (uint32_t k = 0; k < pr->count_props; k++) {
            drmModePropertyRes *pp = drmModeGetProperty(fd, pr->props[k]);
            if (!strcmp(pp->name, "type")) type = pr->prop_values[k];
            drmModeFreeProperty(pp);
        }
        printf("plane %u type=%llu crtcs=0x%x formats:", pl->plane_id, (unsigned long long)type, pl->possible_crtcs);
        for (uint32_t k = 0; k < pl->count_formats; k++) printf(" %.4s", (char *)&pl->formats[k]);
        printf("\n");
    }
    return 0;
}
