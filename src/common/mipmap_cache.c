/*
    This file is part of darktable,
    copyright (c) 2011-2014 johannes hanika.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/darktable.h"
#include "common/exif.h"
#include "common/grealpath.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/imageio_jpeg.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/jobs.h"
#include "common/debug.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <limits.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <xmmintrin.h>
#include <sys/statvfs.h>

#define DT_MIPMAP_CACHE_FILE_MAGIC 0xD71337
#define DT_MIPMAP_CACHE_FILE_VERSION 23
#define DT_MIPMAP_CACHE_DEFAULT_FILE_NAME "mipmaps"

typedef enum dt_mipmap_buffer_dsc_flags
{
  DT_MIPMAP_BUFFER_DSC_FLAG_NONE = 0,
  DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE = 1 << 0,
  DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE = 1 << 1
} dt_mipmap_buffer_dsc_flags;

// the embedded Exif data to tag thumbnails as sRGB or AdobeRGB
static const uint8_t dt_mipmap_cache_exif_data_srgb[] = {
  0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x49, 0x49, 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x69,
  0x87, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x01, 0xa0, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t dt_mipmap_cache_exif_data_adobergb[] = {
  0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x49, 0x49, 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x69,
  0x87, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x01, 0xa0, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const int dt_mipmap_cache_exif_data_srgb_length
                      = sizeof(dt_mipmap_cache_exif_data_srgb) / sizeof(*dt_mipmap_cache_exif_data_srgb);
static const int dt_mipmap_cache_exif_data_adobergb_length
                      = sizeof(dt_mipmap_cache_exif_data_adobergb) / sizeof(*dt_mipmap_cache_exif_data_adobergb);

struct dt_mipmap_buffer_dsc
{
  uint32_t width;
  uint32_t height;
  size_t size;
  dt_mipmap_buffer_dsc_flags flags;
  uint32_t pre_monochrome_demosaiced;
  dt_colorspaces_color_profile_type_t color_space;
  /* NB: sizeof must be a multiple of 4*sizeof(float) */
} __attribute__((packed, aligned(16)));

// last resort mem alloc for dead images. sizeof(dt_mipmap_buffer_dsc) + dead image pixels (8x8)
// __m128 type for sse alignment.
static __m128 dt_mipmap_cache_static_dead_image[sizeof(struct dt_mipmap_buffer_dsc) / sizeof(__m128) + 64];

static inline void dead_image_8(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->buf - 1;
  dsc->width = dsc->height = 8;
  dsc->color_space = DT_COLORSPACE_DISPLAY;
  assert(dsc->size > 64 * sizeof(uint32_t));
  const uint32_t X = 0xffffffffu;
  const uint32_t o = 0u;
  const uint32_t image[]
      = { o, o, o, o, o, o, o, o, o, o, X, X, X, X, o, o, o, X, o, X, X, o, X, o, o, X, X, X, X, X, X, o,
          o, o, X, o, o, X, o, o, o, o, o, o, o, o, o, o, o, o, X, X, X, X, o, o, o, o, o, o, o, o, o, o };
  memcpy(buf->buf, image, sizeof(uint32_t) * 64);
}

static inline void dead_image_f(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->buf - 1;
  dsc->width = dsc->height = 8;
  dsc->color_space = DT_COLORSPACE_DISPLAY;
  assert(dsc->size > 64 * 4 * sizeof(float));
  const __m128 X = _mm_set1_ps(1.0f);
  const __m128 o = _mm_set1_ps(0.0f);
  const __m128 image[]
      = { o, o, o, o, o, o, o, o, o, o, X, X, X, X, o, o, o, X, o, X, X, o, X, o, o, X, X, X, X, X, X, o,
          o, o, X, o, o, X, o, o, o, o, o, o, o, o, o, o, o, o, X, X, X, X, o, o, o, o, o, o, o, o, o, o };
  memcpy(buf->buf, image, sizeof(__m128) * 64);
}

#ifndef NDEBUG
static inline int32_t buffer_is_broken(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return 0;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->buf - 1;
  if(buf->width != dsc->width) return 1;
  if(buf->height != dsc->height) return 2;
  // somewhat loose bound:
  if(buf->width * buf->height > dsc->size) return 3;
  return 0;
}
#endif

static inline uint32_t get_key(const uint32_t imgid, const dt_mipmap_size_t size)
{
  // imgid can't be >= 2^28 (~250 million images)
  return (((uint32_t)size) << 28) | (imgid - 1);
}

static inline uint32_t get_imgid(const uint32_t key)
{
  return (key & 0xfffffff) + 1;
}

static inline dt_mipmap_size_t get_size(const uint32_t key)
{
  return (dt_mipmap_size_t)(key >> 28);
}

static int dt_mipmap_cache_get_filename(gchar *mipmapfilename, size_t size)
{
  int r = -1;
  char *abspath = NULL;

  // Directory
  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  // Build the mipmap filename
  const gchar *dbfilename = dt_database_get_path(darktable.db);
  if(!strcmp(dbfilename, ":memory:"))
  {
    mipmapfilename[0] = '\0';
    r = 0;
    goto exit;
  }

  abspath = g_realpath(dbfilename);
  if(!abspath) abspath = g_strdup(dbfilename);

  GChecksum *chk = g_checksum_new(G_CHECKSUM_SHA1);
  g_checksum_update(chk, (guchar *)abspath, strlen(abspath));
  const gchar *filename = g_checksum_get_string(chk);

  if(!filename || filename[0] == '\0')
    snprintf(mipmapfilename, size, "%s/%s", cachedir, DT_MIPMAP_CACHE_DEFAULT_FILE_NAME);
  else
    snprintf(mipmapfilename, size, "%s/%s-%s", cachedir, DT_MIPMAP_CACHE_DEFAULT_FILE_NAME, filename);

  g_checksum_free(chk);
  r = 0;

exit:
  g_free(abspath);

  return r;
}

static void _init_f(dt_mipmap_buffer_t *mipmap_buf, float *buf, uint32_t *width, uint32_t *height,
                    const uint32_t imgid);
static void _init_8(uint8_t *buf, uint32_t *width, uint32_t *height, dt_colorspaces_color_profile_type_t *color_space,
                    const uint32_t imgid, const dt_mipmap_size_t size);

// callback for the imageio core to allocate memory.
// only needed for _F and _FULL buffers, as they change size
// with the input image. will allocate img->width*img->height*img->bpp bytes.
void *dt_mipmap_cache_alloc(dt_mipmap_buffer_t *buf, const dt_image_t *img)
{
  assert(buf->size == DT_MIPMAP_FULL);

  const int wd = img->width;
  const int ht = img->height;
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)buf->cache_entry->data;
  const size_t buffer_size = (size_t)wd*ht*img->bpp + sizeof(*dsc);

  // buf might have been alloc'ed before,
  // so only check size and re-alloc if necessary:
  if(!buf->buf || (dsc->size < buffer_size) || ((void *)dsc == (void *)dt_mipmap_cache_static_dead_image))
  {
    if((void *)dsc != (void *)dt_mipmap_cache_static_dead_image) dt_free_align(buf->cache_entry->data);
    buf->cache_entry->data = dt_alloc_align(64, buffer_size);
    if(!buf->cache_entry->data)
    {
      // return fallback: at least alloc size for a dead image:
      buf->cache_entry->data = (void*)dt_mipmap_cache_static_dead_image;
      // allocator holds the pointer. but let imageio client know that allocation failed:
      return NULL;
    }
    // set buffer size only if we're making it larger.
    dsc = (struct dt_mipmap_buffer_dsc *)buf->cache_entry->data;
    dsc->size = buffer_size;
  }
  dsc->width = wd;
  dsc->height = ht;
  dsc->color_space = DT_COLORSPACE_NONE;
  dsc->flags = DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;
  buf->buf = (uint8_t *)(dsc + 1);

  // fprintf(stderr, "full buffer allocating img %u %d x %d = %u bytes (%p)\n", img->id, img->width,
  // img->height, buffer_size, *buf);

  // return pointer to start of payload
  return dsc + 1;
}

// callback for the cache backend to initialize payload pointers
void dt_mipmap_cache_allocate_dynamic(void *data, dt_cache_entry_t *entry)
{
  dt_mipmap_cache_t *cache = (dt_mipmap_cache_t *)data;
  // for full image buffers
  struct dt_mipmap_buffer_dsc *dsc = entry->data;
  const dt_mipmap_size_t mip = get_size(entry->key);
  // alloc mere minimum for the header + broken image buffer:
  if(!dsc)
  {
    if(mip <= DT_MIPMAP_F)
    {
      // these are fixed-size:
      entry->data = dt_alloc_align(16, cache->buffer_size[mip]);
    }
    else
    {
      entry->data = dt_alloc_align(16, sizeof(*dsc) + sizeof(float) * 4 * 64);
    }
    // fprintf(stderr, "[mipmap cache] alloc dynamic for key %u %p\n", key, *buf);
    if(!(entry->data))
    {
      fprintf(stderr, "[mipmap cache] memory allocation failed!\n");
      exit(1);
    }
    dsc = entry->data;
    if(mip <= DT_MIPMAP_F)
    {
      dsc->width = cache->max_width[mip];
      dsc->height = cache->max_height[mip];
      dsc->size = cache->buffer_size[mip];
      dsc->color_space = DT_COLORSPACE_NONE;
    }
    else
    {
      dsc->width = 0;
      dsc->height = 0;
      dsc->color_space = DT_COLORSPACE_NONE;
      dsc->size = sizeof(*dsc) + sizeof(float) * 4 * 64;
    }
  }
  assert(dsc->size >= sizeof(*dsc));

  int loaded_from_disk = 0;
  if(mip < DT_MIPMAP_F)
  {
    if(cache->cachedir[0] && dt_conf_get_bool("cache_disk_backend"))
    {
      // try and load from disk, if successful set flag
      char filename[PATH_MAX] = {0};
      snprintf(filename, sizeof(filename), "%s.d/%d/%d.jpg", cache->cachedir, mip, get_imgid(entry->key));
      FILE *f = fopen(filename, "rb");
      if(f)
      {
        long len = 0;
        uint8_t *blob = 0;
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        if(len <= 0) goto read_error; // coverity madness
        blob = (uint8_t *)malloc(len);
        if(!blob) goto read_error;
        fseek(f, 0, SEEK_SET);
        int rd = fread(blob, sizeof(uint8_t), len, f);
        if(rd != len) goto read_error;
        dt_colorspaces_color_profile_type_t color_space;
        dt_imageio_jpeg_t jpg;
        if(dt_imageio_jpeg_decompress_header(blob, len, &jpg)
           || (jpg.width > cache->max_width[mip] || jpg.height > cache->max_height[mip])
           || ((color_space = dt_imageio_jpeg_read_color_space(&jpg)) == DT_COLORSPACE_NONE) // pointless test to keep it in the if clause
           || dt_imageio_jpeg_decompress(&jpg, entry->data + sizeof(*dsc)))
        {
          fprintf(stderr, "[mipmap_cache] failed to decompress thumbnail for image %d from `%s'!\n", get_imgid(entry->key), filename);
          goto read_error;
        }
        dsc->width = jpg.width;
        dsc->height = jpg.height;
        dsc->color_space = color_space;
        loaded_from_disk = 1;
        if(0)
        {
read_error:
          g_unlink(filename);
        }
        free(blob);
        fclose(f);
      }
    }
  }

  if(!loaded_from_disk)
    dsc->flags = DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;
  else dsc->flags = 0;

  // cost is just flat one for the buffer, as the buffers might have different sizes,
  // to make sure quota is meaningful.
  if(mip >= DT_MIPMAP_F) entry->cost = 1;
  else entry->cost = cache->buffer_size[mip];
}

static void dt_mipmap_cache_unlink_ondisk_thumbnail(void *data, uint32_t imgid, dt_mipmap_size_t mip)
{
  dt_mipmap_cache_t *cache = (dt_mipmap_cache_t *)data;

  // also remove jpg backing (always try to do that, in case user just temporarily switched it off,
  // to avoid inconsistencies.
  // if(dt_conf_get_bool("cache_disk_backend"))
  if(cache->cachedir[0])
  {
    char filename[PATH_MAX] = { 0 };
    snprintf(filename, sizeof(filename), "%s.d/%d/%d.jpg", cache->cachedir, mip, imgid);
    g_unlink(filename);
  }
}

void dt_mipmap_cache_deallocate_dynamic(void *data, dt_cache_entry_t *entry)
{
  dt_mipmap_cache_t *cache = (dt_mipmap_cache_t *)data;
  const dt_mipmap_size_t mip = get_size(entry->key);
  if(mip < DT_MIPMAP_F)
  {
    struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)entry->data;
    // don't write skulls:
    if(dsc->width > 8 && dsc->height > 8)
    {
      if(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE)
      {
        dt_mipmap_cache_unlink_ondisk_thumbnail(data, get_imgid(entry->key), mip);
      }
      else if(cache->cachedir[0] && dt_conf_get_bool("cache_disk_backend"))
      {
        // serialize to disk
        char filename[PATH_MAX] = {0};
        snprintf(filename, sizeof(filename), "%s.d/%d", cache->cachedir, mip);
        int mkd = g_mkdir_with_parents(filename, 0750);
        if(!mkd)
        {
          snprintf(filename, sizeof(filename), "%s.d/%d/%d.jpg", cache->cachedir, mip, get_imgid(entry->key));
          // Don't write existing files as both performance and quality (lossy jpg) suffer
          FILE *f = NULL;
          if (!g_file_test(filename, G_FILE_TEST_EXISTS) && (f = fopen(filename, "wb")))
          {
            // first check the disk isn't full
            struct statvfs vfsbuf;
            if (!statvfs(filename, &vfsbuf))
            {
              int64_t free_mb = ((vfsbuf.f_frsize * vfsbuf.f_bavail) >> 20);
              if (free_mb < 100)
              {
                fprintf(stderr, "Aborting image write as only %" PRId64 " MB free to write %s\n", free_mb, filename);
                goto write_error;
              }
            }
            else
            {
              fprintf(stderr, "Aborting image write since couldn't determine free space available to write %s\n", filename);
              goto write_error;
            }

            const int cache_quality = dt_conf_get_int("database_cache_quality");
            const uint8_t *exif = NULL;
            int exif_len = 0;
            if(dsc->color_space == DT_COLORSPACE_SRGB)
            {
              exif = dt_mipmap_cache_exif_data_srgb;
              exif_len = dt_mipmap_cache_exif_data_srgb_length;
            }
            else if(dsc->color_space == DT_COLORSPACE_ADOBERGB)
            {
              exif = dt_mipmap_cache_exif_data_adobergb;
              exif_len = dt_mipmap_cache_exif_data_adobergb_length;
            }
            if(dt_imageio_jpeg_write(filename, entry->data + sizeof(*dsc), dsc->width, dsc->height, MIN(100, MAX(10, cache_quality)), exif, exif_len))
            {
write_error:
              g_unlink(filename);
            }
          }
          if(f) fclose(f);
        }
      }
    }
  }
  dt_free_align(entry->data);
}

static uint32_t nearest_power_of_two(const uint32_t value)
{
  uint32_t rc = 1;
  while(rc < value) rc <<= 1;
  return rc;
}

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache)
{
  dt_mipmap_cache_get_filename(cache->cachedir, sizeof(cache->cachedir));
  // make sure static memory is initialized
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)dt_mipmap_cache_static_dead_image;
  dead_image_f((dt_mipmap_buffer_t *)(dsc + 1));

  // adjust numbers to be large enough to hold what mem limit suggests.
  // we want at least 100MB, and consider 8G just still reasonable.
  int64_t cache_memory = dt_conf_get_int64("cache_memory");
  int worker_threads = dt_conf_get_int("worker_threads");
  size_t max_mem = CLAMPS(cache_memory, 100u << 20, ((uint64_t)8) << 30);
  const uint32_t parallel = CLAMP(worker_threads, 1, 8);

  // Fixed sizes for the thumbnail mip levels, selected for coverage of most screen sizes
  int32_t mipsizes[DT_MIPMAP_F][2] = {
    {180,  110},  // mip0 - ~1/2 size previous one
    {360,  225},  // mip1 - 1/2 size previous one
    {720,  450},  // mip2 - 1/2 size previous one
    {1440, 900},  // mip3 - covers 720p and 1366x768
    {1920, 1200}, // mip4 - covers 1080p and 1600x1200
    {2560, 1600}, // mip5 - covers 2560x1440
    {4096, 2560}, // mip6 - covers 4K and UHD
    {5120, 3200}, // mip7 - covers 5120x2880 panels
  };
  // Set mipf to mip2 size as at most the user will be using an 8K screen and
  // have a preview that's ~4x smaller
  cache->max_width[DT_MIPMAP_F] = mipsizes[DT_MIPMAP_2][0];
  cache->max_height[DT_MIPMAP_F] = mipsizes[DT_MIPMAP_2][1];
  for(int k = DT_MIPMAP_F-1; k >= 0; k--)
  {
    cache->max_width[k]  = mipsizes[k][0];
    cache->max_height[k] = mipsizes[k][1];
  }
    // header + buffer
  for(int k = DT_MIPMAP_F-1; k >= 0; k--)
    cache->buffer_size[k] = sizeof(struct dt_mipmap_buffer_dsc)
                                + cache->max_width[k] * cache->max_height[k] * 4;

  // clear stats:
  cache->mip_thumbs.stats_requests = 0;
  cache->mip_thumbs.stats_near_match = 0;
  cache->mip_thumbs.stats_misses = 0;
  cache->mip_thumbs.stats_fetches = 0;
  cache->mip_thumbs.stats_standin = 0;
  cache->mip_f.stats_requests = 0;
  cache->mip_f.stats_near_match = 0;
  cache->mip_f.stats_misses = 0;
  cache->mip_f.stats_fetches = 0;
  cache->mip_f.stats_standin = 0;
  cache->mip_full.stats_requests = 0;
  cache->mip_full.stats_near_match = 0;
  cache->mip_full.stats_misses = 0;
  cache->mip_full.stats_fetches = 0;
  cache->mip_full.stats_standin = 0;

  dt_cache_init(&cache->mip_thumbs.cache, 0, max_mem);
  dt_cache_set_allocate_callback(&cache->mip_thumbs.cache, dt_mipmap_cache_allocate_dynamic, cache);
  dt_cache_set_cleanup_callback(&cache->mip_thumbs.cache, dt_mipmap_cache_deallocate_dynamic, cache);

  const int full_entries
      = MAX(2, parallel); // even with one thread you want two buffers. one for dr one for thumbs.
  int32_t max_mem_bufs = nearest_power_of_two(full_entries);

  // for this buffer, because it can be very busy during import
  dt_cache_init(&cache->mip_full.cache, 0, max_mem_bufs);
  dt_cache_set_allocate_callback(&cache->mip_full.cache, dt_mipmap_cache_allocate_dynamic, cache);
  dt_cache_set_cleanup_callback(&cache->mip_full.cache, dt_mipmap_cache_deallocate_dynamic, cache);
  cache->buffer_size[DT_MIPMAP_FULL] = 0;

  // same for mipf:
  dt_cache_init(&cache->mip_f.cache, 0, max_mem_bufs);
  dt_cache_set_allocate_callback(&cache->mip_f.cache, dt_mipmap_cache_allocate_dynamic, cache);
  dt_cache_set_cleanup_callback(&cache->mip_f.cache, dt_mipmap_cache_deallocate_dynamic, cache);
  cache->buffer_size[DT_MIPMAP_F] = sizeof(struct dt_mipmap_buffer_dsc)
                                        + 4 * sizeof(float) * cache->max_width[DT_MIPMAP_F]
                                          * cache->max_height[DT_MIPMAP_F];
}

void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache)
{
  dt_cache_cleanup(&cache->mip_thumbs.cache);
  dt_cache_cleanup(&cache->mip_full.cache);
  dt_cache_cleanup(&cache->mip_f.cache);
}

void dt_mipmap_cache_print(dt_mipmap_cache_t *cache)
{
  printf("[mipmap_cache] thumbs fill %.2f/%.2f MB (%.2f%%)\n",
         cache->mip_thumbs.cache.cost / (1024.0 * 1024.0),
         cache->mip_thumbs.cache.cost_quota / (1024.0 * 1024.0),
         100.0f * (float)cache->mip_thumbs.cache.cost / (float)cache->mip_thumbs.cache.cost_quota);
  printf("[mipmap_cache] float fill %d/%d slots (%.2f%%)\n",
         (uint32_t)cache->mip_f.cache.cost, (uint32_t)cache->mip_f.cache.cost_quota,
         100.0f * (float)cache->mip_f.cache.cost / (float)cache->mip_f.cache.cost_quota);
  printf("[mipmap_cache] full  fill %d/%d slots (%.2f%%)\n",
         (uint32_t)cache->mip_full.cache.cost, (uint32_t)cache->mip_full.cache.cost_quota,
         100.0f * (float)cache->mip_full.cache.cost / (float)cache->mip_full.cache.cost_quota);

  uint64_t sum = 0;
  uint64_t sum_fetches = 0;
  uint64_t sum_standins = 0;
  sum += cache->mip_thumbs.stats_requests;
  sum_fetches += cache->mip_thumbs.stats_fetches;
  sum_standins += cache->mip_thumbs.stats_standin;
  sum += cache->mip_f.stats_requests;
  sum_fetches += cache->mip_f.stats_fetches;
  sum_standins += cache->mip_f.stats_standin;
  sum += cache->mip_full.stats_requests;
  sum_fetches += cache->mip_full.stats_fetches;
  sum_standins += cache->mip_full.stats_standin;
  printf("[mipmap_cache] level | near match | miss | stand-in | fetches | total rq\n");
  printf("[mipmap_cache] thumb | %6.2f%% | %6.2f%% | %6.2f%%  | %6.2f%% | %6.2f%%\n",
         100.0 * cache->mip_thumbs.stats_near_match / (float)cache->mip_thumbs.stats_requests,
         100.0 * cache->mip_thumbs.stats_misses / (float)cache->mip_thumbs.stats_requests,
         100.0 * cache->mip_thumbs.stats_standin / (float)sum_standins,
         100.0 * cache->mip_thumbs.stats_fetches / (float)sum_fetches,
         100.0 * cache->mip_thumbs.stats_requests / (float)sum);
  printf("[mipmap_cache] float | %6.2f%% | %6.2f%% | %6.2f%%  | %6.2f%% | %6.2f%%\n",
         100.0 * cache->mip_f.stats_near_match / (float)cache->mip_f.stats_requests,
         100.0 * cache->mip_f.stats_misses / (float)cache->mip_f.stats_requests,
         100.0 * cache->mip_f.stats_standin / (float)sum_standins,
         100.0 * cache->mip_f.stats_fetches / (float)sum_fetches,
         100.0 * cache->mip_f.stats_requests / (float)sum);
  printf("[mipmap_cache] full  | %6.2f%% | %6.2f%% | %6.2f%%  | %6.2f%% | %6.2f%%\n",
         100.0 * cache->mip_full.stats_near_match / (float)cache->mip_full.stats_requests,
         100.0 * cache->mip_full.stats_misses / (float)cache->mip_full.stats_requests,
         100.0 * cache->mip_full.stats_standin / (float)sum_standins,
         100.0 * cache->mip_full.stats_fetches / (float)sum_fetches,
         100.0 * cache->mip_full.stats_requests / (float)sum);
  printf("\n\n");
}

static gboolean _raise_signal_mipmap_updated(gpointer user_data)
{
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED);
  return FALSE; // only call once
}

static dt_mipmap_cache_one_t *_get_cache(dt_mipmap_cache_t *cache, const dt_mipmap_size_t mip)
{
  switch(mip)
  {
    case DT_MIPMAP_FULL:
      return &cache->mip_full;
    case DT_MIPMAP_F:
      return &cache->mip_f;
    default:
      return &cache->mip_thumbs;
  }
}

void dt_mipmap_cache_get_with_caller(
    dt_mipmap_cache_t *cache,
    dt_mipmap_buffer_t *buf,
    const uint32_t imgid,
    const dt_mipmap_size_t mip,
    const dt_mipmap_get_flags_t flags,
    const char mode,
    const char *file,
    int line)
{
  const uint32_t key = get_key(imgid, mip);
  if(flags == DT_MIPMAP_TESTLOCK)
  {
    // simple case: only get and lock if it's there.
    dt_cache_entry_t *entry = dt_cache_testget(&_get_cache(cache, mip)->cache, key, mode);
    buf->cache_entry = entry;
    if(entry)
    {
      struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)entry->data;
      buf->width = dsc->width;
      buf->height = dsc->height;
      buf->color_space = dsc->color_space;
      buf->imgid = imgid;
      buf->size = mip;
      buf->pre_monochrome_demosaiced = dsc->pre_monochrome_demosaiced;
      // skip to next 8-byte alignment, for sse buffers.
      buf->buf = (uint8_t *)(dsc + 1);
    }
    else
    {
      // set to NULL if failed.
      buf->width = buf->height = 0;
      buf->imgid = 0;
      buf->color_space = DT_COLORSPACE_NONE;
      buf->size = DT_MIPMAP_NONE;
      buf->buf = NULL;
    }
  }
  else if(flags == DT_MIPMAP_PREFETCH)
  {
    // and opposite: prefetch without locking
    if(mip > DT_MIPMAP_FULL || (int)mip < DT_MIPMAP_0)
      return; // remove the (int) once we no longer have to support gcc < 4.8 :/
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_FG, dt_image_load_job_create(imgid, mip));
  }
  else if(flags == DT_MIPMAP_PREFETCH_DISK)
  {
    // only prefetch if the disk cache exists:
    if(!cache->cachedir[0]) return;
    if(mip > DT_MIPMAP_FULL || (int)mip < DT_MIPMAP_0)
      return; // remove the (int) once we no longer have to support gcc < 4.8 :/
    char filename[PATH_MAX] = {0};
    snprintf(filename, sizeof(filename), "%s.d/%d/%d.jpg", cache->cachedir, mip, key);
    // don't attempt to load if disk cache doesn't exist
    if(!g_file_test(filename, G_FILE_TEST_EXISTS)) return;
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_FG, dt_image_load_job_create(imgid, mip));
  }
  else if(flags == DT_MIPMAP_BLOCKING)
  {
    // simple case: blocking get
    dt_cache_entry_t *entry =  dt_cache_get_with_caller(&_get_cache(cache, mip)->cache, key, mode, file, line);
    struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)entry->data;
    buf->cache_entry = entry;

    int mipmap_generated = 0;
    if(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE)
    {
      mipmap_generated = 1;
      // _init_f() will set it, if needed
      buf->pre_monochrome_demosaiced = 0;

      __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_fetches), 1);
      // fprintf(stderr, "[mipmap cache get] now initializing buffer for img %u mip %d!\n", imgid, mip);
      // we're write locked here, as requested by the alloc callback.
      // now fill it with data:
      if(mip == DT_MIPMAP_FULL)
      {
        // load the image:
        // make sure we access the r/w lock as shortly as possible!
        dt_image_t buffered_image;
        const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
        buffered_image = *cimg;
        // dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
        // dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
        dt_image_cache_read_release(darktable.image_cache, cimg);

        char filename[PATH_MAX] = { 0 };
        gboolean from_cache = TRUE;
        dt_image_full_path(buffered_image.id, filename, sizeof(filename), &from_cache);

        buf->imgid = imgid;
        buf->size = mip;
        buf->buf = 0;
        buf->width = buf->height = 0;
        buf->color_space = DT_COLORSPACE_NONE; // TODO: does the full buffer need to know this?
        dt_imageio_retval_t ret = dt_imageio_open(&buffered_image, filename, buf); // TODO: color_space?
        // might have been reallocated:
        dsc = (struct dt_mipmap_buffer_dsc *)buf->cache_entry->data;
        if(ret != DT_IMAGEIO_OK)
        {
          // fprintf(stderr, "[mipmap read get] error loading image: %d\n", ret);
          //
          // we can only return a zero dimension buffer if the buffer has been allocated.
          // in case dsc couldn't be allocated and points to the static buffer, it contains
          // a dead image already.
          if((void *)dsc != (void *)dt_mipmap_cache_static_dead_image)
          {
            dsc->width = dsc->height = 0;
            dsc->color_space = DT_COLORSPACE_NONE;
          }
        }
        else
        {
          // swap back new image data:
          dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
          *img = buffered_image;
          // fprintf(stderr, "[mipmap read get] initializing full buffer img %u with %u %u -> %d %d (%p)\n",
          // imgid, data[0], data[1], img->width, img->height, data);
          // don't write xmp for this (we only changed db stuff):
          dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
        }
      }
      else if(mip == DT_MIPMAP_F)
      {
        _init_f(buf, (float *)(dsc + 1), &dsc->width, &dsc->height, imgid);
      }
      else
      {
        // 8-bit thumbs
        _init_8((uint8_t *)(dsc + 1), &dsc->width, &dsc->height, &buf->color_space, imgid, mip);
      }
      dsc->pre_monochrome_demosaiced = buf->pre_monochrome_demosaiced;
      dsc->color_space = buf->color_space;
      dsc->flags &= ~DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;
    }

    // image cache is leaving the write lock in place in case the image has been newly allocated.
    // this leads to a slight increase in thread contention, so we opt for dropping the write lock
    // and acquiring a read lock immediately after. since this opens a small window for other threads
    // to get in between, we need to take some care to re-init cache entries and dsc.
    // note that concurrencykit has rw locks that can be demoted from w->r without losing the lock in between.
    if(mode == 'r')
    {
      entry->_lock_demoting = 1;
      // drop the write lock
      dt_cache_release(&_get_cache(cache, mip)->cache, entry);
      // get a read lock
      buf->cache_entry = entry = dt_cache_get(&_get_cache(cache, mip)->cache, key, mode);
      entry->_lock_demoting = 0;
      dsc = (struct dt_mipmap_buffer_dsc *)buf->cache_entry->data;
    }

#ifdef _DEBUG
    const pthread_t writer = dt_pthread_rwlock_get_writer(&(buf->cache_entry->lock));
    if(mode == 'w')
    {
      assert(pthread_equal(writer, pthread_self()));
    }
    else
    {
      assert(!pthread_equal(writer, pthread_self()));
    }
#endif

    if(mipmap_generated)
    {
      /* raise signal that mipmaps has been flushed to cache */
      g_idle_add(_raise_signal_mipmap_updated, 0);
    }

    buf->width = dsc->width;
    buf->height = dsc->height;
    buf->color_space = dsc->color_space;
    buf->imgid = imgid;
    buf->size = mip;
    buf->pre_monochrome_demosaiced = dsc->pre_monochrome_demosaiced;
    buf->buf = (uint8_t *)(dsc + 1);
    if(dsc->width == 0 || dsc->height == 0)
    {
      // fprintf(stderr, "[mipmap cache get] got a zero-sized image for img %u mip %d!\n", imgid, mip);
      if(mip < DT_MIPMAP_F)
        dead_image_8(buf);
      else if(mip == DT_MIPMAP_F)
        dead_image_f(buf);
      else
        buf->buf = NULL; // full images with NULL buffer have to be handled, indicates `missing image', but still return locked slot
    }
  }
  else if(flags == DT_MIPMAP_BEST_EFFORT)
  {
    __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_requests), 1);
    // best-effort, might also return NULL.
    // never decrease mip level for float buffer or full image:
    dt_mipmap_size_t min_mip = (mip >= DT_MIPMAP_F) ? mip : DT_MIPMAP_0;
    for(int k = mip; k >= min_mip && k >= 0; k--)
    {
      // already loaded?
      dt_mipmap_cache_get(cache, buf, imgid, k, DT_MIPMAP_TESTLOCK, 'r');
      if(buf->buf && buf->width > 0 && buf->height > 0)
      {
        if(mip != k) __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_standin), 1);
        return;
      }
      // didn't succeed the first time? prefetch for later!
      if(mip == k)
      {
        __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_near_match), 1);
        dt_mipmap_cache_get(cache, buf, imgid, mip, DT_MIPMAP_PREFETCH, 'r');
      }
    }
    // couldn't find a smaller thumb, try larger ones only now (these will be slightly slower due to cairo rescaling):
    dt_mipmap_size_t max_mip = (mip >= DT_MIPMAP_F) ? mip : DT_MIPMAP_F-1;
    for(int k = mip+1; k <= max_mip; k++)
    {
      // already loaded?
      dt_mipmap_cache_get(cache, buf, imgid, k, DT_MIPMAP_TESTLOCK, 'r');
      if(buf->buf && buf->width > 0 && buf->height > 0)
      {
        __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_standin), 1);
        return;
      }
    }
    __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_misses), 1);
    // in case we don't even have a disk cache for our requested thumbnail,
    // prefetch at least mip0, in case we have that in the disk caches:
    if(cache->cachedir[0])
    {
      char filename[PATH_MAX] = {0};
      snprintf(filename, sizeof(filename), "%s.d/%d/%d.jpg", cache->cachedir, mip, key);
      if(g_file_test(filename, G_FILE_TEST_EXISTS))
        dt_mipmap_cache_get(cache, 0, imgid, DT_MIPMAP_0, DT_MIPMAP_PREFETCH_DISK, 0);
    }
    // nothing found :(
    buf->buf = NULL;
    buf->imgid = 0;
    buf->size = DT_MIPMAP_NONE;
    buf->width = buf->height = 0;
    buf->color_space = DT_COLORSPACE_NONE;
  }
}

void dt_mipmap_cache_write_get_with_caller(dt_mipmap_cache_t *cache, dt_mipmap_buffer_t *buf, const uint32_t imgid, const int mip, const char *file, int line)
{
  dt_mipmap_cache_get_with_caller(cache, buf, imgid, mip, DT_MIPMAP_BLOCKING, 'w', file, line);
}

void dt_mipmap_cache_release(dt_mipmap_cache_t *cache, dt_mipmap_buffer_t *buf)
{
  if(buf->size == DT_MIPMAP_NONE) return;
  assert(buf->imgid > 0);
  // assert(buf->size >= DT_MIPMAP_0); // breaks gcc-4.6/4.7 build
  assert(buf->size < DT_MIPMAP_NONE);
  assert(buf->cache_entry);
  dt_cache_release(&_get_cache(cache, buf->size)->cache, buf->cache_entry);
  buf->size = DT_MIPMAP_NONE;
  buf->buf = NULL;
}


// return the closest mipmap size
dt_mipmap_size_t dt_mipmap_cache_get_matching_size(const dt_mipmap_cache_t *cache, const int32_t width,
                                                   const int32_t height)
{
  const double ppd = (darktable.gui != NULL) ? darktable.gui->ppd : 1.0;

  // find `best' match to width and height.
  int32_t error = 0x7fffffff;
  dt_mipmap_size_t best = DT_MIPMAP_NONE;
  for(int k = DT_MIPMAP_0; k < DT_MIPMAP_F; k++)
  {
    // find closest l1 norm:
    int32_t new_error = cache->max_width[k] + cache->max_height[k] - width * ppd - height * ppd;
    // and allow the first one to be larger in pixel size to override the smaller mip
    if(abs(new_error) < abs(error) || (error < 0 && new_error > 0))
    {
      best = k;
      error = new_error;
    }
  }
  return best;
}

void dt_mipmap_cache_remove(dt_mipmap_cache_t *cache, const uint32_t imgid)
{
  // get rid of all ldr thumbnails:

  for(dt_mipmap_size_t k = DT_MIPMAP_0; k < DT_MIPMAP_F; k++)
  {
    const uint32_t key = get_key(imgid, k);
    dt_cache_entry_t *entry = dt_cache_testget(&_get_cache(cache, k)->cache, key, 'w');
    if(entry)
    {
      struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)entry->data;
      dsc->flags |= DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE;
      dt_cache_release(&_get_cache(cache, k)->cache, entry);

      // due to DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE, removes thumbnail from disc
      dt_cache_remove(&_get_cache(cache, k)->cache, key);
    }
    else
    {
      // ugly, but avoids alloc'ing thumb if it is not there.
      dt_mipmap_cache_unlink_ondisk_thumbnail((&_get_cache(cache, k)->cache)->cleanup_data, imgid, k);
    }
  }
}

void dt_mimap_cache_evict(dt_mipmap_cache_t *cache, const uint32_t imgid)
{
  for(dt_mipmap_size_t k = DT_MIPMAP_0; k < DT_MIPMAP_F; k++)
  {
    const uint32_t key = get_key(imgid, k);

    // write thumbnail to disc if not existing there
    dt_cache_remove(&_get_cache(cache, k)->cache, key);
  }
}

int dt_image_get_demosaic_method(const int imgid, const char **method_name)
{
  // find the demosaic module -- the pointer stays valid until darktable shuts down
  static dt_iop_module_so_t *demosaic = NULL;
  if(demosaic == NULL)
  {
    GList *modules = g_list_first(darktable.iop);
    while(modules)
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);
      if(!strcmp(module->op, "demosaic"))
      {
        demosaic = module;
        break;
      }
      modules = g_list_next(modules);
    }
  }

  int method = 0; // normal demosaic, DT_IOP_DEMOSAIC_PPG

  // db lookup demosaic params
  if(demosaic && demosaic->get_f && demosaic->get_p)
  {
    dt_introspection_field_t *field = demosaic->get_f("demosaicing_method");

    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "SELECT op_params FROM history WHERE imgid=?1 AND operation='demosaic' ORDER BY num DESC LIMIT 1", -1,
        &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      // use introspection to get the orientation from the binary params blob
      const void *params = sqlite3_column_blob(stmt, 0);
      method = *((int *)demosaic->get_p(params, "demosaicing_method"));
    }
    sqlite3_finalize(stmt);

    if(method_name) *method_name = field->Enum.values[method].name;
  }

  return method;
}

static void _init_f(dt_mipmap_buffer_t *mipmap_buf, float *out, uint32_t *width, uint32_t *height,
                    const uint32_t imgid)
{
  const uint32_t wd = *width, ht = *height;

  static int demosaic_monochrome_enum = -1;
  if(demosaic_monochrome_enum == -1)
  {
    for(GList *modules = g_list_first(darktable.iop); modules; modules = g_list_next(modules))
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);
      if(!strcmp(module->op, "demosaic"))
      {
        if(module->get_f)
        {
          dt_introspection_field_t *f = module->get_f("demosaicing_method");
          if(f && f->header.type == DT_INTROSPECTION_TYPE_ENUM)
          {
            for(dt_introspection_type_enum_tuple_t *value = f->Enum.values; value->name; value++)
            {
              if(!strcmp(value->name, "DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME"))
              {
                demosaic_monochrome_enum = value->value;
                break;
              }
            }
          }
        }

        if(demosaic_monochrome_enum == -1)
        {
          // oh, missing introspection for demosaic,
          // let's hope that DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME is still 3
          demosaic_monochrome_enum = 3;
        }

        break;
      }
    }
  }

  /* do not even try to process file if it isn't available */
  char filename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);
  if(!*filename || !g_file_test(filename, G_FILE_TEST_EXISTS))
  {
    *width = *height = 0;
    return;
  }

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  // lock image after we have the buffer, we might need to lock the image struct for
  // writing during raw loading, to write to width/height.
  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');

  dt_iop_roi_t roi_in, roi_out;
  roi_in.x = roi_in.y = 0;
  roi_in.width = image->width;
  roi_in.height = image->height;
  roi_in.scale = 1.0f;

  roi_out.x = roi_out.y = 0;
  roi_out.scale = fminf(wd / (float)image->width, ht / (float)image->height);
  roi_out.width = roi_out.scale * roi_in.width;
  roi_out.height = roi_out.scale * roi_in.height;

  if(!buf.buf)
  {
    dt_control_log(_("image `%s' is not available!"), image->filename);
    dt_image_cache_read_release(darktable.image_cache, image);
    *width = *height = 0;
    return;
  }

  assert(!buffer_is_broken(&buf));

  mipmap_buf->pre_monochrome_demosaiced = 0;
  mipmap_buf->color_space = DT_COLORSPACE_NONE; // TODO: do we need that information in this buffer?

  if(image->filters)
  {
    // demosaic during downsample
    if(image->filters != 9u)
    {
      const char *method_name = NULL;
      const int method = dt_image_get_demosaic_method(imgid, &(method_name));
      mipmap_buf->pre_monochrome_demosaiced
          = (method == demosaic_monochrome_enum
             && (method_name && !strcmp(method_name, "DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME")));

      // Bayer
      if(image->bpp == sizeof(float))
      {
        if(mipmap_buf->pre_monochrome_demosaiced)
        {
          dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f(out, (const float *)buf.buf, &roi_out,
                                                                 &roi_in, roi_out.width, roi_in.width);
        }
        else
        {
          dt_iop_clip_and_zoom_demosaic_half_size_crop_blacks_f(
              out, (const float *)buf.buf, &roi_out, &roi_in, roi_out.width, roi_in.width, image, 1.0f);
        }
      }
      else
      {
        if(mipmap_buf->pre_monochrome_demosaiced)
        {
          dt_iop_clip_and_zoom_demosaic_passthrough_monochrome(out, (const uint16_t *)buf.buf, &roi_out,
                                                               &roi_in, roi_out.width, roi_in.width);
        }
        else
        {
          dt_iop_clip_and_zoom_demosaic_half_size_crop_blacks(out, (const uint16_t *)buf.buf, &roi_out,
                                                              &roi_in, roi_out.width, roi_in.width, image);
        }
      }
    }
    else
    {
      // X-Trans
      if(image->bpp == sizeof(float))
      {
        dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f(out, (const float *)buf.buf, &roi_out, &roi_in,
                                                          roi_out.width, roi_in.width,
                                                          image->xtrans_uncropped);
      }
      else
      {
        dt_iop_clip_and_zoom_demosaic_third_size_xtrans(out, (const uint16_t *)buf.buf, &roi_out, &roi_in,
                                                        roi_out.width, roi_in.width, image->xtrans_uncropped);
      }
    }
  }
  else
  {
    // downsample
    dt_iop_clip_and_zoom(out, (const float *)buf.buf, &roi_out, &roi_in, roi_out.width, roi_in.width);
  }
  dt_image_cache_read_release(darktable.image_cache, image);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  *width = roi_out.width;
  *height = roi_out.height;
}


// dummy functions for `export' to mipmap buffers:
typedef struct _dummy_data_t
{
  dt_imageio_module_data_t head;
  uint8_t *buf;
} _dummy_data_t;

static int _levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static int _bpp(dt_imageio_module_data_t *data)
{
  return 8;
}

static int _write_image(dt_imageio_module_data_t *data, const char *filename, const void *in, void *exif,
                        int exif_len, int imgid, int num, int total)
{
  _dummy_data_t *d = (_dummy_data_t *)data;
#if 1
  // XXX this is stupid. we'd much rather have proper output spaces in gamma.c
  uint32_t *ibuf = (uint32_t *)in;
  for(int i=0;i<data->width*data->height;i++)
  {
    const uint32_t b = (ibuf[i] >> 22) & 0xff;
    const uint32_t g = (ibuf[i] >> 12) & 0xff;
    const uint32_t r = (ibuf[i] >>  2) & 0xff;
    ((uint32_t *)d->buf)[i] = (r<<16)|(g<<8)|b;
  }
#else
  memcpy(d->buf, in, data->width * data->height * sizeof(uint32_t));
#endif
  return 0;
}

static void _init_8(uint8_t *buf, uint32_t *width, uint32_t *height, dt_colorspaces_color_profile_type_t *color_space, const uint32_t imgid,
                    const dt_mipmap_size_t size)
{
  const uint32_t wd = *width, ht = *height;
  char filename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;

  /* do not even try to process file if it isnt available */
  dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);
  if(!*filename || !g_file_test(filename, G_FILE_TEST_EXISTS))
  {
    *width = *height = 0;
    *color_space = DT_COLORSPACE_NONE;
    return;
  }

  const int altered = dt_image_altered(imgid);
  int res = 1;

  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  // the orientation for this camera is not read correctly from exiv2, so we need
  // to go the full path (as the thumbnail will be flipped the wrong way round)
  const int incompatible = !strncmp(cimg->exif_maker, "Phase One", 9);
  dt_image_cache_read_release(darktable.image_cache, cimg);

  if(!altered && !dt_conf_get_bool("never_use_embedded_thumb") && !incompatible)
  {
    const dt_image_orientation_t orientation = dt_image_get_orientation(imgid);

    // try to load the embedded thumbnail in raw
    gboolean from_cache = TRUE;
    memset(filename, 0, sizeof(filename));
    dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);

    const char *c = filename + strlen(filename);
    while(*c != '.' && c > filename) c--;
    if(!strcasecmp(c, ".jpg"))
    {
      // try to load jpg
      dt_imageio_jpeg_t jpg;
      if(!dt_imageio_jpeg_read_header(filename, &jpg))
      {
        uint8_t *tmp = (uint8_t *)malloc(sizeof(uint8_t) * jpg.width * jpg.height * 4);
        *color_space = dt_imageio_jpeg_read_color_space(&jpg);
        if(!dt_imageio_jpeg_read(&jpg, tmp))
        {
          // scale to fit
          dt_iop_flip_and_zoom_8(tmp, jpg.width, jpg.height, buf, wd, ht, orientation, width, height);
          res = 0;
        }
        free(tmp);
      }
    }
    else
    {
      uint8_t *tmp = 0;
      int32_t thumb_width, thumb_height;
      res = dt_imageio_large_thumbnail(filename, &tmp, &thumb_width, &thumb_height, color_space);
      if(!res)
      {
        // scale to fit
        dt_iop_flip_and_zoom_8(tmp, thumb_width, thumb_height, buf, wd, ht, orientation, width, height);
        free(tmp);
      }
    }
  }

  if(res)
  {
    //try to generate mip from larger mip
    for(dt_mipmap_size_t k = size + 1; k <= DT_MIPMAP_7; k++)
    {
      dt_mipmap_buffer_t tmp;
      dt_mipmap_cache_get(darktable.mipmap_cache, &tmp, imgid, k, DT_MIPMAP_TESTLOCK, 'r');
      if(tmp.buf == NULL)
        continue;
      dt_print(DT_DEBUG_CACHE, "[_init_8] generate mip %d for %s from level %d\n", size, filename, k);
      //keep aspect ratio
      float scale = MIN(wd/(float)tmp.width, ht/(float)tmp.height);
      *width = tmp.width*scale;
      *height = tmp.height*scale;
      *color_space = tmp.color_space;
      // downsample
      dt_iop_clip_and_zoom_8(tmp.buf, 0, 0, tmp.width, tmp.height, tmp.width, tmp.height, buf, 0, 0, *width, *height, *width, *height);
      dt_mipmap_cache_release(darktable.mipmap_cache, &tmp);
      res = 0;
      break;
    }
  }

  if(res)
  {
    // try the real thing: rawspeed + pixelpipe
    dt_imageio_module_format_t format;
    _dummy_data_t dat;
    format.bpp = _bpp;
    format.write_image = _write_image;
    format.levels = _levels;
    dat.head.max_width = wd;
    dat.head.max_height = ht;
    dat.buf = buf;
    // export with flags: ignore exif (don't load from disk), don't swap byte order, don't do hq processing,
    // no upscaling and signal we want thumbnail export
    res = dt_imageio_export_with_flags(imgid, "unused", &format, (dt_imageio_module_data_t *)&dat, 1, 1, 0, 0, 1,
                                       NULL, FALSE, NULL, NULL, 1, 1);
    if(!res)
    {
      // might be smaller, or have a different aspect than what we got as input.
      *width = dat.head.width;
      *height = dat.head.height;
      *color_space = dt_mipmap_cache_get_colorspace();
    }
  }

  // fprintf(stderr, "[mipmap init 8] export image %u finished (sizes %d %d => %d %d)!\n", imgid, wd, ht,
  // dat.head.width, dat.head.height);

  // any errors?
  if(res)
  {
    // fprintf(stderr, "[mipmap_cache] could not process thumbnail!\n");
    *width = *height = 0;
    *color_space = DT_COLORSPACE_NONE;
    return;
  }

  // TODO: various speed optimizations:
  // TODO: also init all smaller mips!
  // TODO: use mipf, but:
  // TODO: if output is cropped, don't use mipf!
}

dt_colorspaces_color_profile_type_t dt_mipmap_cache_get_colorspace()
{
  if(dt_conf_get_bool("cache_color_managed"))
    return DT_COLORSPACE_ADOBERGB;
  return DT_COLORSPACE_DISPLAY;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
