/* Host-only types for compiling the exact firmware path classifier. */
#ifndef BOOTGATE_TEST_H_
#define BOOTGATE_TEST_H_

#include <stdint.h>
#include <stddef.h>

typedef void      VOID;
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint64_t  UINTN;
typedef uint16_t  CHAR16;
typedef char      CHAR8;
typedef uint8_t   BOOLEAN;

typedef struct {
  UINT8 Type;
  UINT8 SubType;
  UINT8 Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

#define CONST const
#define STATIC static
#define IN
#define TRUE  1
#define FALSE 0

#define MEDIA_DEVICE_PATH              0x04
#define MEDIA_FILEPATH_DP               0x04
#define END_DEVICE_PATH_TYPE            0x7f
#define END_ENTIRE_DEVICE_PATH_SUBTYPE  0xff

BOOLEAN HvIsWindowsBootPath (IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath);

#endif
