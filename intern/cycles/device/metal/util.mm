/* SPDX-FileCopyrightText: 2021-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_METAL

#  include "device/metal/util.h"
#  include "device/metal/device_impl.h"
#  include "util/md5.h"
#  include "util/path.h"
#  include "util/string.h"
#  include "util/time.h"

#  include <IOKit/IOKitLib.h>
#  include <cstdlib>
#  include <ctime>
#  include <pwd.h>
#  include <sys/shm.h>

CCL_NAMESPACE_BEGIN

/* Comment this out to test workaround for getting gpuAddress and gpuResourceID on macOS < 13.0. */
#  define CYCLES_USE_TIER2D_BINDLESS

string MetalInfo::get_device_name(id<MTLDevice> device)
{
  string device_name = [device.name UTF8String];

  /* Append the GPU core count so we can distinguish between GPU variants in benchmarks. */
  int gpu_core_count = get_apple_gpu_core_count(device);
  device_name += string_printf(gpu_core_count ? " (GPU - %d cores)" : " (GPU)", gpu_core_count);

  return device_name;
}

int MetalInfo::get_apple_gpu_core_count(id<MTLDevice> device)
{
  int core_count = 0;
  if (@available(macos 12.0, *)) {
    io_service_t gpu_service = IOServiceGetMatchingService(
        kIOMainPortDefault, IORegistryEntryIDMatching(device.registryID));
    if (CFNumberRef numberRef = (CFNumberRef)IORegistryEntryCreateCFProperty(
            gpu_service, CFSTR("gpu-core-count"), nullptr, 0))
    {
      if (CFGetTypeID(numberRef) == CFNumberGetTypeID()) {
        CFNumberGetValue(numberRef, kCFNumberSInt32Type, &core_count);
      }
      CFRelease(numberRef);
    }
  }
  return core_count;
}

static bool is_word_char(const char ch)
{
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

/* Parses the Apple Silicon generation from a GPU name, e.g. 2 for "Apple M2 Ultra". The generation
 * token must be a standalone "M<digits>" word so that other occurrences of 'M' in a device name
 * (e.g. a trailing "5700M") never match. Returns 0 if no generation token is present. */
static int parse_apple_gpu_generation(const char *device_name)
{
  for (const char *c = device_name; *c; c++) {
    if (*c != 'M') {
      continue;
    }
    if (c != device_name && c[-1] != ' ' && c[-1] != '(') {
      continue;
    }
    if (!(c[1] >= '0' && c[1] <= '9')) {
      continue;
    }
    char *token_end = nullptr;
    const long generation = strtol(c + 1, &token_end, 10);
    if (!is_word_char(*token_end)) {
      return int(generation);
    }
  }
  return 0;
}

AppleGPUArchitecture MetalInfo::get_apple_gpu_architecture(id<MTLDevice> device)
{
  const char *device_name = [device.name UTF8String];
  switch (parse_apple_gpu_generation(device_name)) {
    case 1:
      return APPLE_M1;
    case 2:
      return get_apple_gpu_core_count(device) <= 10 ? APPLE_M2 : APPLE_M2_BIG;
    case 3:
      return APPLE_M3;
    case 4:
      return APPLE_M4;
    case 5:
      return APPLE_M5;
    default:
      break;
  }

  /* Either the name carries no generation token, or the generation is newer than the ones handled
   * above. Use the GPU family as a secondary signal where possible: per the Metal Feature Set
   * Tables, Apple9 is M3-or-newer, Apple8 is the M2 generation and Apple7 is the M1 generation.
   * `supportsFamily:` returns NO for family values unknown to the current OS. */
#  if defined(MAC_OS_VERSION_14_0)
  if ([device supportsFamily:MTLGPUFamilyApple9]) {
    /* At least M3-class: APPLE_UNKNOWN inherits the most recent defaults. */
    return APPLE_UNKNOWN;
  }
#  endif
#  if defined(MAC_OS_VERSION_13_0)
  if ([device supportsFamily:MTLGPUFamilyApple8]) {
    return get_apple_gpu_core_count(device) <= 10 ? APPLE_M2 : APPLE_M2_BIG;
  }
#  endif
  if ([device supportsFamily:MTLGPUFamilyApple7]) {
    return APPLE_M1;
  }
  return APPLE_UNKNOWN;
}

int MetalInfo::optimal_sort_partition_elements()
{
  if (auto *str = getenv("CYCLES_METAL_SORT_PARTITION_ELEMENTS")) {
    return atoi(str);
  }

  /* On M1 and M2 GPUs, we see better cache utilization if we partition the active indices before
   * sorting each partition by material. Partitioning into chunks of 65536 elements results in an
   * overall render time speedup of up to 15%. */

  return 65536;
}

const vector<id<MTLDevice>> &MetalInfo::get_usable_devices()
{
  static vector<id<MTLDevice>> usable_devices;
  static bool already_enumerated = false;

  if (already_enumerated) {
    return usable_devices;
  }

  metal_printf("Usable Metal devices:");
  for (id<MTLDevice> device in MTLCopyAllDevices()) {
    string device_name = get_device_name(device);
    bool usable = false;

    if (@available(macos 12.2, *)) {
      const char *device_name_char = [device.name UTF8String];
      if (!(strstr(device_name_char, "Intel") || strstr(device_name_char, "AMD")) &&
          strstr(device_name_char, "Apple"))
      {
        /* TODO: Implement a better way to identify device vendor instead of relying on name. */
        /* We only support Apple Silicon GPUs which all have unified memory, but explicitly check
         * just in case it ever changes. */
        usable = [device hasUnifiedMemory];
      }
    }

    if (usable) {
      metal_printf("- %s", device_name.c_str());
      [device retain];
      usable_devices.push_back(device);
    }
    else {
      metal_printf("  (skipping \"%s\")", device_name.c_str());
    }
  }
  if (usable_devices.empty()) {
    metal_printf("   No usable Metal devices found");
  }
  already_enumerated = true;

  return usable_devices;
}

struct GPUAddressHelper {
  id<MTLBuffer> resource_buffer = nil;
  id<MTLArgumentEncoder> address_encoder = nil;

  /* One time setup of arg encoder. */
  void init(id<MTLDevice> device)
  {
    if (resource_buffer) {
      /* No setup required - already initialised. */
      return;
    }

#  ifdef CYCLES_USE_TIER2D_BINDLESS
    if (@available(macos 13.0, *)) {
      /* No setup required - there's an API now! */
      return;
    }
#  endif

    /* Setup a tiny buffer to encode the GPU address / resourceID into. */
    resource_buffer = [device newBufferWithLength:8 options:MTLResourceStorageModeShared];

    /* Create an encoder to extract a gpuAddress from a MTLBuffer. */
    MTLArgumentDescriptor *encoder_params = [[MTLArgumentDescriptor alloc] init];
    encoder_params.arrayLength = 1;
    encoder_params.access = MTLBindingAccessReadWrite;
    encoder_params.dataType = MTLDataTypePointer;
    address_encoder = [device newArgumentEncoderWithArguments:@[ encoder_params ]];
    [address_encoder setArgumentBuffer:resource_buffer offset:0];
  };

  uint64_t gpuAddress(id<MTLBuffer> buffer)
  {
#  ifdef CYCLES_USE_TIER2D_BINDLESS
    if (@available(macos 13.0, *)) {
      return buffer.gpuAddress;
    }
#  endif
    [address_encoder setBuffer:buffer offset:0 atIndex:0];
    return *(uint64_t *)[resource_buffer contents];
  }

  uint64_t gpuResourceID(id<MTLTexture> texture)
  {
#  ifdef CYCLES_USE_TIER2D_BINDLESS
    if (@available(macos 13.0, *)) {
      MTLResourceID resourceID = texture.gpuResourceID;
      return (uint64_t &)resourceID;
    }
#  endif
    [address_encoder setTexture:texture atIndex:0];
    return *(uint64_t *)[resource_buffer contents];
  }

  uint64_t gpuResourceID(id<MTLAccelerationStructure> accel_struct)
  {
#  ifdef CYCLES_USE_TIER2D_BINDLESS
    if (@available(macos 13.0, *)) {
      MTLResourceID resourceID = accel_struct.gpuResourceID;
      return (uint64_t &)resourceID;
    }
#  endif
    [address_encoder setAccelerationStructure:accel_struct atIndex:0];
    return *(uint64_t *)[resource_buffer contents];
  }

  uint64_t gpuResourceID(id<MTLIntersectionFunctionTable> ift)
  {
#  ifdef CYCLES_USE_TIER2D_BINDLESS
    if (@available(macos 13.0, *)) {
      MTLResourceID resourceID = ift.gpuResourceID;
      return (uint64_t &)resourceID;
    }
#  endif
    [address_encoder setIntersectionFunctionTable:ift atIndex:0];
    return *(uint64_t *)[resource_buffer contents];
  }
};

GPUAddressHelper g_gpu_address_helper;

void metal_gpu_address_helper_init(id<MTLDevice> device)
{
  g_gpu_address_helper.init(device);
}

uint64_t metal_gpuAddress(id<MTLBuffer> buffer)
{
  return g_gpu_address_helper.gpuAddress(buffer);
}

uint64_t metal_gpuResourceID(id<MTLTexture> texture)
{
  return g_gpu_address_helper.gpuResourceID(texture);
}

uint64_t metal_gpuResourceID(id<MTLAccelerationStructure> accel_struct)
{
  return g_gpu_address_helper.gpuResourceID(accel_struct);
}

uint64_t metal_gpuResourceID(id<MTLIntersectionFunctionTable> ift)
{
  return g_gpu_address_helper.gpuResourceID(ift);
}

CCL_NAMESPACE_END

#endif /* WITH_METAL */
