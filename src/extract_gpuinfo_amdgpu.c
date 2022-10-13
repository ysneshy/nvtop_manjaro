/*
 * Copyright (C) 2012 Lauri Kasanen
 * Copyright (C) 2018 Genesis Cloud Ltd.
 * Copyright (C) 2022 YiFei Zhu <zhuyifei1999@gmail.com>
 * Copyright (C) 2022 Maxime Schmitt <maxime.schmitt91@gmail.com>
 *
 * This file is part of Nvtop and adapted from radeontop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "nvtop/common.h"
#include "nvtop/extract_gpuinfo_common.h"
#include "nvtop/extract_processinfo_fdinfo.h"
#include "nvtop/time.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>
#include <linux/kcmp.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <uthash.h>
#include <xf86drm.h>

// Local function pointers to DRM interface
static typeof(drmGetDevices) *_drmGetDevices;
static typeof(drmGetDevices2) *_drmGetDevices2;
static typeof(drmFreeDevices) *_drmFreeDevices;
static typeof(drmGetVersion) *_drmGetVersion;
static typeof(drmFreeVersion) *_drmFreeVersion;
static typeof(drmGetMagic) *_drmGetMagic;
static typeof(drmAuthMagic) *_drmAuthMagic;
static typeof(drmDropMaster) *_drmDropMaster;

// Local function pointers to amdgpu DRM interface
static typeof(amdgpu_device_initialize) *_amdgpu_device_initialize;
static typeof(amdgpu_device_deinitialize) *_amdgpu_device_deinitialize;
static typeof(amdgpu_get_marketing_name) *_amdgpu_get_marketing_name;
static typeof(amdgpu_query_gpu_info) *_amdgpu_query_gpu_info;
static typeof(amdgpu_query_info) *_amdgpu_query_info;
static typeof(amdgpu_query_sensor_info) *_amdgpu_query_sensor_info;

static void *libdrm_handle;
static void *libdrm_amdgpu_handle;

static int last_libdrm_return_status = 0;
static char didnt_call_gpuinfo_init[] = "uninitialized";
static const char *local_error_string = didnt_call_gpuinfo_init;

#define HASH_FIND_CLIENT(head, key_ptr, out_ptr) HASH_FIND(hh, head, key_ptr, sizeof(unsigned), out_ptr)

#define HASH_ADD_CLIENT(head, in_ptr) HASH_ADD(hh, head, client_id, sizeof(unsigned), in_ptr)

#define SET_AMDGPU_CACHE(cachePtr, field, value) SET_VALUE(cachePtr, field, value, amdgpu_cache_)
#define RESET_AMDGPU_CACHE(cachePtr, field) INVALIDATE_VALUE(cachePtr, field, amdgpu_cache_)
#define AMDGPU_CACHE_FIELD_VALID(cachePtr, field) VALUE_IS_VALID(cachePtr, field, amdgpu_cache_)

enum amdgpu_process_info_cache_valid {
  amdgpu_cache_gfx_engine_used_valid = 0,
  amdgpu_cache_compute_engine_used_valid,
  amdgpu_cache_enc_engine_used_valid,
  amdgpu_cache_dec_engine_used_valid,
  amdgpu_cache_process_info_cache_valid_count
};

struct __attribute__((__packed__)) unique_cache_id {
  unsigned client_id;
  pid_t pid;
};

struct amdgpu_process_info_cache {
  struct unique_cache_id client_id;
  uint64_t gfx_engine_used;
  uint64_t compute_engine_used;
  uint64_t enc_engine_used;
  uint64_t dec_engine_used;
  nvtop_time last_measurement_tstamp;
  unsigned char valid[(amdgpu_cache_process_info_cache_valid_count + CHAR_BIT - 1) / CHAR_BIT];
  UT_hash_handle hh;
};

#define PDEV_LEN 20

struct gpu_info_amdgpu {
  struct gpu_info base;

  drmVersionPtr drmVersion;
  int fd;
  amdgpu_device_handle amdgpu_device;

  char pdev[PDEV_LEN];
  int sysfsFD; // file descriptor for the /sys/bus/pci/devices/<this gpu>/ folder
  int hwmonFD; // file descriptor for the /sys/bus/pci/devices/<this gpu>/hwmon/hwmon[0-9]+ folder

  // We poll the fan frequently enough and want to avoid the open/close overhead of the sysfs file
  FILE *fanSpeedFILE; // FILE* for this device current fan speed
  FILE *PCIeDPM; // FILE* for this device valid and active PCIe speed/width configurations
  FILE *PCIeBW; // FILE* for this device PCIe bandwidth over one second
  FILE *powerCap; // FILE* for this device power cap
  struct amdgpu_process_info_cache *last_update_process_cache, *current_update_process_cache; // Cached processes info

  // Used to compute the actual fan speed
  unsigned maxFanValue;
};

unsigned amdgpu_count;
static struct gpu_info_amdgpu *gpu_infos;

static bool gpuinfo_amdgpu_init(void);
static void gpuinfo_amdgpu_shutdown(void);
static const char *gpuinfo_amdgpu_last_error_string(void);
static bool gpuinfo_amdgpu_get_device_handles(
    struct list_head *devices, unsigned *count,
    ssize_t *mask);
static void gpuinfo_amdgpu_populate_static_info(struct gpu_info *_gpu_info);
static void gpuinfo_amdgpu_refresh_dynamic_info(struct gpu_info *_gpu_info);
static void gpuinfo_amdgpu_get_running_processes(struct gpu_info *_gpu_info);

struct gpu_vendor gpu_vendor_amdgpu = {
    .init = gpuinfo_amdgpu_init,
    .shutdown = gpuinfo_amdgpu_shutdown,
    .last_error_string = gpuinfo_amdgpu_last_error_string,
    .get_device_handles = gpuinfo_amdgpu_get_device_handles,
    .populate_static_info = gpuinfo_amdgpu_populate_static_info,
    .refresh_dynamic_info = gpuinfo_amdgpu_refresh_dynamic_info,
    .refresh_running_processes = gpuinfo_amdgpu_get_running_processes,
};

__attribute__((constructor))
static void init_extract_gpuinfo_amdgpu(void) {
  register_gpu_vendor(&gpu_vendor_amdgpu);
}

static int wrap_drmGetDevices(drmDevicePtr devices[], int max_devices) {
  assert(_drmGetDevices2 || _drmGetDevices);

  if (_drmGetDevices2)
    return _drmGetDevices2(0, devices, max_devices);
  return _drmGetDevices(devices, max_devices);
}

static bool parse_drm_fdinfo_amd(struct gpu_info *info, FILE *fdinfo_file, struct gpu_process *process_info);

static bool gpuinfo_amdgpu_init(void) {
  libdrm_handle = dlopen("libdrm.so", RTLD_LAZY);
  if (!libdrm_handle)
    libdrm_handle = dlopen("libdrm.so.2", RTLD_LAZY);
  if (!libdrm_handle)
    libdrm_handle = dlopen("libdrm.so.1", RTLD_LAZY);
  if (!libdrm_handle) {
    local_error_string = dlerror();
    return false;
  }

  _drmGetDevices2 = dlsym(libdrm_handle, "drmGetDevices2");
  if (!_drmGetDevices2)
    _drmGetDevices = dlsym(libdrm_handle, "drmGetDevices");
  if (!_drmGetDevices2 && !_drmGetDevices)
    goto init_error_clean_exit;

  _drmFreeDevices = dlsym(libdrm_handle, "drmFreeDevices");
  if (!_drmFreeDevices)
    goto init_error_clean_exit;

  _drmGetVersion = dlsym(libdrm_handle, "drmGetVersion");
  if (!_drmGetVersion)
    goto init_error_clean_exit;

  _drmFreeVersion = dlsym(libdrm_handle, "drmFreeVersion");
  if (!_drmFreeVersion)
    goto init_error_clean_exit;

  _drmGetMagic = dlsym(libdrm_handle, "drmGetMagic");
  if (!_drmGetMagic)
    goto init_error_clean_exit;

  _drmAuthMagic = dlsym(libdrm_handle, "drmAuthMagic");
  if (!_drmAuthMagic)
    goto init_error_clean_exit;

  _drmDropMaster = dlsym(libdrm_handle, "drmDropMaster");
  if (!_drmDropMaster)
    goto init_error_clean_exit;

  libdrm_amdgpu_handle = dlopen("libdrm_amdgpu.so", RTLD_LAZY);
  if (!libdrm_amdgpu_handle)
    libdrm_amdgpu_handle = dlopen("libdrm_amdgpu.so.1", RTLD_LAZY);

  if (libdrm_amdgpu_handle) {
    _amdgpu_device_initialize = dlsym(libdrm_amdgpu_handle, "amdgpu_device_initialize");
    _amdgpu_device_deinitialize = dlsym(libdrm_amdgpu_handle, "amdgpu_device_deinitialize");
    _amdgpu_get_marketing_name = dlsym(libdrm_amdgpu_handle, "amdgpu_get_marketing_name");
    _amdgpu_query_info = dlsym(libdrm_amdgpu_handle, "amdgpu_query_info");
    _amdgpu_query_gpu_info = dlsym(libdrm_amdgpu_handle, "amdgpu_query_gpu_info");
    _amdgpu_query_sensor_info = dlsym(libdrm_amdgpu_handle, "amdgpu_query_sensor_info");
  }

  local_error_string = NULL;
  return true;

init_error_clean_exit:
  dlclose(libdrm_handle);
  libdrm_handle = NULL;
  return false;
}

static void gpuinfo_amdgpu_shutdown(void) {
  for (unsigned i = 0; i < amdgpu_count; ++i) {
    struct gpu_info_amdgpu *gpu_info = &gpu_infos[i];
    if (gpu_info->fanSpeedFILE)
      fclose(gpu_info->fanSpeedFILE);
    if (gpu_info->PCIeDPM)
      fclose(gpu_info->PCIeDPM);
    if (gpu_info->PCIeBW)
      fclose(gpu_info->PCIeBW);
    if (gpu_info->powerCap)
      fclose(gpu_info->powerCap);
    _drmFreeVersion(gpu_info->drmVersion);
    _amdgpu_device_deinitialize(gpu_info->amdgpu_device);
    // Clean the process cache
    struct amdgpu_process_info_cache *cache_entry, *cache_tmp;
    HASH_ITER(hh, gpu_info->last_update_process_cache, cache_entry, cache_tmp) {
      HASH_DEL(gpu_info->last_update_process_cache, cache_entry);
      free(cache_entry);
    }
  }
  free(gpu_infos);
  gpu_infos = NULL;
  amdgpu_count = 0;

  if (libdrm_handle) {
    dlclose(libdrm_handle);
    libdrm_handle = NULL;
    local_error_string = didnt_call_gpuinfo_init;
  }

  if (libdrm_amdgpu_handle) {
    dlclose(libdrm_amdgpu_handle);
    libdrm_amdgpu_handle = NULL;
  }
}

static const char *gpuinfo_amdgpu_last_error_string(void) {
  if (local_error_string) {
    return local_error_string;
  } else if (last_libdrm_return_status < 0) {
    switch (last_libdrm_return_status) {
    case DRM_ERR_NO_DEVICE:
      return "no device\n";
    case DRM_ERR_NO_ACCESS:
      return "no access\n";
    case DRM_ERR_NOT_ROOT:
      return "not root\n";
    case DRM_ERR_INVALID:
      return "invalid args\n";
    case DRM_ERR_NO_FD:
      return "no fd\n";
    default:
      return "unknown error\n";
    }
  } else {
    return "An unanticipated error occurred while accessing AMDGPU "
           "information\n";
  }
}

static void authenticate_drm(int fd) {
  drm_magic_t magic;

  if (_drmGetMagic(fd, &magic) < 0) {
    return;
  }

  if (_drmAuthMagic(fd, magic) == 0) {
    if (_drmDropMaster(fd)) {
      perror("Failed to drop DRM master");
      fprintf(stderr, "\nWARNING: other DRM clients will crash on VT switch while nvtop is running!\npress ENTER to continue\n");
      fgetc(stdin);
    }
    return;
  }

  // XXX: Ideally I'd implement this too, but I'd need to pull in libxcb and yet
  // more functions and structs that may break ABI compatibility.
  // See radeontop auth_xcb.c for what is involved here
  fprintf(stderr, "Failed to authenticate to DRM; XCB authentication unimplemented\n");
}

static void initDeviceSysfsPaths(struct gpu_info_amdgpu *gpu_info) {
  // Open the device sys folder to gather information not available through the DRM driver
  char devicePath[22 + PDEV_LEN];
  snprintf(devicePath, sizeof(devicePath), "/sys/bus/pci/devices/%s", gpu_info->pdev);
  gpu_info->sysfsFD = open(devicePath, O_RDONLY);
  gpu_info->hwmonFD = -1;

  // Open the device hwmon folder (Fan speed are available there)
  static const char hwmon[] = "hwmon";
  if (gpu_info->sysfsFD >= 0) {
    int hwmondirFD = openat(gpu_info->sysfsFD, hwmon, O_RDONLY);
    if (hwmondirFD >= 0) {
      DIR *hwmonDir = fdopendir(hwmondirFD);
      if (!hwmonDir) {
        close(hwmondirFD);
      } else {
        struct dirent *dirEntry;
        while ((dirEntry = readdir(hwmonDir))) {
          // There should be one directory inside hwmon, with a name having the following pattern hwmon[0-9]+
          if (dirEntry->d_type == DT_DIR && strncmp(hwmon, dirEntry->d_name, sizeof(hwmon) - 1) == 0) {
            break;
          }
        }
        if (dirEntry) {
          gpu_info->hwmonFD = openat(dirfd(hwmonDir), dirEntry->d_name, O_RDONLY);
        }
        closedir(hwmonDir);
      }
    }
  }
}

#define VENDOR_AMD 0x1002

static bool gpuinfo_amdgpu_get_device_handles(
    struct list_head *devices, unsigned *count,
    ssize_t *mask) {
  if (!libdrm_handle)
    return false;

  last_libdrm_return_status = wrap_drmGetDevices(NULL, 0);
  if (last_libdrm_return_status <= 0)
    return false;

  drmDevicePtr devs[last_libdrm_return_status];
  last_libdrm_return_status = wrap_drmGetDevices(devs, last_libdrm_return_status);
  if (last_libdrm_return_status <= 0)
    return false;

  unsigned int libdrm_count = last_libdrm_return_status;
  gpu_infos = calloc(libdrm_count, sizeof(*gpu_infos));
  if (!gpu_infos) {
    local_error_string = strerror(errno);
    return false;
  }

  for (unsigned int i = 0; i < libdrm_count; i++) {
    if (devs[i]->bustype != DRM_BUS_PCI ||
        devs[i]->deviceinfo.pci->vendor_id != VENDOR_AMD)
      continue;

    int fd = -1;

    // Try render node first
    if (1 << DRM_NODE_RENDER & devs[i]->available_nodes) {
      fd = open(devs[i]->nodes[DRM_NODE_RENDER], O_RDWR);
    }
    if (fd < 0) {
      // Fallback to primary node (control nodes are unused according to the DRM documentation)
      if (1 << DRM_NODE_PRIMARY & devs[i]->available_nodes) {
        fd = open(devs[i]->nodes[DRM_NODE_PRIMARY], O_RDWR);
      }
    }

    if (fd < 0)
      continue;

    drmVersionPtr ver = _drmGetVersion(fd);

    if (!ver) {
      close(fd);
      continue;
    }

    bool is_radeon = false; // TODO: !strcmp(ver->name, "radeon");
    bool is_amdgpu = !strcmp(ver->name, "amdgpu");

    if (!is_amdgpu && !is_radeon) {
      _drmFreeVersion(ver);
      close(fd);
      continue;
    }

    if ((*mask & 1) == 0) {
      *mask >>= 1;
      _drmFreeVersion(ver);
      close(fd);
      continue;
    }
    *mask >>= 1;

    authenticate_drm(fd);

    if (is_amdgpu) {
      if (!libdrm_amdgpu_handle || !_amdgpu_device_initialize) {
        _drmFreeVersion(ver);
        close(fd);
        continue;
      }

      uint32_t drm_major, drm_minor;
      last_libdrm_return_status =
          _amdgpu_device_initialize(fd, &drm_major, &drm_minor, &gpu_infos[amdgpu_count].amdgpu_device);
    } else {
      // TODO: radeon suppport here
      assert(false);
    }

    if (!last_libdrm_return_status) {
      gpu_infos[amdgpu_count].drmVersion = ver;
      gpu_infos[amdgpu_count].fd = fd;
      gpu_infos[amdgpu_count].base.vendor = &gpu_vendor_amdgpu;

      snprintf(gpu_infos[*count].pdev, PDEV_LEN - 1, "%04x:%02x:%02x.%d",
               devs[i]->businfo.pci->domain,
               devs[i]->businfo.pci->bus,
               devs[i]->businfo.pci->dev,
               devs[i]->businfo.pci->func);
      initDeviceSysfsPaths(&gpu_infos[amdgpu_count]);
      list_add_tail(&gpu_infos[amdgpu_count].base.list, devices);
      // Register a fdinfo callback for this GPU
      processinfo_register_fdinfo_callback(parse_drm_fdinfo_amd, &gpu_infos[amdgpu_count].base);
      amdgpu_count++;
    } else {
      _drmFreeVersion(ver);
      close(fd);
      continue;
    }
  }

  _drmFreeDevices(devs, libdrm_count);
  *count = amdgpu_count;

  return true;
}

static int rewindAndReadPattern(FILE *file, const char *format, ...) {
  va_list args;
  va_start(args, format);
  rewind(file);
  fflush(file);
  int matches = vfscanf(file, format, args);
  va_end(args);
  return matches;
}

static int readValueFromFileAt(int folderFD, const char *fileName, const char *format, ...) {
  va_list args;
  va_start(args, format);
  // Open the file
  int fd = openat(folderFD, fileName, O_RDONLY);
  if (fd < 0)
    return 0;
  FILE *file = fdopen(fd, "r");
  if (!file) {
    close(fd);
    return 0;
  }
  // Read the pattern
  int nread = vfscanf(file, format, args);
  fclose(file);
  va_end(args);
  return nread;
}

// Converts the link speed in GT/s to a PCIe generation
static unsigned pcieGenFromLinkSpeedAndWidth(unsigned linkSpeed) {
  unsigned pcieGen = 0;
  switch (linkSpeed) {
  case 2:
    pcieGen = 1;
    break;
  case 5:
    pcieGen = 2;
    break;
  case 8:
    pcieGen = 3;
    break;
  case 16:
    pcieGen = 4;
    break;
  case 32:
    pcieGen = 5;
    break;
  case 64:
    pcieGen = 6;
    break;
  }
  return pcieGen;
}

static bool getGenAndWidthFromPP_DPM_PCIE(FILE *pp_dpm_pcie, unsigned *speed, unsigned *width) {
  rewind(pp_dpm_pcie);
  fflush(pp_dpm_pcie);
  // The line we are interested in looks like "1: 8.0GT/s, x16 619Mhz *"; the active configuration ends with a star
  char line[64]; // 64 should be plenty enough
  while (fgets(line, 64, pp_dpm_pcie)) {
    // Look for a * character, with possible spece characters
    size_t lineSize = strlen(line);
    bool endsWithAStar = false;
    for (unsigned pos = lineSize - 1; !endsWithAStar && pos < lineSize; --pos) {
      endsWithAStar = line[pos] == '*';
      if (!isspace(line[pos]))
        break;
    }
    if (endsWithAStar) {
      unsigned speedReading, widthReading;
      unsigned nmatch = sscanf(line, "%*u: %u.%*uGT/s, x%u", &speedReading, &widthReading);
      if (nmatch == 2) {
        *speed = speedReading;
        *width = widthReading;
        return true;
      }
    }
  }
  return false;
}

static void gpuinfo_amdgpu_populate_static_info(struct gpu_info *_gpu_info) {
  struct gpu_info_amdgpu *gpu_info =
    container_of(_gpu_info, struct gpu_info_amdgpu, base);
  struct gpuinfo_static_info *static_info = &gpu_info->base.static_info;
  bool info_query_success = false;
  struct amdgpu_gpu_info info;
  const char *name = NULL;

  RESET_ALL(static_info->valid);

  if (libdrm_amdgpu_handle && _amdgpu_get_marketing_name)
    name = _amdgpu_get_marketing_name(gpu_info->amdgpu_device);

  if (libdrm_amdgpu_handle && _amdgpu_query_gpu_info)
    info_query_success = !_amdgpu_query_gpu_info(gpu_info->amdgpu_device, &info);

  static_info->device_name[MAX_DEVICE_NAME - 1] = '\0';
  if (name && strlen(name)) {
    strncpy(static_info->device_name, name, MAX_DEVICE_NAME - 1);
    SET_VALID(gpuinfo_device_name_valid, static_info->valid);
  } else if (gpu_info->drmVersion->desc && strlen(gpu_info->drmVersion->desc)) {
    strncpy(static_info->device_name, gpu_info->drmVersion->desc, MAX_DEVICE_NAME - 1);
    SET_VALID(gpuinfo_device_name_valid, static_info->valid);

    if (info_query_success) {
      size_t len = strlen(static_info->device_name);
      assert(len < MAX_DEVICE_NAME);

      char *dst = static_info->device_name + len;
      size_t remaining_len = MAX_DEVICE_NAME - 1 - len;
      switch (info.family_id) {
#ifdef AMDGPU_FAMILY_SI
      case AMDGPU_FAMILY_SI:
        strncpy(dst, " (Hainan / Oland / Verde / Pitcairn / Tahiti)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_CI
      case AMDGPU_FAMILY_CI:
        strncpy(dst, " (Bonaire / Hawaii)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_KV
      case AMDGPU_FAMILY_KV:
        strncpy(dst, " (Kaveri / Kabini / Mullins)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_VI
      case AMDGPU_FAMILY_VI:
        strncpy(dst, " (Iceland / Tonga)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_CZ
      case AMDGPU_FAMILY_CZ:
        strncpy(dst, " (Carrizo / Stoney)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_AI
      case AMDGPU_FAMILY_AI:
        strncpy(dst, " (Vega10)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_RV
      case AMDGPU_FAMILY_RV:
        strncpy(dst, " (Raven)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_NV
      case AMDGPU_FAMILY_NV:
        strncpy(dst, " (Navi10)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_VGH
      case AMDGPU_FAMILY_VGH:
        strncpy(dst, " (Van Gogh)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_YC
      case AMDGPU_FAMILY_YC:
        strncpy(dst, " (Yellow Carp)", remaining_len);
        break;
#endif
      default:
        break;
      }
    }
  }

  // Retrieve infos from sysfs.

  // 1) Fan
  // If multiple fans are present, use the first one. Some hardware do not wire
  // the sensor for the second fan, or use the same value as the first fan.

  // Look for which fan to use (PWM or RPM)
  gpu_info->fanSpeedFILE = NULL;
  unsigned pwmIsEnabled;
  int NreadPatterns = readValueFromFileAt(gpu_info->hwmonFD, "pwm1_enable", "%u", &pwmIsEnabled);
  bool usePWMSensor = NreadPatterns == 1 && pwmIsEnabled > 0;

  bool useRPMSensor = false;
  if (!usePWMSensor) {
    unsigned rpmIsEnabled;
    NreadPatterns = readValueFromFileAt(gpu_info->hwmonFD, "fan1_enable", "%u", &rpmIsEnabled);
    useRPMSensor = NreadPatterns && rpmIsEnabled > 0;
  }
  // Either RPM or PWM or neither
  assert((useRPMSensor ^ usePWMSensor) || (!useRPMSensor && !usePWMSensor));
  if (usePWMSensor || useRPMSensor) {
    char *maxFanSpeedFile = usePWMSensor ? "pwm1_max" : "fan1_max";
    char *fanSensorFile = usePWMSensor ? "pwm1" : "fan1_input";
    unsigned maxSpeedVal;
    NreadPatterns = readValueFromFileAt(gpu_info->hwmonFD, maxFanSpeedFile, "%u", &maxSpeedVal);
    if (NreadPatterns == 1) {
      gpu_info->maxFanValue = maxSpeedVal;
      // Open the fan file for dynamic info gathering
      int fanSpeedFD = openat(gpu_info->hwmonFD, fanSensorFile, O_RDONLY);
      if (fanSpeedFD >= 0) {
        gpu_info->fanSpeedFILE = fdopen(fanSpeedFD, "r");
        if (!gpu_info->fanSpeedFILE)
          close(fanSpeedFD);
      }
    }
  }

  // Critical temparature
  // temp1_* files should always be the GPU die in millidegrees Celsius
  unsigned criticalTemp;
  NreadPatterns = readValueFromFileAt(gpu_info->hwmonFD, "temp1_crit", "%u", &criticalTemp);
  if (NreadPatterns == 1) {
    SET_GPUINFO_STATIC(static_info, temperature_slowdown_threshold, criticalTemp);
  }

  // Emergency/shutdown temparature
  unsigned emergemcyTemp;
  NreadPatterns = readValueFromFileAt(gpu_info->hwmonFD, "temp1_emergency", "%u", &emergemcyTemp);
  if (NreadPatterns == 1) {
    SET_GPUINFO_STATIC(static_info, temperature_shutdown_threshold, emergemcyTemp);
  }

  // PCIe max link width
  unsigned maxLinkWidth;
  NreadPatterns = readValueFromFileAt(gpu_info->sysfsFD, "max_link_width", "%u", &maxLinkWidth);
  if (NreadPatterns == 1) {
    SET_GPUINFO_STATIC(static_info, max_pcie_link_width, maxLinkWidth);
  }

  // PCIe max link speed
  // [max|current]_link_speed export the value as "x.y GT/s PCIe" where x.y is a float value.
  float maxLinkSpeedf;
  NreadPatterns = readValueFromFileAt(gpu_info->sysfsFD, "max_link_speed", "%f GT/s PCIe", &maxLinkSpeedf);
  if (NreadPatterns == 1 && GPUINFO_STATIC_FIELD_VALID(static_info, max_pcie_link_width)) {
    maxLinkSpeedf = floorf(maxLinkSpeedf);
    unsigned maxLinkSpeed = (unsigned)maxLinkSpeedf;
    unsigned pcieGen = pcieGenFromLinkSpeedAndWidth(maxLinkSpeed);
    if (pcieGen) {
      SET_GPUINFO_STATIC(static_info, max_pcie_gen, pcieGen);
    }
  }
  // Open current link speed
  gpu_info->PCIeDPM = NULL;
  int pcieDPMFD = openat(gpu_info->sysfsFD, "pp_dpm_pcie", O_RDONLY);
  if (pcieDPMFD) {
    gpu_info->PCIeDPM = fdopen(pcieDPMFD, "r");
  }

  // Open the PCIe bandwidth file for dynamic info gathering
  gpu_info->PCIeBW = NULL;
  int pcieBWFD = openat(gpu_info->sysfsFD, "pcie_bw", O_RDONLY);
  if (pcieBWFD) {
    gpu_info->PCIeBW = fdopen(pcieBWFD, "r");
  }

  // Open the power cap file for dynamic info gathering
  gpu_info->powerCap = NULL;
  int powerCapFD = openat(gpu_info->hwmonFD, "power1_cap", O_RDONLY);
  if (powerCapFD) {
    gpu_info->powerCap = fdopen(powerCapFD, "r");
  }
}

static void gpuinfo_amdgpu_refresh_dynamic_info(struct gpu_info *_gpu_info) {
  struct gpu_info_amdgpu *gpu_info =
    container_of(_gpu_info, struct gpu_info_amdgpu, base);
  struct gpuinfo_dynamic_info *dynamic_info = &gpu_info->base.dynamic_info;
  bool info_query_success = false;
  struct amdgpu_gpu_info info;
  uint32_t out32;

  RESET_ALL(dynamic_info->valid);

  if (libdrm_amdgpu_handle && _amdgpu_query_gpu_info)
    info_query_success = !_amdgpu_query_gpu_info(gpu_info->amdgpu_device, &info);

  // GPU current speed
  if (libdrm_amdgpu_handle && _amdgpu_query_sensor_info)
    last_libdrm_return_status =
        _amdgpu_query_sensor_info(gpu_info->amdgpu_device, AMDGPU_INFO_SENSOR_GFX_SCLK, sizeof(out32), &out32);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed, out32);
  }

  // GPU max speed
  if (info_query_success) {
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed_max, info.max_engine_clk / 1000);
  }

  // Memory current speed
  if (libdrm_amdgpu_handle && _amdgpu_query_sensor_info)
    last_libdrm_return_status =
        _amdgpu_query_sensor_info(gpu_info->amdgpu_device, AMDGPU_INFO_SENSOR_GFX_MCLK, sizeof(out32), &out32);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    SET_GPUINFO_DYNAMIC(dynamic_info, mem_clock_speed, out32);
  }

  // Memory max speed
  if (info_query_success) {
    SET_GPUINFO_DYNAMIC(dynamic_info, mem_clock_speed_max, info.max_memory_clk / 1000);
  }

  // Load
  if (libdrm_amdgpu_handle && _amdgpu_query_sensor_info)
    last_libdrm_return_status =
        _amdgpu_query_sensor_info(gpu_info->amdgpu_device, AMDGPU_INFO_SENSOR_GPU_LOAD, sizeof(out32), &out32);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_util_rate, out32);
  }

  // Memory usage
  struct drm_amdgpu_memory_info memory_info;
  if (libdrm_amdgpu_handle && _amdgpu_query_info)
    last_libdrm_return_status =
        _amdgpu_query_info(gpu_info->amdgpu_device, AMDGPU_INFO_MEMORY, sizeof(memory_info), &memory_info);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    // TODO: Determine if we want to include GTT (GPU accessible system memory)
    SET_GPUINFO_DYNAMIC(dynamic_info, total_memory, memory_info.vram.total_heap_size);
    SET_GPUINFO_DYNAMIC(dynamic_info, used_memory, memory_info.vram.heap_usage);
    SET_GPUINFO_DYNAMIC(dynamic_info, free_memory, memory_info.vram.usable_heap_size - dynamic_info->used_memory);
    SET_GPUINFO_DYNAMIC(dynamic_info, mem_util_rate,
                        (dynamic_info->total_memory - dynamic_info->free_memory) * 100 / dynamic_info->total_memory);
  }

  // GPU temperature
  if (libdrm_amdgpu_handle && _amdgpu_query_sensor_info)
    last_libdrm_return_status =
        _amdgpu_query_sensor_info(gpu_info->amdgpu_device, AMDGPU_INFO_SENSOR_GPU_TEMP, sizeof(out32), &out32);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_temp, out32 / 1000);
  }

  // Fan speed
  if (gpu_info->fanSpeedFILE) {
    unsigned currentFanSpeed;
    int patternsMatched = rewindAndReadPattern(gpu_info->fanSpeedFILE, "%u", &currentFanSpeed);
    if (patternsMatched == 1) {
      SET_GPUINFO_DYNAMIC(dynamic_info, fan_speed, currentFanSpeed * 100 / gpu_info->maxFanValue);
    }
  }

  // Device power usage
  if (libdrm_amdgpu_handle && _amdgpu_query_sensor_info)
    last_libdrm_return_status =
        _amdgpu_query_sensor_info(gpu_info->amdgpu_device, AMDGPU_INFO_SENSOR_GPU_AVG_POWER, sizeof(out32), &out32);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    SET_GPUINFO_DYNAMIC(dynamic_info, power_draw, out32 * 1000);
  }

  // Current PCIe link used
  if (gpu_info->PCIeDPM) {
    unsigned currentLinkSpeed = 0;
    unsigned currentLinkWidth = 0;
    if (getGenAndWidthFromPP_DPM_PCIE(gpu_info->PCIeDPM, &currentLinkSpeed, &currentLinkWidth)) {
      SET_GPUINFO_DYNAMIC(dynamic_info, pcie_link_width, currentLinkWidth);
      unsigned pcieGen = pcieGenFromLinkSpeedAndWidth(currentLinkSpeed);
      if (pcieGen) {
        SET_GPUINFO_DYNAMIC(dynamic_info, pcie_link_gen, pcieGen);
      }
    }
  }
  // PCIe bandwidth
  if (gpu_info->PCIeBW) {
    // According to https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/pm/amdgpu_pm.c, under the pcie_bw
    // section, we should be able to read the number of packets received and sent by the GPU and get the maximum payload
    // size during the last second. This is untested but should work when the file is populated by the driver.
    uint64_t received, transmitted;
    int maxPayloadSize;
    int NreadPatterns = rewindAndReadPattern(gpu_info->PCIeBW, "%" SCNu64 " %" SCNu64 " %i", &received, &transmitted, &maxPayloadSize);
    if (NreadPatterns == 3) {
      received *= maxPayloadSize;
      transmitted *= maxPayloadSize;
      SET_GPUINFO_DYNAMIC(dynamic_info, pcie_rx, received);
      SET_GPUINFO_DYNAMIC(dynamic_info, pcie_tx, transmitted);
    }
  }

  if (gpu_info->powerCap) {
    // The power cap in microwatts
    unsigned powerCap;
    int NreadPatterns = rewindAndReadPattern(gpu_info->powerCap, "%u", &powerCap);
    if (NreadPatterns == 1) {
      SET_GPUINFO_DYNAMIC(dynamic_info, power_draw_max, powerCap / 1000);
    }
  }
}

static bool extract_kv(char *buf, char **key, char **val) {
  char *p = buf;

  p = index(buf, ':');
  if (!p || p == buf)
    return false;
  *p = '\0';

  while (*++p && isspace(*p))
    ;
  if (!*p)
    return false;

  *key = buf;
  *val = p;

  return true;
}

static inline unsigned busy_usage_from_time_usage_round(uint64_t current_use_ns, uint64_t previous_use_ns,
                                                        uint64_t time_between_measurement) {
  return ((current_use_ns - previous_use_ns) * UINT64_C(100) + time_between_measurement / UINT64_C(2)) /
         time_between_measurement;
}

static const char pdev_old[] = "pdev";
static const char pdev_new[] = "drm-pdev";
static const char vram_old[] = "vram mem";
static const char vram_new[] = "drm-memory-vram";
static const char gfx_old[] = "gfx";
static const char gfx_new[] = "drm-engine-gfx";
static const char compute_old[] = "compute";
static const char compute_new[] = "drm-engine-compute";
static const char dec_old[] = "dec";
static const char dec_new[] = "drm-engine-dec";
static const char enc_old[] = "enc";
static const char enc_new[] = "drm-engine-enc";
static const char client_id[] = "drm-client-id";

static bool parse_drm_fdinfo_amd(struct gpu_info *info, FILE *fdinfo_file, struct gpu_process *process_info) {
  struct gpu_info_amdgpu *gpu_info = container_of(info, struct gpu_info_amdgpu, base);
  static char *line = NULL;
  static size_t line_buf_size = 0;
  ssize_t count = 0;

  bool client_id_set = false;
  unsigned cid;
  nvtop_time current_time;
  nvtop_get_current_time(&current_time);

  while ((count = getline(&line, &line_buf_size, fdinfo_file)) != -1) {
    char *key, *val;
    // Get rid of the newline if present
    if (line[count - 1] == '\n') {
      line[--count] = '\0';
    }

    if (!extract_kv(line, &key, &val))
      continue;

    // see drivers/gpu/drm/amd/amdgpu/amdgpu_fdinfo.c amdgpu_show_fdinfo()
    if (!strcmp(key, pdev_old) || !strcmp(key, pdev_new)) {
      if (strcmp(val, gpu_info->pdev)) {
        return false;
      }
    } else if (!strcmp(key, client_id)) {
      // Client id is a unique identifier. From the DRM documentation "Unique value relating to the open DRM
      // file descriptor used to distinguish duplicated and shared file descriptors. Conceptually the value should map
      // 1:1 to the in kernel representation of struct drm_file instances."
      // This information is available for the AMDGPU driver shipping with
      // the kernel >= 5.19. We still have to use the kcmp syscall to
      // distinguish duplicated file descriptors for older kernels.
      char *endptr;
      cid = strtoul(val, &endptr, 10);
      if (*endptr)
        continue;
      client_id_set = true;
    } else if (!strcmp(key, vram_old) || !strcmp(key, vram_new)) {
      // TODO: do we count "gtt mem" too?
      unsigned long mem_int;
      char *endptr;

      mem_int = strtoul(val, &endptr, 10);
      if (endptr == val || (strcmp(endptr, " kB") && strcmp(endptr, " KiB")))
        continue;

      SET_GPUINFO_PROCESS(process_info, gpu_memory_usage, mem_int * 1024);
    } else {
      bool is_gfx_old = !strncmp(key, gfx_old, sizeof(gfx_old) - 1);
      bool is_compute_old = !strncmp(key, compute_old, sizeof(compute_old) - 1);
      bool is_dec_old = !strncmp(key, dec_old, sizeof(dec_old) - 1);
      bool is_enc_old = !strncmp(key, enc_old, sizeof(enc_old) - 1);

      bool is_gfx_new = !strncmp(key, gfx_new, sizeof(gfx_new) - 1);
      bool is_dec_new = !strncmp(key, dec_new, sizeof(dec_new) - 1);
      bool is_enc_new = !strncmp(key, enc_new, sizeof(enc_new) - 1);
      bool is_compute_new = !strncmp(key, compute_new, sizeof(compute_new) - 1);

      if (is_gfx_old || is_compute_old || is_dec_old || is_enc_old) {
        // The old interface exposes a usage percentage with an unknown update interval
        unsigned int usage_percent_int;
        char *key_off, *endptr;
        double usage_percent;

        if (is_gfx_old)
          key_off = key + sizeof(gfx_old) - 1;
        else if (is_compute_old)
          key_off = key + sizeof(compute_old) - 1;
        else if (is_dec_old)
          key_off = key + sizeof(dec_old) - 1;
        else if (is_enc_old)
          key_off = key + sizeof(enc_old) - 1;
        else
          continue;

        // The prefix should be followed by a number and only a number
        if (!*key_off)
          continue;
        strtoul(key_off, &endptr, 10);
        if (*endptr)
          continue;

        usage_percent_int = (unsigned int)(usage_percent = round(strtod(val, &endptr)));
        if (endptr == val || strcmp(endptr, "%"))
          continue;

        if (is_gfx_old) {
          SET_GPUINFO_PROCESS(process_info, gpu_usage, process_info->gpu_usage + usage_percent_int);
        } else if (is_compute_old) {
          process_info->type = gpu_process_compute;
          SET_GPUINFO_PROCESS(process_info, gpu_usage, process_info->gpu_usage + usage_percent_int);
        } else if (is_dec_old) {
          SET_GPUINFO_PROCESS(process_info, decode_usage, process_info->decode_usage + usage_percent_int);
        } else if (is_enc_old) {
          SET_GPUINFO_PROCESS(process_info, encode_usage, process_info->encode_usage + usage_percent_int);
        }
      } else if (is_gfx_new || is_compute_new || is_dec_new || is_enc_new) {
        char *endptr;
        uint64_t time_spent = strtoull(val, &endptr, 10);
        if (endptr == val || strcmp(endptr, " ns"))
          continue;

        if (is_gfx_new) {
          SET_GPUINFO_PROCESS(process_info, gfx_engine_used, time_spent);
        } else if (is_compute_new) {
          process_info->type = gpu_process_compute;
          SET_GPUINFO_PROCESS(process_info, compute_engine_used, time_spent);
        } else if (is_enc_new) {
          SET_GPUINFO_PROCESS(process_info, enc_engine_used, time_spent);
        } else if (is_dec_new) {
          SET_GPUINFO_PROCESS(process_info, dec_engine_used, time_spent);
        }
      }
    }
  }

  // The AMDGPU fdinfo interface in kernels >=5.19 is way nicer; it provides the
  // cumulative GPU engines (e.g., gfx, enc, dec) usage in nanoseconds.
  // Previously, we displayed the usage provided in fdinfo by the kernel/driver
  // which uses an internal update interval. Now, we can compute an accurate
  // busy percentage since the last measurement.
  if (client_id_set) {
    struct amdgpu_process_info_cache *cache_entry;
    struct unique_cache_id ucid = {.client_id = cid, .pid = process_info->pid};
    HASH_FIND_CLIENT(gpu_info->last_update_process_cache, &ucid, cache_entry);
    if (cache_entry) {
      uint64_t time_elapsed = nvtop_difftime_u64(cache_entry->last_measurement_tstamp, current_time);
      HASH_DEL(gpu_info->last_update_process_cache, cache_entry);
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, gfx_engine_used) &&
          AMDGPU_CACHE_FIELD_VALID(cache_entry, gfx_engine_used) &&
          // In some rare occasions, the gfx engine usage reported by the driver is lowering (might be a driver bug)
          process_info->gfx_engine_used >= cache_entry->gfx_engine_used &&
          process_info->gfx_engine_used - cache_entry->gfx_engine_used <= time_elapsed) {
        SET_GPUINFO_PROCESS(process_info, gpu_usage,
                            busy_usage_from_time_usage_round(process_info->gfx_engine_used,
                                                             cache_entry->gfx_engine_used, time_elapsed));
      }
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, compute_engine_used) &&
          AMDGPU_CACHE_FIELD_VALID(cache_entry, compute_engine_used) &&
          process_info->compute_engine_used >= cache_entry->compute_engine_used &&
          process_info->compute_engine_used - cache_entry->compute_engine_used <= time_elapsed) {
        SET_GPUINFO_PROCESS(process_info, gpu_usage,
                            busy_usage_from_time_usage_round(process_info->compute_engine_used,
                                                             cache_entry->compute_engine_used, time_elapsed));
      }
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, dec_engine_used) &&
          AMDGPU_CACHE_FIELD_VALID(cache_entry, dec_engine_used) &&
          process_info->dec_engine_used >= cache_entry->dec_engine_used &&
          process_info->dec_engine_used - cache_entry->dec_engine_used <= time_elapsed) {
        SET_GPUINFO_PROCESS(process_info, decode_usage,
                            busy_usage_from_time_usage_round(process_info->dec_engine_used,
                                                             cache_entry->dec_engine_used, time_elapsed));
      }
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, enc_engine_used) &&
          AMDGPU_CACHE_FIELD_VALID(cache_entry, enc_engine_used) &&
          process_info->enc_engine_used >= cache_entry->enc_engine_used &&
          process_info->enc_engine_used - cache_entry->enc_engine_used <= time_elapsed) {
        SET_GPUINFO_PROCESS(process_info, encode_usage,
                            busy_usage_from_time_usage_round(process_info->enc_engine_used,
                                                             cache_entry->enc_engine_used, time_elapsed));
      }
    } else {
      cache_entry = calloc(1, sizeof(*cache_entry));
      if (!cache_entry)
        goto parse_fdinfo_exit;
      cache_entry->client_id.client_id = cid;
      cache_entry->client_id.pid = process_info->pid;
    }

#ifndef NDEBUG
    // We should only process one fdinfo entry per client id per update
    struct amdgpu_process_info_cache *cache_entry_check;
    HASH_FIND_CLIENT(gpu_info->current_update_process_cache, &cid, cache_entry_check);
    assert(!cache_entry_check && "We should not be processing a client id twice per update");
#endif

    // Store this measurement data
    RESET_ALL(cache_entry->valid);
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, gfx_engine_used))
      SET_AMDGPU_CACHE(cache_entry, gfx_engine_used, process_info->gfx_engine_used);
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, compute_engine_used))
      SET_AMDGPU_CACHE(cache_entry, compute_engine_used, process_info->compute_engine_used);
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, dec_engine_used))
      SET_AMDGPU_CACHE(cache_entry, dec_engine_used, process_info->dec_engine_used);
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, enc_engine_used))
      SET_AMDGPU_CACHE(cache_entry, enc_engine_used, process_info->enc_engine_used);

    cache_entry->last_measurement_tstamp = current_time;
    HASH_ADD_CLIENT(gpu_info->current_update_process_cache, cache_entry);
  }

parse_fdinfo_exit:
  return true;
}

static void swap_process_cache_for_next_update(struct gpu_info_amdgpu *gpu_info) {
  // Free old cache data and set the cache for the next update
  if (gpu_info->last_update_process_cache) {
    struct amdgpu_process_info_cache *cache_entry, *tmp;
    HASH_ITER(hh, gpu_info->last_update_process_cache, cache_entry, tmp) {
      HASH_DEL(gpu_info->last_update_process_cache, cache_entry);
      free(cache_entry);
    }
  }
  gpu_info->last_update_process_cache = gpu_info->current_update_process_cache;
  gpu_info->current_update_process_cache = NULL;
}

static void gpuinfo_amdgpu_get_running_processes(struct gpu_info *_gpu_info) {
  // For AMDGPU, we register a fdinfo callback that will fill the gpu_process datastructure of the gpu_info structure
  // for us. This avoids going through /proc multiple times per update for multiple GPUs.
  struct gpu_info_amdgpu *gpu_info = container_of(_gpu_info, struct gpu_info_amdgpu, base);
  swap_process_cache_for_next_update(gpu_info);
}
