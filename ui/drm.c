#include "qemu/osdep.h"
#include "qemu-common.h"
#include "sysemu/sysemu.h"
#include "ui/console.h"

#include "xf86drm.h"
#include "xf86drmMode.h"

#include "drm_fourcc.h"

struct drm_output {
    drmModeConnector *connector;
    drmModeModeInfoPtr mode;
    uint32_t width;
    uint32_t height;
    
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t fb_id;
    bool has_cursor;
    uint32_t cursor_handle;
    uint32_t cursor_w, cursor_h;
};

typedef struct drm_dpy {
    DisplayChangeListener dcl;
    DisplaySurface *ds;
    int fd;
    struct drm_output *output;
    uint32_t pos_x, pos_y;
} drm_dpy;

/* ------------------------------------------------------------------ */

static void drm_refresh(DisplayChangeListener *dcl)
{
    // will call hwop->gfx_update, vfio_display_dmabuf_update
    graphic_hw_update(dcl->con);
}

static void drm_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
    /* refresh -> graphic_hw_update -> vfio/display -> dpy_gl_update */
}

static void drm_gfx_switch(DisplayChangeListener *dcl,
                           struct DisplaySurface *new_surface)
{
    drm_dpy *dpy = container_of(dcl, drm_dpy, dcl);

    dpy->ds = new_surface;
}

static void drm_scanout_disable(DisplayChangeListener *dcl)
{
    /* destroy fb, disable plane */
    //drm_dpy *dpy = container_of(dcl, drm_dpy, dcl);
}

static void drm_scanout_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    /* get dmabuf fd, init drm fb from it */
    drm_dpy *dpy = container_of(dcl, drm_dpy, dcl);
    uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
    uint64_t modifiers[4] = { 0 };
    uint32_t fb_id;
    int ret;

    if (drmPrimeFDToHandle(dpy->fd, dmabuf->fd, &dmabuf->handle)) {
        error_report("fd->handle failed\n");
        return;
    }
    handles[0] = dmabuf->handle;
    pitches[0] = dmabuf->stride;
    offsets[0] = 0;
    modifiers[0] = dmabuf->mod;

    ret = drmModeAddFB2WithModifiers(dpy->fd, dmabuf->width, dmabuf->height,
                                     dmabuf->fourcc, handles, pitches, offsets,
                                     modifiers, &fb_id, DRM_MODE_FB_MODIFIERS);
    if (ret) {
        error_report("addfb2 fail: %d", ret);
        return;
    }
    dpy->output->fb_id = fb_id;
    dpy->output->width = dmabuf->width;
    dpy->output->height = dmabuf->height;
}

static void drm_cursor_dmabuf(DisplayChangeListener *dcl,
                              QemuDmaBuf *dmabuf, bool have_hot,
                              uint32_t hot_x, uint32_t hot_y)
{
    /* cursor fb */
    drm_dpy *dpy = container_of(dcl, drm_dpy, dcl);

    if (!dmabuf) {
        dpy->output->has_cursor = false;
        //XXX cleanup old?
        return;
    }
    if (drmPrimeFDToHandle(dpy->fd, dmabuf->fd, &dmabuf->handle)) {
        error_report("cursor fd->handle failed\n");
        return;
    }
    dpy->output->cursor_handle = dmabuf->handle;
    dpy->output->cursor_w = dmabuf->width;
    dpy->output->cursor_h = dmabuf->height;
    dpy->output->has_cursor = true;
}

static void drm_cursor_position(DisplayChangeListener *dcl,
                                uint32_t pos_x, uint32_t pos_y)
{
    /* cursor update */
    drm_dpy *dpy = container_of(dcl, drm_dpy, dcl);

    dpy->pos_x = pos_x;
    dpy->pos_y = pos_y;
}

static void drm_release_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    /* release framebuffer */
    drm_dpy *dpy = container_of(dcl, drm_dpy, dcl);

    drmModeRmFB(dpy->fd, dpy->output->fb_id);
}

static drmModeModeInfo *
choose_mode(drm_dpy *dpy)
{
    drmModeConnector *connector = dpy->output->connector;
    int i;

    /* This is still rough way to choose best mode fit */
    for (i = 0; i < connector->count_modes; i++) {
        if (dpy->output->width >= connector->modes[i].hdisplay &&
            dpy->output->height >= connector->modes[i].vdisplay &&
            (dpy->output->width * dpy->output->height) >=
            (connector->modes[i].hdisplay * connector->modes[i].vdisplay))
            return &connector->modes[i];
    }
    return NULL;
}

static void drm_scanout_flush(DisplayChangeListener *dcl,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    /* Do modesetting here */
    drm_dpy *dpy = container_of(dcl, drm_dpy, dcl);
    drmModeModeInfo *mode;
    
    if (!dpy->ds) {
        error_report("no DisplayState?");
        return;
    }

    assert(surface_width(dpy->ds)  == dpy->output->width);
    assert(surface_height(dpy->ds) == dpy->output->height);
    assert(surface_format(dpy->ds) == PIXMAN_x8r8g8b8);

    mode = choose_mode(dpy);
    if (!mode) {
        error_report("no mode found!");
        return;
    }
    dpy->output->mode = mode;
    
    drmModeSetCrtc(dpy->fd, dpy->output->crtc_id,
                   dpy->output->fb_id, 0, 0,
                   &dpy->output->connector_id, 1,
                   mode);

    if (dpy->output->has_cursor) {
        drmModeSetCursor(dpy->fd, dpy->output->crtc_id,
                         dpy->output->cursor_handle,
                         dpy->output->cursor_w, dpy->output->cursor_h);

        drmModeMoveCursor(dpy->fd, dpy->output->crtc_id,
                          dpy->pos_x, dpy->pos_y);
    }
                     
    //will call drm_ops->dpy_gfx_update
    dpy_gfx_update(dpy->dcl.con, x, y, w, h);
    
}

static const DisplayChangeListenerOps drm_ops = {
    .dpy_name                = "drm",
    .dpy_refresh             = drm_refresh,
    .dpy_gfx_update          = drm_gfx_update,
    .dpy_gfx_switch          = drm_gfx_switch,
    
    .dpy_gl_scanout_disable  = drm_scanout_disable,
    .dpy_gl_scanout_dmabuf   = drm_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf    = drm_cursor_dmabuf,
    .dpy_gl_cursor_position  = drm_cursor_position,
    .dpy_gl_release_dmabuf   = drm_release_dmabuf,
    .dpy_gl_update           = drm_scanout_flush,
};

static void early_drm_init(DisplayOptions *opts)
{
    //XXX need below? workaround some gl scanout dmabuf check
    display_opengl = 1;
}

static int
find_crtc_for_connector(int fd, drmModeRes *res, drmModeConnector *connector)
{
    drmModeEncoder *encoder;
    int i, j;
    int ret = -1;

    for (j = 0; j < connector->count_encoders; j++) {
        uint32_t possible_crtcs, encoder_id, crtc_id;
        
        encoder = drmModeGetEncoder(fd, connector->encoders[j]);                                
        if (encoder == NULL)
            continue;

        encoder_id = encoder->encoder_id;
        possible_crtcs = encoder->possible_crtcs;
        crtc_id = encoder->crtc_id;
        drmModeFreeEncoder(encoder);

        for (i = 0; i < res->count_crtcs; i++) {
            if (!(possible_crtcs & (1 << i)))
                continue;                                                                      

            /* XXX currently just choose default connected one.
             * qemu needs a way to choose which target connector.
             */
            if (!connector->encoder_id ||
                (encoder_id == connector->encoder_id &&
                 crtc_id == res->crtcs[i]))
                return i;                                                                      

            ret = i;
        }
    }
    return ret;
}

static struct drm_output *
init_kms(int fd)
{
    drmModeResPtr res;
    drmModeConnector *connector;
    int err, i, idx;
    struct drm_output *output;

    output = g_new0(struct drm_output, 1);
    if (!output)
        return NULL;

    err = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (err < 0) {
        fprintf(stderr, "drmSetClientCap() failed: %d\n", err);
        free(output);
        return NULL;
    }                  
    res = drmModeGetResources(fd);
    if (!res) {
        free(output);
        return NULL;
    }

    for (i = 0; i < res->count_connectors; i++) {
        connector = drmModeGetConnector(fd, res->connectors[i]);
        if (connector == NULL)
            continue;
        
        if (connector->connection == DRM_MODE_CONNECTED) {
            output->connector = connector;
            output->connector_id = connector->connector_id;
            output->mode = &connector->modes[0];
            idx = find_crtc_for_connector(fd, res, connector);
            output->crtc_id = res->crtcs[idx];
            break;
        } else
            drmModeFreeConnector(connector);
    }
    if (!output->connector) {
        error_report("drm: no output found");
        free(output);
        return NULL;
    }
    drmModeFreeResources(res);
    return output;
}

static void drm_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;
    drm_dpy *dpy;
    int idx;
    int fd;
    struct drm_output *output;

    if ((fd = drmOpen("i915", "pci:0000:00:02.0")) < 0) {
        error_report("drm: open card failed");
        exit(1);
    }

    /* init kms */
    if ((output = init_kms(fd)) == NULL) {
        close(fd);
        error_report("drm: init_kms failed");
        exit(1);
    }
    
    for (idx = 0;; idx++) {
        con = qemu_console_lookup_by_index(idx);
        if (!con || !qemu_console_is_graphic(con)) {
            break;
        }

        dpy = g_new0(drm_dpy, 1);
        dpy->dcl.con = con;
        dpy->dcl.ops = &drm_ops;
        dpy->dcl.need_gl = true;
        dpy->fd = fd;
        dpy->output = output;
        register_displaychangelistener(&dpy->dcl);
    }
}

static QemuDisplay qemu_display_drm = {
    .type       = DISPLAY_TYPE_DRM,
    .early_init = early_drm_init,
    .init       = drm_init,
};

static void register_drm(void)
{
    qemu_display_register(&qemu_display_drm);
}

type_init(register_drm);
