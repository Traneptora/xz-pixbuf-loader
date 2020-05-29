/* GdkPixbuf library - .image.xz Image Loader
 *
 * Author(s): Leo Izen (thebombzen) <leo.izen@gmail.com>
 *
 * Copyright (C) 2020 Leo Izen (thebombzen)
 *
 * Permission is hereby granted, free of charge, to any person 
 * obtaining a copy of this software and associated documentation 
 * files (the "Software"), to deal in the Software without 
 * restriction, including without limitation the rights to use, 
 * copy, modify, merge, publish, distribute, sublicense, and/or 
 * sell copies of the Software, and to permit persons to whom the 
 * Software is furnished to do so, subject to the following 
 * conditions:
 *
 * The above copyright notice and this permission notice shall be 
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR 
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <gio/gio.h>
#include <lzma.h>

#define GDK_PIXBUF_ENABLE_BACKEND
#include <gdk-pixbuf/gdk-pixbuf.h>
#undef  GDK_PIXBUF_ENABLE_BACKEND

/* Loader Context */
typedef struct {

    GdkPixbufModuleSizeFunc size_func;
    GdkPixbufModuleUpdatedFunc updated_func;
    GdkPixbufModulePreparedFunc prepare_func;

    lzma_stream *lzstream;
    uint8_t *unxz_buffer;
    size_t xz_buffer_size;

    gpointer extra_context;
    GdkPixbuf *pixbuf;
    GError **error;
    GInputStream *memory_istream;

} XZImageDecodeContext;

/* Load xz-compressed image directly in one go */
static GdkPixbuf *gdk_pixbuf__load_xz_image(FILE *file, GError **error) {

    char *error_message = NULL;

    const size_t buffer_size = 1 << 20;
    uint8_t *xz_buffer = NULL;
    uint8_t *unxz_buffer = NULL;
    GInputStream *memory_istream = NULL;
    GdkPixbuf *pixbuf = NULL;
    
    lzma_stream *lzstream = NULL;
    lzma_ret lzret;
    lzma_action lzaction;

    lzstream = malloc(sizeof(lzma_stream));
    if (!lzstream){
        error_message = "Failed to allocate lzma_stream";
        goto failure;
    }
    *lzstream = (lzma_stream) LZMA_STREAM_INIT;

    lzret = lzma_stream_decoder(lzstream, UINT64_MAX, LZMA_CONCATENATED);
    if (lzret != LZMA_OK) {
        error_message = "Could not create lzma_stream_decoder";
        goto failure;
    }

    xz_buffer = (uint8_t *) malloc(buffer_size);
    unxz_buffer = (uint8_t *) malloc(buffer_size);
    if (!xz_buffer || !unxz_buffer){
        error_message = "Could not allocate xz data buffers";
        goto failure;
    }

    lzaction = LZMA_RUN;
    lzstream->next_in = NULL;
    lzstream->avail_in = 0;
    lzstream->next_out = unxz_buffer;
    lzstream->avail_out = buffer_size;

    memory_istream = g_memory_input_stream_new();

    while (TRUE){

        if (lzstream->avail_in == 0 && !feof(file)){
            size_t bytes_read = fread(xz_buffer, 1, buffer_size, file);
            if (bytes_read < buffer_size){
                if (ferror(file)){
                    error_message = "Error reading file with fread";
                    goto failure;
                }
            }
            lzstream->next_in = xz_buffer;
            lzstream->avail_in = bytes_read;
            if (feof(file)){
                lzaction = LZMA_FINISH;
            }
        }

        lzret = lzma_code(lzstream, lzaction);
        
        if (lzstream->avail_out == 0 || lzret == LZMA_STREAM_END){
            size_t mem_buffer_size = buffer_size - lzstream->avail_out;
            void *mem_buffer = malloc(mem_buffer_size);
            if (!mem_buffer){
                error_message = "Error allocating memory";
                goto failure;
            }
            memcpy(mem_buffer, unxz_buffer, mem_buffer_size);
            g_memory_input_stream_add_data(G_MEMORY_INPUT_STREAM(memory_istream), mem_buffer, mem_buffer_size, free);
            lzstream->next_out = unxz_buffer;
            lzstream->avail_out = buffer_size;
        }

        if(lzret != LZMA_OK){
            if (lzret == LZMA_STREAM_END)
                break;
            error_message = "Some LZMA error occurred";
            goto failure;
        }

    } // while(TRUE)

    pixbuf = gdk_pixbuf_new_from_stream(memory_istream, NULL, error);
    if (!pixbuf){
        error_message = "Could not create pixbuf from memory stream";
        goto failure;
    }

    g_input_stream_close(memory_istream, NULL, error);
    lzma_end(lzstream);
    free(lzstream);
    free(xz_buffer);
    free(unxz_buffer);

    return pixbuf;

failure:
    g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED, error_message);
    if (xz_buffer)
        free(xz_buffer);
    if (unxz_buffer)
        free(unxz_buffer);
    if (lzstream)
        free(lzstream);
    if (memory_istream)
        g_input_stream_close(memory_istream, NULL, error);
    return NULL;

}

/* Start the asynchronous loading process */
static gpointer gdk_pixbuf__begin_load_xz_image(GdkPixbufModuleSizeFunc size_func, GdkPixbufModulePreparedFunc prepare_func,
        GdkPixbufModuleUpdatedFunc updated_func, gpointer extra_context, GError **error) {

    char *error_message = NULL;

    XZImageDecodeContext *context = (XZImageDecodeContext *) calloc(1, sizeof(XZImageDecodeContext));
    if (!context){
        error_message = "Error allocating decode context";
        goto failure;
    }

    context->lzstream = (lzma_stream *) malloc(sizeof(lzma_stream));
    if (!context->lzstream){
        error_message = "Error allocating lzma stream in context";
        goto failure;
    }
    *(context->lzstream) = (lzma_stream) LZMA_STREAM_INIT;

    lzma_ret lzret = lzma_stream_decoder(context->lzstream, UINT64_MAX, LZMA_CONCATENATED);
    if (lzret != LZMA_OK) {
        error_message = "Could not create lzma_stream_decoder";
        goto failure;
    }

    context->xz_buffer_size = 1 << 16;
    context->unxz_buffer = (uint8_t *) malloc(context->xz_buffer_size);
    if (!context->unxz_buffer) {
        error_message = "Could not create xz buffers";
        goto failure;
    }

    context->lzstream->next_in = NULL;
    context->lzstream->avail_in = 0;
    context->lzstream->next_out = context->unxz_buffer;
    context->lzstream->avail_out = context->xz_buffer_size;
    
    context->memory_istream = g_memory_input_stream_new();
    context->size_func = size_func;
    context->prepare_func = prepare_func;
    context->updated_func  = updated_func;
    context->extra_context = extra_context;
    context->error = error;

    return context;

failure:
    g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED, error_message);
    if (context){
        if (context->lzstream)
            free(context->lzstream);
        if (context->unxz_buffer)
            free(context->unxz_buffer);
        if (context->memory_istream)
            g_input_stream_close(context->memory_istream, NULL, error);
        free(context);
    }
    return NULL;
}

/* Here we do the actual LZMA decoding */
static gboolean _gdk_pixbuf__lzma_code(gpointer user_context, const guchar *buf, guint size, GError **error, lzma_action lzaction){
    char *error_message = NULL;

    XZImageDecodeContext *context = (XZImageDecodeContext *) user_context;
    context->lzstream->next_in = (const uint8_t *) buf;
    context->lzstream->avail_in = size;

    do {
        lzma_ret lzret = lzma_code(context->lzstream, lzaction);
        if (lzret == LZMA_OK || lzret == LZMA_STREAM_END){
            size_t mem_buffer_size = context->xz_buffer_size - context->lzstream->avail_out;
            void *mem_buffer = malloc(mem_buffer_size);
            if (!mem_buffer){
                error_message = "Error allocating buffer";
                goto failure;
            }
            memcpy(mem_buffer, context->unxz_buffer, mem_buffer_size);
            g_memory_input_stream_add_data(G_MEMORY_INPUT_STREAM(context->memory_istream), mem_buffer, mem_buffer_size, free);
            context->lzstream->avail_out = context->xz_buffer_size;
            context->lzstream->next_out = context->unxz_buffer;
        } else {
            error_message = "Error with lzma decode";
            goto failure;
        }
    } while (context->lzstream->avail_in != 0);

    return TRUE;

failure:
    g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED, error_message);
    return FALSE;    
}

/* Finish decoding the image, and render it */
static gboolean gdk_pixbuf__stop_load_xz_image(gpointer user_context, GError **error) {

    XZImageDecodeContext *context = (XZImageDecodeContext *) user_context;

    /* We do a final run of lzma_code in order to tell liblzma to finish and flush */
    gboolean ret = _gdk_pixbuf__lzma_code(user_context, NULL, 0, error, LZMA_FINISH);
    lzma_end(context->lzstream);

    context->pixbuf = gdk_pixbuf_new_from_stream(context->memory_istream, NULL, error);
    if (!context->pixbuf)
        ret = FALSE;
    g_input_stream_close(context->memory_istream, NULL, context->error);

    if (context->pixbuf && context->prepare_func){
        (* context->prepare_func)(context->pixbuf, NULL, context->extra_context);
    }
    if (context->pixbuf && context->updated_func) {
        (* context->updated_func)(context->pixbuf, 0, 0, gdk_pixbuf_get_width(context->pixbuf), gdk_pixbuf_get_height(context->pixbuf), context->extra_context);
    }

    g_object_unref(context->pixbuf);
    free(context->lzstream);
    free(context->unxz_buffer);
    free(context);
    return ret;
}

/*
 * Incrementally decode lzma
 * This wrapper is here so we don't have to duplicate this in stop_load
 */
static gboolean gdk_pixbuf__load_xz_image_increment(gpointer user_context, const guchar *buf, guint size, GError **error) {
    return _gdk_pixbuf__lzma_code(user_context, buf, size, error, LZMA_RUN);
}

/* Gdk Pixbuf clients call this */
void fill_vtable(GdkPixbufModule *module) {
    module->load = gdk_pixbuf__load_xz_image;
    module->begin_load = gdk_pixbuf__begin_load_xz_image;
    module->stop_load = gdk_pixbuf__stop_load_xz_image;
    module->load_increment = gdk_pixbuf__load_xz_image_increment;
}

/* Gdk Pixbuf clients call this */
void fill_info(GdkPixbufFormat *info) {

    /*
     * In theory liblzma should be able to handle LZMA ALONE files
     * However, .lzma files do not have a magic
     * So it's just not listed here.
     */
    static GdkPixbufModulePattern signature[] = {
        { "\xFD" "7zXZx", "     z", 100 },
        { NULL, NULL, 0 }
    };

    /*
     * MIME type taken from the specs
     * https://tukaani.org/xz/xz-file-format.txt
     * https://svn.python.org/projects/external/xz-5.0.3/doc/lzma-file-format.txt
     */
    static gchar *mime_types[] = {
        "application/x-xz",
        "application/x-lzma",
        NULL
    };

    /*
     * afaik it's case sensitive, but I'm too
     * lazy to actually find out if this is true
     */
    static gchar *extensions[] = {
        "xz", "XZ",
        "lzma", "LZMA",
        NULL
    };

    info->name        = "xz";
    info->signature   = signature;
    info->description = "xz-compressed Image";
    info->mime_types  = mime_types;
    info->extensions  = extensions;
    info->flags       = GDK_PIXBUF_FORMAT_THREADSAFE;
    info->license     = "MIT";
}
