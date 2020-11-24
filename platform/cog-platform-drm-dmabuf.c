#include "../core/cog.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <xf86drm.h>
#include <string.h>
#include <wayland-server.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-dmabuf.h>


static struct {
    int fd;
    struct gbm_device *device;
} gbm_data = {
    .fd = -1,
    .device = NULL,
};

static struct {
    GSource *update_source;
} glib_data = {
    .update_source = NULL,
};

static struct {
    struct wpe_dmabuf_pool_entry *pending_entry;
    struct wpe_dmabuf_pool_entry *committed_entry;
} dmabuf_data = {
    .pending_entry = NULL,
    .committed_entry = NULL,
};

static struct {
    struct wpe_view_backend_dmabuf_pool_fdo* dmabuf_pool;
} wpe_host_data;

static struct {
    struct wpe_view_backend *backend;
} wpe_view_data;


static void
init_config (CogShell *shell)
{
    GKeyFile *key_file = cog_shell_get_config_file (shell);
    if (!key_file)
        return;
}


static struct wpe_dmabuf_pool_entry *
dmabuf_pool_create_entry (void *data)
{
    struct gbm_bo *bo = gbm_bo_create (gbm_data.device, 800, 600,
                                       GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    fprintf(stderr, "dmabuf_pool_create_entry(): bo %p, %d planes, modifier %lu\n",
        bo, gbm_bo_get_plane_count (bo), gbm_bo_get_modifier (bo));

    struct wpe_dmabuf_pool_entry_init entry_init = {
        .width = gbm_bo_get_width (bo),
        .height = gbm_bo_get_height (bo),
        .format = gbm_bo_get_format (bo),
        .num_planes = gbm_bo_get_plane_count (bo),
    };

    uint64_t modifier = gbm_bo_get_modifier (bo);
    for (unsigned i = 0; i < entry_init.num_planes; ++i) {
        union gbm_bo_handle handle = gbm_bo_get_handle_for_plane (bo, i);
        drmPrimeHandleToFD(gbm_data.fd, handle.u32, 0, &entry_init.fds[i]);

        entry_init.strides[i] = gbm_bo_get_stride_for_plane (bo, i);
        entry_init.offsets[i] = gbm_bo_get_offset (bo, i);
        entry_init.modifiers[i] = modifier;
    }

    struct wpe_dmabuf_pool_entry *entry = wpe_dmabuf_pool_entry_create (&entry_init);
    fprintf(stderr, "dmabuf_pool_create_entry(): entry %p\n", entry);
    return entry;
}

static void
dmabuf_pool_destroy_entry (void *data, struct wpe_dmabuf_pool_entry *entry)
{
}

static void
dmabuf_pool_commit_entry (void *data, struct wpe_dmabuf_pool_entry *entry)
{
    fprintf(stderr, "dmabuf_pool_commit_entry(): entry %p\n", entry);
    dmabuf_data.pending_entry = entry;
}


static void
clear_gbm (void)
{
    g_clear_pointer (&gbm_data.device, gbm_device_destroy);
}

static gboolean
init_gbm (void)
{
    gbm_data.fd = open("/dev/dri/renderD128", O_RDWR);
    if (gbm_data.fd < 0)
        return FALSE;

    gbm_data.device = gbm_create_device (gbm_data.fd);
    if (!gbm_data.device)
        return FALSE;

    return TRUE;
}


static gboolean
update_source_dispatch (gpointer data)
{
    fprintf(stderr, "\n\nupdate_source_dispatch(), pending_entry %p\n", dmabuf_data.pending_entry);

    if (dmabuf_data.committed_entry) {
        struct wpe_dmabuf_pool_entry *committed_entry = dmabuf_data.committed_entry;
        dmabuf_data.committed_entry = NULL;

        wpe_view_backend_dmabuf_pool_fdo_dispatch_release_entry (wpe_host_data.dmabuf_pool, committed_entry);
    }

    if (dmabuf_data.pending_entry) {
        dmabuf_data.committed_entry = dmabuf_data.pending_entry;
        dmabuf_data.pending_entry = NULL;

        wpe_view_backend_dmabuf_pool_fdo_dispatch_frame_complete (wpe_host_data.dmabuf_pool);
    }

    return G_SOURCE_CONTINUE;
}

static void
clear_glib (void)
{
    if (glib_data.update_source)
        g_source_destroy (glib_data.update_source);
}

static gboolean
init_glib (void)
{
    glib_data.update_source = g_timeout_source_new (500);
    g_source_set_callback (glib_data.update_source, update_source_dispatch, NULL, NULL);
    g_source_attach (glib_data.update_source, g_main_context_get_thread_default ());
    return TRUE;
}


gboolean
cog_platform_plugin_setup (CogPlatform *platform,
                           CogShell    *shell,
                           const char  *params,
                           GError     **error)
{
    g_assert (platform);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    init_config (shell);

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to set backend library name");
        return FALSE;
    }

    if (!init_gbm ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GBM");
        return FALSE;
    }

    if (!init_glib ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GLib");
        return FALSE;
    }

    wpe_fdo_initialize_dmabuf ();

    return TRUE;
}

void
cog_platform_plugin_teardown (CogPlatform *platform)
{
    g_assert (platform);

    clear_glib ();
    clear_gbm ();
}

WebKitWebViewBackend *
cog_platform_plugin_get_view_backend (CogPlatform   *platform,
                                      WebKitWebView *related_view,
                                      GError       **error)
{
    static struct wpe_view_backend_dmabuf_pool_fdo_client dmabuf_pool_client = {
        .create_entry = dmabuf_pool_create_entry,
        .destroy_entry = dmabuf_pool_destroy_entry,
        .commit_entry = dmabuf_pool_commit_entry,
    };

    wpe_host_data.dmabuf_pool = wpe_view_backend_dmabuf_pool_fdo_create (&dmabuf_pool_client,
                                                                         NULL,
                                                                         800, 600);
    g_assert (wpe_host_data.dmabuf_pool);

    wpe_view_data.backend = wpe_view_backend_dmabuf_pool_fdo_get_view_backend (wpe_host_data.dmabuf_pool);
    g_assert (wpe_view_data.backend);

    WebKitWebViewBackend *wk_view_backend =
        webkit_web_view_backend_new (wpe_view_data.backend,
                                     (GDestroyNotify) wpe_view_backend_dmabuf_pool_fdo_destroy,
                                     wpe_host_data.dmabuf_pool);
    g_assert (wk_view_backend);

    return wk_view_backend;
}
